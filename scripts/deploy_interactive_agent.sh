#!/bin/sh
# Push the interactive agent to Windows and (re)start its scheduled task.
set -eu
cd "$(dirname -- "$0")/.."
KEY="$HOME/.ssh/id_ed25519_windows"
scp -i "$KEY" -o HostKeyAlias=windows -o BatchMode=yes tests/windows/interactive_agent.ps1 \
    "Arthur@192.168.1.31:C:/Users/Arthur/AppData/Local/OpenVST-test/" 2>&1 |
    grep -v -e post-quantum -e 'store now' -e 'may need to be upgraded' || true
python3 - <<'PY'
import base64, pathlib, subprocess
ps = r'''
$stage = Join-Path $env:LOCALAPPDATA 'OpenVST-test'
$agentDir = Join-Path $stage 'agent'
New-Item -ItemType Directory -Force -Path $agentDir | Out-Null
Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" |
  Where-Object { $_.CommandLine -like '*interactive_agent*' } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 2
Unregister-ScheduledTask -TaskName 'OpenVST-Agent' -Confirm:$false -ErrorAction SilentlyContinue
$action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ('-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + (Join-Path $stage 'interactive_agent.ps1') + '" -WorkDir "' + $agentDir + '"') -WorkingDirectory $stage
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Highest
Register-ScheduledTask -TaskName 'OpenVST-Agent' -Action $action -Principal $principal -Force | Out-Null
Remove-Item -Force (Join-Path $agentDir 'agent-ready.txt') -ErrorAction SilentlyContinue
Start-ScheduledTask -TaskName 'OpenVST-Agent'
Start-Sleep -Seconds 6
Write-Output ('ready=' + (Test-Path (Join-Path $agentDir 'agent-ready.txt')))
'''
encoded = base64.b64encode(ps.encode('utf-16le')).decode()
r = subprocess.run(['ssh', '-i', str(pathlib.Path.home() / '.ssh/id_ed25519_windows'),
                    '-o', 'HostKeyAlias=windows', '-o', 'BatchMode=yes', 'Arthur@192.168.1.31',
                    'powershell -NoProfile -EncodedCommand ' + encoded],
                   capture_output=True, text=True, timeout=90, errors='replace')
print(r.stdout.strip()[-200:])
PY
