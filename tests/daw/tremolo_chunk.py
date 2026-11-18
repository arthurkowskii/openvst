"""Build the VST3 state chunk that makes REAPER run the installed Tremolo.

The container writes its own state document, and JUCE appends a small private
block after it (the host-level bypass flag). REAPER's ``vst_chunk`` named
parameter is that whole blob, base64 encoded. Injecting it is the same code
path the host uses when it opens a saved project, so a render made this way
proves the project state selects the catalogue module, without a hand-edited
project file.

Usage: tremolo_chunk.py STATE_JSON REFERENCE_RPP OUTPUT_B64
"""

from __future__ import annotations

import base64
import json
import sys
from pathlib import Path


def split_container_state(chunk: bytes) -> tuple[bytes, bytes]:
    """Return (prefix, tail) around the container's JSON state document.

    JUCE's VST3 wrapper stores the processor state through a memory stream that
    prefixes the data with its size and a version word, and appends a private
    block (the host-level bypass flag) after it. The size word counts the state
    minus those 16 bytes, so it has to follow the document length.
    """
    import struct

    if chunk[8:9] != b"{":
        raise SystemExit("reference chunk does not start with the JUCE state header + JSON")
    size, version = struct.unpack("<II", chunk[:8])
    if version != 1 or size != len(chunk) - 16:
        raise SystemExit(f"unexpected JUCE state header: size={size} version={version} total={len(chunk)}")
    _, end = json.JSONDecoder().raw_decode(chunk[8:].decode("utf-8"))
    tail = chunk[8 + end:]
    if b"JUCEPrivateData" not in tail:
        raise SystemExit("reference chunk tail is not JUCE private data")
    return chunk[:8], tail


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        print("usage: tremolo_chunk.py STATE_JSON REFERENCE_B64 OUTPUT_B64", file=sys.stderr)
        return 2
    state = json.loads(Path(argv[1]).read_text(encoding="utf-8"))
    if state.get("schema") != 3 or not state.get("module"):
        raise SystemExit("state is not a schema 3 module document")
    # The reference is the chunk the host reads from a healthy FX instance: the
    # container's document plus JUCE's private tail. Only the document changes.
    prefix, tail = split_container_state(base64.b64decode(Path(argv[2]).read_text().strip()))
    # The container's own writer formats this document with spaces; keeping the
    # exact saved bytes means the plugin reads back what it wrote.
    text = Path(argv[1]).read_text(encoding="utf-8").encode("utf-8")
    import struct
    size = struct.pack("<I", len(prefix) + len(text) + len(tail) - 16)
    chunk = size + prefix[4:] + text + tail
    Path(argv[3]).write_text(base64.b64encode(chunk).decode("ascii"), encoding="ascii")
    print(json.dumps({"module": state["module"], "params": state.get("params", {}),
                      "chunk_bytes": len(chunk)}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
