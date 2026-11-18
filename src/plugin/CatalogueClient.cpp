// SPDX-License-Identifier: AGPL-3.0-only
#include "CatalogueClient.h"
#include "ModuleRegistry.h"
#include <cmath>

namespace openvst {
namespace {

constexpr int kMaxResponseBytes = 2 * 1024 * 1024;
constexpr int kMaxModules = 100;

bool stringField(const juce::var& item, const char* key, int maxLength, juce::String& out) {
    const auto v = item[key];
    if (!v.isString()) return false;
    const auto s = v.toString();
    if (s.isEmpty() || s.length() > maxLength) return false;
    out = s;
    return true;
}

bool numberField(const juce::var& item, const char* key, double& out) {
    const auto v = item[key];
    if (!(v.isDouble() || v.isInt()) || v.isBool()) return false;
    const double d = (double)v;
    if (!std::isfinite(d)) return false;
    out = d;
    return true;
}

bool parseParams(const juce::var& item, std::vector<ModuleParamSpec>& out) {
    const auto params = item["parameters"];
    if (params.isVoid()) return true; // older servers may omit the list
    if (!params.isArray()) return false;
    auto* array = params.getArray();
    if (array == nullptr || array->size() > kMaxParamsPerModule) return false;
    for (const auto& p : *array) {
        if (!p.isObject()) return false;
        ModuleParamSpec spec;
        if (!stringField(p, "id", 64, spec.id) || !stringField(p, "name", 120, spec.name)) return false;
        double minimum = 0, maximum = 0, def = 0;
        if (!numberField(p, "min", minimum) || !numberField(p, "max", maximum)
            || !numberField(p, "default", def)) return false;
        if (!(minimum <= def && def <= maximum)) return false;
        spec.min = (float)minimum; spec.max = (float)maximum; spec.def = (float)def;
        const auto unit = p["unit"];
        if (!unit.isVoid()) {
            if (!unit.isString() || unit.toString().length() > 16) return false;
            spec.unit = unit.toString();
        }
        const auto endpoint = p["endpoint"];
        if (!endpoint.isVoid()) {
            if (!endpoint.isString() || endpoint.toString().isEmpty() || endpoint.toString().length() > 128) return false;
            spec.endpoint = endpoint.toString();
        } else {
            spec.endpoint = spec.id;
        }
        out.push_back(std::move(spec));
    }
    return true;
}

} // namespace

CatalogueClient::CatalogueClient(juce::String baseURL) : base(std::move(baseURL)) {
    while (base.endsWithChar('/')) base = base.dropLastCharacters(1);
}

juce::URL CatalogueClient::endpoint(const juce::String& base, const juce::String& path) {
    return juce::URL(base + path);
}

CatalogueClient::ListResult CatalogueClient::list(const juce::String& kind, const juce::String& query) {
    ListResult result;
    if (base.isEmpty()) { result.error = "No catalogue URL configured"; return result; }
    auto url = endpoint(base, "/v1/modules").withParameter("limit", "100");
    if (kind.isNotEmpty()) url = url.withParameter("kind", kind);
    if (query.isNotEmpty()) url = url.withParameter("q", query);

    int statusCode = 0;
    auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                       .withConnectionTimeoutMs(15000)
                       .withNumRedirectsToFollow(5)
                       .withStatusCode(&statusCode)
                       .withExtraHeaders("Accept: application/json\r\n");
    auto stream = url.createInputStream(options);
    if (stream == nullptr) { result.error = "Catalogue unreachable"; return result; }
    juce::String body;
    if (!readBounded(*stream, body, result.error)) return result;
    if (statusCode != 200) { result.error = "Catalogue HTTP " + juce::String(statusCode); return result; }
    juce::var doc;
    const auto parsed = juce::JSON::parse(body, doc);
    if (!parsed.wasOk()) { result.error = "Catalogue returned invalid JSON"; return result; }
    auto* items = doc["items"].getArray();
    if (items == nullptr || items->size() > kMaxModules) { result.error = "Catalogue response shape invalid"; return result; }
    for (const auto& item : *items) {
        CatalogueEntry entry;
        if (!item.isObject()
            || !stringField(item, "id", 128, entry.id)
            || !stringField(item, "name", 128, entry.name)
            || !stringField(item, "vendor", 128, entry.vendor)
            || !stringField(item, "kind", 16, entry.kind)
            || !stringField(item, "version", 64, entry.version)
            || !stringField(item, "digest", 64, entry.digest)
            || !stringField(item, "status", 16, entry.status)) {
            result.error = "Catalogue item invalid";
            return result;
        }
        const auto size = item["size"];
        if (!(size.isInt() || size.isDouble()) || (double)size < 0) { result.error = "Catalogue item size invalid"; return result; }
        entry.size = (juce::int64)(double)size;
        if (entry.digest.length() != 64 || entry.digest.containsAnyOf("GHIJKLMNOPQRSTUVWXYZghijklmnopqrstuvwxyz")) {
            result.error = "Catalogue digest invalid";
            return result;
        }
        if (!parseParams(item, entry.params)) { result.error = "Catalogue parameters invalid"; return result; }
        result.modules.push_back(std::move(entry));
    }
    result.ok = true;
    return result;
}

CatalogueClient::DownloadResult CatalogueClient::download(const CatalogueEntry& entry, const juce::File& partFile,
                                                          std::function<bool(double)> progress) {
    DownloadResult result;
    if (base.isEmpty()) { result.error = "No catalogue URL configured"; return result; }
    const auto url = endpoint(base, "/v1/packages/" + entry.digest);
    const auto fetched = ModuleStore::download(url, partFile, entry.digest, std::move(progress));
    result.ok = fetched.ok;
    result.error = fetched.error;
    result.bytes = fetched.bytes;
    result.file = partFile;
    return result;
}

bool CatalogueClient::readBounded(juce::InputStream& stream, juce::String& out, juce::String& error) {
    juce::MemoryOutputStream mem;
    char buffer[8192];
    int total = 0;
    for (;;) {
        const int n = stream.read(buffer, (int)sizeof(buffer));
        if (n < 0) { error = "Catalogue stream failed"; return false; }
        if (n == 0) break;
        total += n;
        if (total > kMaxResponseBytes) { error = "Catalogue response too large"; return false; }
        mem.write(buffer, (size_t)n);
    }
    out = mem.toString();
    return true;
}

} // namespace openvst
