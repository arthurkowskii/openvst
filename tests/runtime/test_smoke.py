"""Contrat minimal exécuté avant l'implémentation puis après compilation."""
import pathlib
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]

class Smoke(unittest.TestCase):
    def test_runtime_version(self):
        exe = ROOT / 'artifacts/runtime/bin/runtime_harness'
        self.assertTrue(exe.is_file(), 'harness natif non implémenté/compilé')
        p = subprocess.run([str(exe), str(ROOT / 'research/toolchain/1.0.3209/linux/x64/libCmajPerformer.so'), '--version'], capture_output=True, text=True, timeout=15)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout.strip(), '1.0.3209')

if __name__ == '__main__':
    unittest.main()
