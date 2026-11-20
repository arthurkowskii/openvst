#!/usr/bin/env python3
"""End-to-end catalogue proof: real server, real package, real install, real DSP.

Run after building the plugin:

    python3 -m unittest discover -s tests/integration -v

The server is stopped between the first render and the offline restore, so the
second half proves that an installed module and its saved state reopen without
any network.
"""
from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROBE = ROOT / "build" / "plugin" / "openvst_catalogue_probe"
SERVER = ROOT / "server" / "openvst_catalogue.py"
RUNTIME = ROOT / "research" / "toolchain" / "1.0.3209" / "linux" / "x64" / "libCmajPerformer.so"
FIXTURES = ROOT / "artifacts" / "plugin" / "fixtures"
MODULE_ID = "org.openvst.demo.tremolo"


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_health(url: str, deadline: float = 10.0) -> None:
    end = time.time() + deadline
    while time.time() < end:
        try:
            with urllib.request.urlopen(url + "/healthz", timeout=0.5) as reply:
                if reply.status == 200:
                    return
        except Exception:
            time.sleep(0.1)
    raise RuntimeError("catalogue server did not become healthy")


@unittest.skipUnless(PROBE.exists(), "build the plugin first: sh scripts/build_plugin.sh")
class CatalogueEndToEnd(unittest.TestCase):
    def test_download_install_render_offline_restore(self) -> None:
        self.exercise_module("make_demo_package.py", MODULE_ID)

    def test_creature_voice_install_render_offline_restore(self) -> None:
        self.exercise_module("make_creature_package.py", "org.openvst.creature-voice")

    def exercise_module(self, builder: str, module_id: str) -> None:
        with tempfile.TemporaryDirectory(prefix="openvst-e2e-") as tmp:
            tmp_path = Path(tmp)
            package = tmp_path / "module.openvst.zip"
            built = subprocess.run(
                [sys.executable, str(ROOT / "tools" / builder), str(package)],
                cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(built.returncode, 0, built.stderr + built.stdout)
            info = json.loads(built.stdout)
            self.assertEqual(info["module"]["id"], module_id)

            data = tmp_path / "server-data"
            seed = subprocess.run(
                [sys.executable, str(SERVER), "--data", str(data),
                 "--admin-token", "e2e-token", "--seed", str(package)],
                cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(seed.returncode, 0, seed.stderr + seed.stdout)

            port = free_port()
            log = (tmp_path / "server.log").open("w")
            server = subprocess.Popen(
                [sys.executable, str(SERVER), "--data", str(data),
                 "--host", "127.0.0.1", "--port", str(port),
                 "--admin-token", "e2e-token"],
                cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
            try:
                url = f"http://127.0.0.1:{port}"
                wait_health(url)
                store = tmp_path / "store"
                state = tmp_path / "module.state"
                common = [str(PROBE), "--store", str(store), "--runtime", str(RUNTIME),
                          "--fixtures", str(FIXTURES), "--id", module_id]
                online = subprocess.run(
                    common + ["--url", url, "--state-out", str(state)],
                    cwd=ROOT, text=True, capture_output=True, timeout=120)
                self.assertEqual(online.returncode, 0, online.stderr + online.stdout)
                self.assertIn("OK id=", online.stdout)
                self.assertTrue(state.is_file() and state.stat().st_size > 0)
            finally:
                server.terminate()
                try:
                    server.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    server.kill()
                log.close()

            # The server is gone: the installed module and the saved state must
            # reopen from the store alone.
            offline = subprocess.run(
                common + ["--offline", "1", "--state-in", str(state)],
                cwd=ROOT, text=True, capture_output=True, timeout=60)
            self.assertEqual(offline.returncode, 0, offline.stderr + offline.stdout)
            self.assertIn("OK id=", offline.stdout)
            # The rendered peak and every parameter must survive a cold reopen.
            self.assertEqual(online.stdout.strip(), offline.stdout.strip())

            installed = list(store.glob("*/**/install.json"))
            self.assertEqual(len(installed), 1, f"expected one install.json, got {installed}")
            self.assertTrue(any(store.rglob("*.cmajor")))


if __name__ == "__main__":
    unittest.main()
