#!/bin/sh
# Push the Windows test kit and start the isolated build + REAPER run.
# The kit lives in $env:LOCALAPPDATA\OpenVST-test and never touches Arthur's
# own REAPER configuration, projects or VST3 folders.
set -eu
cd "$(dirname -- "$0")/.."
ART=artifacts/windows
SSH="ssh -i $HOME/.ssh/id_ed25519_windows -o HostKeyAlias=windows -o BatchMode=yes"
DEST='C:/Users/Arthur/AppData/Local/OpenVST-test-staging/'
cp tests/windows/CMakeLists.txt "$ART/kit/CMakeLists.txt"
mkdir -p "$ART/kit/tests/daw"
cp tests/windows/create_projects.lua "$ART/kit/tests/daw/"
cp tests/daw/make_input.py tests/daw/check_render.py "$ART/kit/tests/daw/"
tar -C research/JUCE -czf "$ART/juce-9.0.5-src.tar.gz" modules extras/Build CMakeLists.txt LICENSE.md
(cd "$ART" && tar -czf kit.tar.gz kit juce-9.0.5-src.tar.gz cmajor-include-1.0.3209.tar.gz run_windows_test.ps1)
sha256sum "$ART/kit.tar.gz" "$ART/run_windows_test.ps1"
$SSH Arthur@192.168.1.31 "cmd /c mkdir \"C:\\Users\\Arthur\\AppData\\Local\\OpenVST-test-staging\"" >/dev/null 2>&1 || true
scp -i "$HOME/.ssh/id_ed25519_windows" -o HostKeyAlias=windows -o BatchMode=yes \
  "$ART/kit.tar.gz" "Arthur@192.168.1.31:$DEST" 2>&1 | grep -v -e 'post-quantum' -e 'store now' -e 'may need to be upgraded' || true
python3 - <<'PY'
import base64, pathlib, subprocess
ps = r'''
$stage = Join-Path $env:LOCALAPPDATA 'OpenVST-test'
$src = Join-Path $env:LOCALAPPDATA 'OpenVST-test-staging'
Remove-Item -Recurse -Force (Join-Path $stage 'kit') -ErrorAction SilentlyContinue
tar -xzf (Join-Path $src 'kit.tar.gz') -C $stage
Write-Output ('ps1_sha=' + (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $stage 'run_windows_test.ps1')).Hash.ToLower())
Write-Output ('cmake_sha=' + (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $stage 'kit\CMakeLists.txt')).Hash.ToLower())
'''
encoded = base64.b64encode(ps.encode('utf-16le')).decode()
r = subprocess.run(['ssh', '-i', str(pathlib.Path.home() / '.ssh/id_ed25519_windows'),
                    '-o', 'HostKeyAlias=windows', '-o', 'BatchMode=yes', 'Arthur@192.168.1.31',
                    'powershell -NoProfile -EncodedCommand ' + encoded],
                   capture_output=True, text=True, timeout=180)
print(r.stdout.strip()[-300:])
PY
echo "Kit deployed. Run: ssh ... 'powershell -NoProfile -ExecutionPolicy Bypass -File ...\\run_windows_test.ps1'"
