#!/bin/sh
# Drive REAPER in Arthur's interactive Windows session through the agent queue.
#
# Usage: repl.sh "command"        send one command line
#        repl.sh --result         print the result tail
#        repl.sh --shot NAME      capture and fetch a screenshot to artifacts/windows/shots/NAME.png
#        repl.sh --restart        restart the agent task
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
KEY="$HOME/.ssh/id_ed25519_windows"
SSH="ssh -i $KEY -o HostKeyAlias=windows -o BatchMode=yes"
STAGE='C:\Users\Arthur\AppData\Local\OpenVST-test'
AGENT="$STAGE\\agent"

send() {
    python3 - "$1" <<'PY'
import base64, pathlib, subprocess, sys
line = sys.argv[1]
ps = r'''
$dir = Join-Path $env:LOCALAPPDATA 'OpenVST-test\agent'
$tmp = Join-Path $dir 'cmd.tmp'
Remove-Item -Force (Join-Path $dir 'result.txt') -ErrorAction SilentlyContinue
$text = [Convert]::FromBase64String('%s')
[System.IO.File]::WriteAllBytes($tmp, $text)
Move-Item -Force $tmp (Join-Path $dir 'cmd.txt')
Write-Output 'queued'
''' % base64.b64encode(line.encode('utf-8')).decode()
encoded = base64.b64encode(ps.encode('utf-16le')).decode()
r = subprocess.run(['ssh', '-i', str(pathlib.Path.home() / '.ssh/id_ed25519_windows'),
                    '-o', 'HostKeyAlias=windows', '-o', 'BatchMode=yes', 'Arthur@192.168.1.31',
                    'powershell -NoProfile -EncodedCommand ' + encoded],
                   capture_output=True, text=True, timeout=60, errors='replace')
print(r.stdout.strip()[-200:])
PY
}

result() {
    python3 - <<'PY'
import base64, pathlib, subprocess
ps = r'''
$f = Join-Path $env:LOCALAPPDATA 'OpenVST-test\agent\result.txt'
if (Test-Path $f) { Get-Content -LiteralPath $f -Encoding UTF8 | Write-Output } else { 'no result yet' }
'''
encoded = base64.b64encode(ps.encode('utf-16le')).decode()
r = subprocess.run(['ssh', '-i', str(pathlib.Path.home() / '.ssh/id_ed25519_windows'),
                    '-o', 'HostKeyAlias=windows', '-o', 'BatchMode=yes', 'Arthur@192.168.1.31',
                    'powershell -NoProfile -EncodedCommand ' + encoded],
                   capture_output=True, text=True, timeout=60, errors='replace')
print(r.stdout.strip()[-1500:])
PY
}

case "${1:-}" in
    --result) result ;;
    --restart)
        python3 - <<'PY'
import base64, pathlib, subprocess
ps = r'''
$task = 'OpenVST-Agent'
Start-ScheduledTask -TaskName $task
Start-Sleep -Seconds 4
Write-Output ('running=' + ((Get-ScheduledTask -TaskName $task).State))
'''
encoded = base64.b64encode(ps.encode('utf-16le')).decode()
r = subprocess.run(['ssh', '-i', str(pathlib.Path.home() / '.ssh/id_ed25519_windows'),
                    '-o', 'HostKeyAlias=windows', '-o', 'BatchMode=yes', 'Arthur@192.168.1.31',
                    'powershell -NoProfile -EncodedCommand ' + encoded],
                   capture_output=True, text=True, timeout=60, errors='replace')
print(r.stdout.strip()[-300:])
PY
        ;;
    --shot)
        name="${2:-shot}"
        send "shot $name"
        sleep 2
        mkdir -p "$ROOT/artifacts/windows/shots"
        scp -i "$KEY" -o HostKeyAlias=windows -o BatchMode=yes \
            "Arthur@192.168.1.31:C:/Users/Arthur/AppData/Local/OpenVST-test/agent/$name.png" \
            "$ROOT/artifacts/windows/shots/" 2>&1 | grep -v -e post-quantum -e 'store now' -e 'may need to be upgraded' || true
        echo "$ROOT/artifacts/windows/shots/$name.png"
        ;;
    *) send "${1:?command required}" ;;
esac
