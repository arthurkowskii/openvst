# Module UI, version 0: a declarative interface document

Status: design, validator and native renderer implemented. The renderer
(`src/plugin/ModuleUI.cpp`) parses and validates the document against the active
module and builds native JUCE widgets. The document is validated headlessly by
the catalogue end-to-end probe; the panel has not been opened in a DAW on a
machine with a display yet.
This document describes the data format, the rules an author or an agent must
follow, and the boundary that custom HTML/CSS/JS has not crossed yet.

## Why declarative

Arthur wants module authors, human or agent, to ship a custom-looking interface
without running author JavaScript. A declarative document is the only shape
that can be accepted today with the evidence the project has.

- **Safe by construction.** The document is data. There is no code path that
  evaluates it, no string that becomes a script, no URL the container is asked
  to fetch. The worst a hostile document can do is be refused.
- **Cheap.** The container parses a small JSON tree once, at load time, and
  builds native widgets. No WebView, no JavaScript engine, no per-module
  runtime, no extra process.
- **Deterministic.** The same document renders the same panel on every host.
  There is no layout race, no timing dependency, no difference between the
  offline and the online build.
- **Easy for an agent to generate.** The whole format is a closed set of
  component types with a closed set of keys. A generator cannot drift into a
  half-supported state because there is no half-supported state: an unknown key
  is an error, not a warning.
- **Native accessibility.** Native widgets carry the platform accessibility
  tree, keyboard focus, screen reader roles and high contrast modes for free.
  A web view would have to re-implement them, badly, and prove it.
- **No WebView dependency.** The container stays a native plugin.

The cost is honest: a v0 panel is a column or row of knobs, sliders, toggles,
readouts, labels and spacers. It is not a bespoke skin. The point of v0 is to
prove the pipeline end to end, with a boundary that a future richer format can
grow from without breaking the existing one.

## The document: `openvst.ui/0`

One JSON object, UTF-8, at most 64 KiB.

    {
      "schema": "openvst.ui/0",
      "title": "Transient Shaper",
      "layout": { "type": "column", "gap": 8, "padding": 12, "children": [ ... ] }
    }

Top level keys, exactly:

| Key       | Required | Meaning |
|-----------|----------|---------|
| `schema`  | yes      | Must be the string `openvst.ui/0`. |
| `title`   | no       | Display title, 1..64 printable ASCII characters. |
| `layout`  | yes      | The root component node. |

An unknown top-level key is refused. There is no `style`, no `color`, no
`url`, no `src`, no `script`, no `html` key anywhere in the document. The
absence of those keys is a hard rule, not a convention.

### Node common keys

| Key       | Type | Range |
|-----------|------|-------|
| `type`    | string | Required. One of the component types below. |
| `children` | array | Containers only. |
| `gap`     | integer | 0..32, pixels between children. |
| `padding` | integer | 0..32, pixels inside the node. |
| `width`   | string or integer | `"auto"`, `"fill"`, or 24..400 pixels. |
| `height`  | string or integer | `"auto"`, `"fill"`, or 24..400 pixels. |
| `visible` | boolean | Default true. |

The key set depends on the type. A key that is not in the table for that type
is refused, never ignored.

### Containers

- `column`: children stacked vertically. At least one child.
- `row`: children side by side. At least one child.
- `group`: like a column, with a mandatory `title` (1..64 characters) drawn
  as a group frame caption. A group may be empty.

An empty `column` or `row` is refused: it has no size and no purpose.

### Leaves

- `knob`: `param` (required), optional `label` (1..120), optional `size`
  (`small`, `medium`, `large`), optional `unit` (1..16) overriding the unit
  declared by the manifest parameter.
- `slider`: same keys as `knob`.
- `toggle`: `param` (required), optional `label`. The bound parameter must
  declare a range within 0..1, otherwise the control would misrepresent its
  own state.
- `value`: `param` (required), optional `label`, optional `precision` (0..6
  decimals).
- `label`: `text` (required, 1..120). A static text line.
- `spacer`: optional `size` (0..64).

`param` must be a parameter id declared in the manifest `parameters` array. A
parameter may be bound at most twice, for example a knob and a readout of the
same value. A third binding is refused: two controls writing one parameter in
opposite directions is a contradiction the author has to resolve, not
something the container should arbitrate.

### Bounds

- Maximum depth: 8 levels of nesting.
- Maximum nodes: 128 in one document.
- Maximum string length: 120 characters, except `title` (64) and `unit` (16).
- Maximum document size: 64 KiB.
- All user-visible strings are printable ASCII. Non-ASCII text is refused at
  validation time rather than turning into mojibake in a DAW on another code
  page.

### Worked example: the transient shaper

The file `fixtures/ui/transient-shaper.ui.json` is a complete, valid document
for the transient shaper module. Its three knobs match the three manifest
parameters (`attack`, `sustain`, `gain`), and the readouts below them bind the
same parameters a second time, which is the legal maximum.

    {
      "schema": "openvst.ui/0",
      "title": "Transient Shaper",
      "layout": {
        "type": "column",
        "gap": 8,
        "padding": 12,
        "children": [
          {
            "type": "row",
            "gap": 8,
            "children": [
              { "type": "knob", "param": "attack",  "label": "Attack",      "size": "medium", "unit": "dB" },
              { "type": "knob", "param": "sustain", "label": "Sustain",     "size": "medium", "unit": "dB" },
              { "type": "knob", "param": "gain",    "label": "Output gain", "size": "medium" }
            ]
          },
          {
            "type": "group",
            "title": "Readout",
            "gap": 4,
            "children": [
              { "type": "value", "param": "attack",  "label": "Attack",      "precision": 1 },
              { "type": "value", "param": "sustain", "label": "Sustain",     "precision": 1 },
              { "type": "value", "param": "gain",    "label": "Output gain", "precision": 2 }
            ]
          },
          { "type": "label", "text": "Transient Shaper: differential envelope shaping." }
        ]
      }
    }

## How an author or an agent adds a UI

1. Write a JSON file, for example `ui.json`, next to `openvst.json` in the
   package.
2. Declare it in the manifest, at the top level only:

       "ui": "ui.json"

   The value is a safe relative POSIX path ending in `.json`, 1..240
   characters, with no traversal, no backslash, no drive letter. The key is
   refused anywhere else in the manifest: inside `module`, `runtime`,
   `parameters` or `resources`.

3. The file must exist in the archive, be at most 64 KiB, and be hashed like
   any other entry. Its digest lands in the normal inventory.
4. Run the validator:

       python3 scripts/package_validate.py my-module.zip

The package is refused, with a structured error code, when the document is
wrong. The codes an author will meet:

| Code | Meaning |
|------|---------|
| `ui_missing` | The declared UI file is not in the archive. |
| `ui_too_large` | The document exceeds 64 KiB. |
| `ui_invalid_json` | Not strict UTF-8 JSON: malformed, duplicate key, NaN or Infinity. |
| `ui_schema` | Wrong type, missing required key, bad enum, non-ASCII text, toggle on a parameter whose range is not within 0..1. |
| `ui_unknown_key` | A key that the type does not allow, at any level. |
| `ui_unknown_type` | A component type that does not exist. |
| `ui_bounds` | A length, depth, node count or numeric range exceeded. |
| `ui_param_unknown` | `param` is not declared in the manifest `parameters` array. |
| `ui_param_duplicate_binding` | The same parameter bound more than twice. |

Every error carries the JSON path of the offending node, so a generator can
repair itself from the report alone.

The JSON Schema for the document lives at `docs/schema/module-ui.schema.json`
(draft 2020-12). It describes the shape; the tree-wide rules (node count,
depth, parameter bindings, toggle ranges) live in the validator, because JSON
Schema cannot express them.

## Visual guidance that survives a colourblind reader

Arthur is daltonian. The rules below are part of the format's contract with
the author, not decoration.

- **Every control carries a text label.** A knob with no label is a dial with
  no meaning. The `label` key exists on every control type for that reason.
- **Colour is never the only carrier of meaning.** v0 has no colour key at
  all, which makes the rule structural rather than advisory. A future format
  that adds colour must pair it with a shape, a text, or a position difference.
- **The unit travels with the value.** `unit` on a knob or slider, and the
  manifest parameter's own unit, so a value is never a bare number.
- **Contrast is the container's job.** Native theme colours, checked against
  the platform's contrast requirements in the host platform, not chosen by the
  module author.
- **Large hit targets.** A knob or slider is at least 24 pixels on its short
  side, which is the smallest extent the format accepts. A knob drawn at
  `medium` or `large` should be the default for a primary control.
- **Text scales.** Labels and readouts use the platform font, so the host's
  text size setting applies. The format has no fixed font size.
- **No colour-coded states.** A toggle shows its state as a position and a
  text state, not as a hue.

## Future custom HTML/CSS/JS: not implemented, not claimed safe

The format below is not a preview of a web view. Custom HTML, CSS and
JavaScript inside a module is a separate feature that does not exist in
OpenVST today, and this document does not claim it is close.

What is true now: the container has no module UI at all beyond a minimal
editor (`src/plugin/NativeEditor.h`). A v0 UI document is validated and
hashed, but not rendered.

If custom interfaces are to be enabled later, the following requirements come
before the feature, and none of them is satisfied by a WebView alone:

1. **Isolation.** A separate process with an OS sandbox, or a proven
   equivalent. A JIT, a CSP header or a web view in its own process is not a
   sandbox on its own. The permissions and handles of that process have to be
   measured on Windows and macOS, not assumed.
2. **No network.** The view process gets no route to the internet and no
   access to the catalogue endpoints or to the user's credentials.
3. **A typed, bounded bridge.** The only channel between the view and the
   host is a closed list of message types, each with a size limit and a rate
   limit. Never a raw `PatchConnection` handed to author code.
4. **No native bridge leakage.** No path from the view to arbitrary host
   functions, no file access, no process spawn, no clipboard, no screen
   capture.
5. **Content Security Policy.** A strict CSP with no inline script and no
   remote origin, plus blocked navigation, subresources, frames, popups and
   downloads.
6. **Resource limits.** CPU, memory, file descriptors, IPC queue depth and
   wall-clock time, enforced outside the view process, with a bounded failure
   mode: the view dies, the audio does not.
7. **A signature proves provenance, not safety.** It shows who published a
   package. It says nothing about what the package does.

Until those are implemented and measured, a module UI is a declarative
document and nothing else. A future format version (`openvst.ui/1`) may add
styling, and it will have to keep the same rule: the author ships data, the
container ships the code that draws it.

## Catalogue UX implications

The catalogue can show a module's UI title and the list of its controls on the
module card, because a v0 document is data that the client has already
validated. The card may show the panel title, the control labels, the units
and the parameter ranges.

What the catalogue must not do is treat the document as a web page. There is
no preview rendering, no HTML, no remote content, no screenshot fetched from
a server. The catalogue renders the same native components the plugin does,
from the same validated data, so a card can never show something the plugin
will not show.

A module without a UI document is valid. The catalogue then shows the
parameter list alone, without a panel preview. The UI is optional by design:
a missing UI is never a reason to refuse a package.

## References

- `docs/schema/module-ui.schema.json`: the JSON Schema of the document.
- `fixtures/ui/transient-shaper.ui.json`: a complete valid example.
- `openvst/package_validation.py`: the validator that enforces the rules.
- `tests/packages/test_ui_validation.py`: the tests that prove the rules.
- `docs/MODULE_FORMAT.md`: the package contract this document extends.
- `docs/design/CATALOGUE_DESIGN.md` section 3: the trust boundary this
  format respects.
