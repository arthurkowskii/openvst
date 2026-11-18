// SPDX-License-Identifier: AGPL-3.0-only
#include "ModuleUI.h"
#include <algorithm>
#include <cmath>
#include <map>

namespace openvst {
namespace {

constexpr int kMaxNodes = 128;
constexpr int kMaxDepth = 8;
const juce::Colour kAccent(0xffd9a441);

juce::Font uiFont(float size) { return juce::Font(juce::FontOptions(size)); }

struct Node {
    enum class Type { column, row, group, label, knob, slider, toggle, value, spacer };
    Type type = Type::column;
    std::vector<std::unique_ptr<Node>> children;
    int param = -1;
    juce::String label, text, unit, title;
    float min = 0.0f, max = 1.0f;
    int precision = 2;
    int gap = 6, padding = 8;
    int width = -1, height = -1; // -1 auto, 0 fill, >0 fixed
    int spacer = 8;
};

bool onlyKeys(const juce::DynamicObject& o, std::initializer_list<const char*> allowed, juce::String& error) {
    for (const auto& entry : o.getProperties()) {
        const juce::String key = entry.name.toString();
        bool ok = false;
        for (const char* a : allowed) if (key == a) { ok = true; break; }
        if (!ok) { error = "unknown key: " + key; return false; }
    }
    return true;
}

bool intValue(const juce::var& v, int lo, int hi, int& out, const char* name, juce::String& error) {
    if (!v.isInt() || v.isBool()) { error = juce::String(name) + " must be an integer"; return false; }
    const int n = (int)v;
    if (n < lo || n > hi) { error = juce::String(name) + " out of range"; return false; }
    out = n;
    return true;
}

bool parseSize(const juce::var& v, int& out, juce::String& error) {
    if (v.isString()) {
        const juce::String s = v.toString();
        if (s == "auto") { out = -1; return true; }
        if (s == "fill") { out = 0; return true; }
        error = "width/height must be auto, fill or 24..400";
        return false;
    }
    int n = 0;
    if (!intValue(v, 24, 400, n, "size", error)) return false;
    out = n;
    return true;
}

bool parseNode(const juce::var& v, int depth, int& count, Processor& processor,
               std::map<juce::String, int>& bindings, Node& node, juce::String& error) {
    if (++count > kMaxNodes) { error = "too many nodes"; return false; }
    if (depth > kMaxDepth) { error = "layout too deep"; return false; }
    auto* o = v.getDynamicObject();
    if (o == nullptr) { error = "node must be an object"; return false; }
    const auto typeVar = v["type"];
    if (!typeVar.isString()) { error = "node type missing"; return false; }
    const juce::String type = typeVar.toString();

    const bool container = type == "column" || type == "row" || type == "group";
    if (container) {
        if (!onlyKeys(*o, {"type", "children", "gap", "padding", "width", "height", "visible", "title"}, error)) return false;
        node.type = type == "column" ? Node::Type::column : type == "row" ? Node::Type::row : Node::Type::group;
        if (type == "group") {
            const auto t = v["title"];
            if (!t.isString() || t.toString().isEmpty() || t.toString().length() > 64) { error = "group title invalid"; return false; }
            node.title = t.toString();
        }
        const auto gap = v["gap"]; if (!gap.isVoid() && !intValue(gap, 0, 32, node.gap, "gap", error)) return false;
        const auto pad = v["padding"]; if (!pad.isVoid() && !intValue(pad, 0, 32, node.padding, "padding", error)) return false;
    } else if (type == "knob" || type == "slider") {
        if (!onlyKeys(*o, {"type", "param", "label", "size", "unit", "width", "height", "visible"}, error)) return false;
        node.type = type == "knob" ? Node::Type::knob : Node::Type::slider;
    } else if (type == "toggle") {
        if (!onlyKeys(*o, {"type", "param", "label", "width", "height", "visible"}, error)) return false;
        node.type = Node::Type::toggle;
    } else if (type == "value") {
        if (!onlyKeys(*o, {"type", "param", "label", "precision", "width", "height", "visible"}, error)) return false;
        node.type = Node::Type::value;
    } else if (type == "label") {
        if (!onlyKeys(*o, {"type", "text", "width", "height", "visible"}, error)) return false;
        node.type = Node::Type::label;
        const auto t = v["text"];
        if (!t.isString() || t.toString().isEmpty() || t.toString().length() > 120) { error = "label text invalid"; return false; }
        node.text = t.toString();
    } else if (type == "spacer") {
        if (!onlyKeys(*o, {"type", "size", "width", "height", "visible"}, error)) return false;
        node.type = Node::Type::spacer;
        const auto s = v["size"];
        if (!s.isVoid() && !intValue(s, 0, 64, node.spacer, "spacer size", error)) return false;
    } else {
        error = "unknown node type: " + type;
        return false;
    }

    const auto visible = v["visible"];
    if (!visible.isVoid() && !visible.isBool()) { error = "visible must be a boolean"; return false; }
    if (visible.isBool() && !(bool)visible) {
        // A hidden node is still validated above; it simply contributes no
        // component and no size.
        node.type = Node::Type::spacer;
        node.spacer = 0;
        return true;
    }
    const auto w = v["width"]; if (!w.isVoid() && !parseSize(w, node.width, error)) return false;
    const auto h = v["height"]; if (!h.isVoid() && !parseSize(h, node.height, error)) return false;

    if (node.type == Node::Type::knob || node.type == Node::Type::slider
        || node.type == Node::Type::toggle || node.type == Node::Type::value) {
        const auto pv = v["param"];
        if (!pv.isString() || pv.toString().isEmpty() || pv.toString().length() > 120) { error = "param binding missing"; return false; }
        const juce::String paramID = pv.toString();
        const ModuleSpec& m = processor.module();
        const ParamSpec* found = nullptr;
        for (int i = 0; i < (int)m.params.size(); ++i)
            if (paramID == juce::String(m.params[(std::size_t)i].id.c_str())) { found = &m.params[(std::size_t)i]; node.param = i; break; }
        if (found == nullptr) { error = "param not declared by the active module: " + paramID; return false; }
        if (!found->unit.empty()) node.unit = juce::String(found->unit.c_str());
        if (++bindings[paramID] > 2) { error = "param bound more than twice: " + paramID; return false; }
        node.min = found->min; node.max = found->max;
        if (node.type == Node::Type::toggle && (found->min < 0.0f || found->max > 1.0f)) {
            error = "toggle requires a 0..1 parameter";
            return false;
        }
        const auto lv = v["label"];
        if (!lv.isVoid()) {
            if (!lv.isString() || lv.toString().length() > 120) { error = "label invalid"; return false; }
            node.label = lv.toString();
        } else {
            node.label = juce::String(found->name.c_str());
        }
        if (node.type == Node::Type::knob || node.type == Node::Type::slider) {
            const auto uv = v["unit"];
            if (!uv.isVoid()) {
                if (!uv.isString() || uv.toString().length() > 16) { error = "unit invalid"; return false; }
                node.unit = uv.toString();
            }
            const auto sv = v["size"];
            if (!sv.isVoid()) {
                if (!sv.isString()) { error = "size invalid"; return false; }
                const auto s = sv.toString();
                if (s != "small" && s != "medium" && s != "large") { error = "size must be small, medium or large"; return false; }
                node.text = s;
            } else {
                node.text = "medium";
            }
        }
        if (node.type == Node::Type::value) {
            const auto pv2 = v["precision"];
            if (!pv2.isVoid() && !intValue(pv2, 0, 6, node.precision, "precision", error)) return false;
        }
    }

    if (container) {
        const auto childrenVar = v["children"];
        if (!childrenVar.isArray() || childrenVar.size() > kMaxNodes) { error = "children must be a bounded array"; return false; }
        for (const auto& child : *childrenVar.getArray()) {
            auto sub = std::make_unique<Node>();
            if (!parseNode(child, depth + 1, count, processor, bindings, *sub, error)) return false;
            node.children.push_back(std::move(sub));
        }
        if (node.children.empty() && node.type != Node::Type::group) { error = "container has no visible children"; return false; }
    }
    return true;
}

class UIItem : public juce::Component {
public:
    int preferredWidth() const { return fixedW > 0 ? fixedW : contentWidth(); }
    int preferredHeight() const { return fixedH > 0 ? fixedH : contentHeight(); }
    virtual void refreshValue() {}
    int fixedW = -1, fixedH = -1, fillW = 0;
protected:
    virtual int contentWidth() const = 0;
    virtual int contentHeight() const = 0;
};

class ContainerItem final : public UIItem {
public:
    ContainerItem(bool isRow, int gapIn, int paddingIn, juce::String titleIn, bool groupIn)
        : row(isRow), gap(gapIn), padding(paddingIn), title(std::move(titleIn)), group(groupIn) {}
    void add(std::unique_ptr<UIItem> child) { kids.push_back(std::move(child)); addAndMakeVisible(*kids.back()); }
    void refreshValue() override { for (auto& k : kids) k->refreshValue(); }
    void paint(juce::Graphics& g) override {
        if (!group) return;
        auto r = getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(juce::Colour(0x18ffffff)); g.fillRoundedRectangle(r, 6.0f);
        g.setColour(juce::Colour(0x30ffffff)); g.drawRoundedRectangle(r, 6.0f, 1.0f);
        g.setColour(juce::Colours::white.withAlpha(0.75f));
        g.setFont(uiFont(12.0f));
        g.drawText(title, getLocalBounds().reduced(padding, 0).removeFromTop(20), juce::Justification::centredLeft, true);
    }
    void resized() override {
        auto content = getLocalBounds().reduced(padding);
        if (group) content.removeFromTop(22);
        if (kids.empty()) return;
        if (!row) {
            int y = content.getY();
            for (auto& k : kids) {
                const int h = k->fixedH > 0 ? k->fixedH : k->preferredHeight();
                const int w = k->fixedW > 0 ? k->fixedW : content.getWidth();
                k->setBounds(content.getX(), y, w, h);
                y += h + gap;
            }
        } else {
            const int n = (int)kids.size();
            const int usable = juce::jmax(0, content.getWidth() - gap * (n - 1));
            int x = content.getX();
            for (int i = 0; i < n; ++i) {
                auto& k = *kids[(std::size_t)i];
                int w = k.fixedW > 0 ? k.fixedW : usable / n;
                if (i == n - 1) w = content.getRight() - x;
                k.setBounds(x, content.getY(), w, k.fixedH > 0 ? k.fixedH : content.getHeight());
                x += w + gap;
            }
        }
    }
protected:
    int contentWidth() const override {
        int w = 0;
        if (row) { for (auto& k : kids) w += k->preferredWidth() + gap; if (w > 0) w -= gap; }
        else { for (auto& k : kids) w = juce::jmax(w, k->preferredWidth()); }
        return w + padding * 2;
    }
    int contentHeight() const override {
        int h = 0;
        if (row) { for (auto& k : kids) h = juce::jmax(h, k->preferredHeight()); }
        else { for (auto& k : kids) h += k->preferredHeight() + gap; if (h > 0) h -= gap; }
        return h + padding * 2 + (group ? 22 : 0);
    }
private:
    std::vector<std::unique_ptr<UIItem>> kids;
    bool row, group;
    int gap, padding;
    juce::String title;
};

class KnobItem final : public UIItem {
public:
    KnobItem(Processor& pIn, int paramIn, juce::String label, juce::String unit, float min, float max, juce::String size)
        : p(pIn), param(paramIn) {
        addAndMakeVisible(name); addAndMakeVisible(slider);
        name.setText(std::move(label), juce::dontSendNotification);
        name.setJustificationType(juce::Justification::centred);
        name.setFont(uiFont(12.0f));
        slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 66, 18);
        slider.setRange(min, max, 0.001);
        if (unit.isNotEmpty()) slider.setTextValueSuffix(" " + unit);
        slider.setColour(juce::Slider::rotarySliderFillColourId, kAccent);
        slider.setColour(juce::Slider::thumbColourId, kAccent.brighter(0.25f));
        slider.setColour(juce::Slider::textBoxTextColourId, juce::Colours::white.withAlpha(0.85f));
        slider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        slider.onDragStart = [this] { if (auto* h = p.hostParameter(p.moduleParamHostIndex(param))) h->beginChangeGesture(); };
        slider.onDragEnd = [this] { if (auto* h = p.hostParameter(p.moduleParamHostIndex(param))) h->endChangeGesture(); };
        slider.onValueChange = [this] { p.setModuleParamValue(param, (float)slider.getValue()); };
        setSize(size == "large" ? 132 : size == "small" ? 88 : 110, size == "large" ? 112 : size == "small" ? 84 : 100);
    }
    void refreshValue() override { if (!slider.isMouseButtonDown()) slider.setValue(p.moduleParamValue(param), juce::dontSendNotification); }
    void resized() override { auto r = getLocalBounds(); name.setBounds(r.removeFromTop(18)); slider.setBounds(r); }
protected:
    int contentWidth() const override { return getWidth(); }
    int contentHeight() const override { return getHeight(); }
private:
    Processor& p; int param;
    juce::Label name; juce::Slider slider;
};

class SliderItem final : public UIItem {
public:
    SliderItem(Processor& pIn, int paramIn, juce::String label, juce::String unit, float min, float max)
        : p(pIn), param(paramIn) {
        addAndMakeVisible(name); addAndMakeVisible(slider);
        name.setText(std::move(label), juce::dontSendNotification);
        name.setFont(uiFont(12.0f));
        slider.setRange(min, max, 0.001);
        if (unit.isNotEmpty()) slider.setTextValueSuffix(" " + unit);
        slider.setColour(juce::Slider::trackColourId, kAccent.withAlpha(0.75f));
        slider.setColour(juce::Slider::thumbColourId, kAccent);
        slider.onDragStart = [this] { if (auto* h = p.hostParameter(p.moduleParamHostIndex(param))) h->beginChangeGesture(); };
        slider.onDragEnd = [this] { if (auto* h = p.hostParameter(p.moduleParamHostIndex(param))) h->endChangeGesture(); };
        slider.onValueChange = [this] { p.setModuleParamValue(param, (float)slider.getValue()); };
        setSize(260, 40);
    }
    void refreshValue() override { if (!slider.isMouseButtonDown()) slider.setValue(p.moduleParamValue(param), juce::dontSendNotification); }
    void resized() override { auto r = getLocalBounds(); name.setBounds(r.removeFromLeft(96)); slider.setBounds(r); }
protected:
    int contentWidth() const override { return 260; }
    int contentHeight() const override { return 40; }
private:
    Processor& p; int param;
    juce::Label name; juce::Slider slider;
};

class ToggleItem final : public UIItem {
public:
    ToggleItem(Processor& pIn, int paramIn, juce::String label)
        : p(pIn), param(paramIn) {
        addAndMakeVisible(button);
        button.setButtonText(std::move(label));
        button.setClickingTogglesState(true);
        button.setColour(juce::TextButton::buttonOnColourId, kAccent);
        button.onClick = [this] { p.setModuleParamValue(param, button.getToggleState() ? 1.0f : 0.0f); };
        setSize(200, 32);
    }
    void refreshValue() override {
        if (!button.isMouseButtonDown())
            button.setToggleState(p.moduleParamValue(param) >= 0.5f, juce::dontSendNotification);
    }
    void resized() override { button.setBounds(getLocalBounds()); }
protected:
    int contentWidth() const override { return 200; }
    int contentHeight() const override { return 32; }
private:
    Processor& p; int param;
    juce::TextButton button;
};

class ValueItem final : public UIItem {
public:
    ValueItem(Processor& pIn, int paramIn, juce::String label, int precisionIn, juce::String unit)
        : p(pIn), param(paramIn), precision(precisionIn), suffix(unit.isNotEmpty() ? " " + unit : juce::String()) {
        addAndMakeVisible(name); addAndMakeVisible(value);
        name.setText(std::move(label), juce::dontSendNotification);
        name.setFont(uiFont(12.0f));
        value.setJustificationType(juce::Justification::centredRight);
        value.setFont(uiFont(12.0f));
        setSize(180, 24);
    }
    void refreshValue() override {
        value.setText(juce::String(p.moduleParamValue(param), precision) + suffix, juce::dontSendNotification);
    }
    void resized() override { auto r = getLocalBounds(); name.setBounds(r.removeFromLeft(96)); value.setBounds(r); }
protected:
    int contentWidth() const override { return 180; }
    int contentHeight() const override { return 24; }
private:
    Processor& p; int param, precision; juce::String suffix;
    juce::Label name, value;
};

class LabelItem final : public UIItem {
public:
    explicit LabelItem(juce::String text) {
        addAndMakeVisible(label);
        label.setText(std::move(text), juce::dontSendNotification);
        label.setFont(uiFont(12.0f));
        setSize(240, 24);
    }
    void resized() override { label.setBounds(getLocalBounds()); }
protected:
    int contentWidth() const override { return 240; }
    int contentHeight() const override { return 24; }
private:
    juce::Label label;
};

class SpacerItem final : public UIItem {
public:
    explicit SpacerItem(int size) { setSize(size, size); }
protected:
    int contentWidth() const override { return getWidth(); }
    int contentHeight() const override { return getHeight(); }
};

std::unique_ptr<UIItem> build(Processor& p, const Node& node) {
    switch (node.type) {
        case Node::Type::column:
        case Node::Type::row:
        case Node::Type::group: {
            auto c = std::make_unique<ContainerItem>(node.type == Node::Type::row, node.gap, node.padding,
                                                     node.title, node.type == Node::Type::group);
            for (const auto& child : node.children) if (auto sub = build(p, *child)) c->add(std::move(sub));
            return c;
        }
        case Node::Type::knob: return std::make_unique<KnobItem>(p, node.param, node.label, node.unit, node.min, node.max, node.text);
        case Node::Type::slider: return std::make_unique<SliderItem>(p, node.param, node.label, node.unit, node.min, node.max);
        case Node::Type::toggle: return std::make_unique<ToggleItem>(p, node.param, node.label);
        case Node::Type::value: return std::make_unique<ValueItem>(p, node.param, node.label, node.precision, node.unit);
        case Node::Type::label: return std::make_unique<LabelItem>(node.text);
        case Node::Type::spacer: return std::make_unique<SpacerItem>(node.spacer);
    }
    return {};
}

} // namespace

bool parseDocument(Processor& p, const juce::String& json, Node& rootNode, juce::String& error) {
    juce::var v;
    const auto result = juce::JSON::parse(json, v);
    if (!result.wasOk()) { error = "invalid JSON"; return false; }
    auto* o = v.getDynamicObject();
    if (o == nullptr) { error = "document must be an object"; return false; }
    if (!onlyKeys(*o, {"schema", "title", "layout"}, error)) return false;
    if (!v["schema"].isString() || v["schema"].toString() != "openvst.ui/0") { error = "unsupported UI schema"; return false; }
    const auto title = v["title"];
    if (!title.isVoid() && (!title.isString() || title.toString().length() > 64)) { error = "title invalid"; return false; }
    if (!v["layout"].isObject()) { error = "layout missing"; return false; }
    int count = 0;
    std::map<juce::String, int> bindings;
    return parseNode(v["layout"], 0, count, p, bindings, rootNode, error);
}

struct ModuleUIComponent::Impl {
    explicit Impl(Processor& processor) : p(processor) {}
    Processor& p;
    std::unique_ptr<UIItem> root;
    juce::String error;
    bool parse(const juce::String& json) {
        Node rootNode;
        if (!parseDocument(p, json, rootNode, error)) return false;
        root = build(p, rootNode);
        return root != nullptr;
    }
};

ModuleUIComponent::ModuleUIComponent(Processor& processor)
    : impl(std::make_unique<Impl>(processor)) {}

ModuleUIComponent::~ModuleUIComponent() = default;

std::unique_ptr<ModuleUIComponent> ModuleUIComponent::createFromJSON(Processor& p, const juce::String& json) {
    if (json.length() > 64 * 1024) return {};
    auto ui = std::unique_ptr<ModuleUIComponent>(new ModuleUIComponent(p));
    if (!ui->impl->parse(json)) {
        juce::Logger::writeToLog("OpenVST UI rejected: " + ui->impl->error);
        return {};
    }
    ui->addAndMakeVisible(*ui->impl->root);
    ui->setSize(ui->preferredWidth(), ui->preferredHeight());
    ui->startTimerHz(30);
    return ui;
}

std::unique_ptr<ModuleUIComponent> ModuleUIComponent::create(Processor& p, const juce::File& uiFile) {
    if (!uiFile.existsAsFile() || uiFile.getSize() <= 0 || uiFile.getSize() > 64 * 1024) return {};
    return createFromJSON(p, uiFile.loadFileAsString());
}

bool ModuleUIComponent::validate(Processor& p, const juce::String& json, juce::String& error) {
    if (json.isEmpty() || json.length() > 64 * 1024) { error = "UI document missing or too large"; return false; }
    Node root;
    return parseDocument(p, json, root, error);
}

int ModuleUIComponent::preferredWidth() const { return impl->root ? impl->root->preferredWidth() : 0; }
int ModuleUIComponent::preferredHeight() const { return impl->root ? impl->root->preferredHeight() : 0; }

void ModuleUIComponent::resized() {
    if (impl->root) impl->root->setBounds(getLocalBounds());
}

void ModuleUIComponent::timerCallback() {
    if (impl->root) impl->root->refreshValue();
}

} // namespace openvst
