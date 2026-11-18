"""Rewrite verified Linux REAPER projects for a Windows render.

The .rpp format and the plugin state blob are portable, so the projects are
produced once by REAPER on Linux and only their absolute paths are rewritten
here. No ReaScript runs on Windows: REAPER does not execute command-line
scripts in the context this test uses.

Usage: prepare_projects.py SOURCE_DIR TARGET_WINDOWS_DIR OUTPUT_DIR
"""
import shutil
import sys
from pathlib import Path

PROJECTS = ("fx", "synth", "automation", "tremolo")


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        print("usage: prepare_projects.py SOURCE_DIR TARGET_WINDOWS_DIR OUTPUT_DIR",
              file=sys.stderr)
        return 2
    source, remote, output = Path(argv[1]), argv[2].rstrip("\\"), Path(argv[3])
    output.mkdir(parents=True, exist_ok=True)
    shutil.copy(source / "input.wav", output / "input.wav")
    for name in PROJECTS:
        text = (source / f"{name}.rpp").read_text(encoding="utf-8", errors="surrogateescape")
        lines = []
        for line in text.splitlines(True):
            if line.lstrip().startswith("RENDER_FILE"):
                line = '  RENDER_FILE "%s\\%s-render.wav"\n' % (remote, name)
            lines.append(line)
        (output / f"{name}.rpp").write_text("".join(lines), encoding="utf-8",
                                           errors="surrogateescape")
    print("\n".join(sorted(p.name for p in output.iterdir())))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
