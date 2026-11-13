"""Executable contract tests for the real JUCE VST3 host probe, not a DAW certification."""
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
BINARY = ROOT / "build/host/OpenVSTHostProbe_artefacts/Release/OpenVSTHostProbe"


class HostCliTests(unittest.TestCase):
    def test_usage(self):
        result = subprocess.run([str(BINARY)], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 2)
        self.assertIn("usage:", result.stderr)

    def test_missing_plugin(self):
        result = subprocess.run([str(BINARY), str(ROOT / "absent.vst3")],
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing plugin", result.stderr)

    def test_unknown_fixture_mode(self):
        result = subprocess.run([str(BINARY), str(ROOT / "absent.vst3"), "--unknown"],
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 2)
        self.assertIn("unknown mode", result.stderr)


if __name__ == "__main__":
    unittest.main()
