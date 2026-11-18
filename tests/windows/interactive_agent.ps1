# Interactive-session agent: a small command queue polled from the server.
#
# REAPER cannot run from an SSH session (no desktop), so this script runs in the
# logged-on user session through a scheduled task and executes commands the
# server writes to cmd.txt. Supported commands, one per line:
#
#   launch <exe> <args...>    start a process
#   focus <processName>       bring its main window to the front
#   key <sendkeys>            send keystrokes to the focused window
#   click <x> <y>             move the pointer and left click
#   type <text>               send text to the focused window
#   shot <name>               save a PNG of the whole screen
#   windows                   list processes that own a window
#   kill <processName>        terminate a process
#   sleep <ms>                wait
#   stop                      end the agent loop
#
# Every command appends a line to result.txt so the caller can confirm it ran.
param(
    [Parameter(Mandatory = $true)][string]$WorkDir
)
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Native {
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
$cmdFile = Join-Path $WorkDir 'cmd.txt'
$resFile = Join-Path $WorkDir 'result.txt'
$ready = Join-Path $WorkDir 'agent-ready.txt'
$log = Join-Path $WorkDir 'agent.log'
function Note($text) { ('{0:HH:mm:ss} {1}' -f (Get-Date), $text) | Out-File -FilePath $log -Append -Encoding utf8 }
function Dismiss-AudioPrompt {
    if (-not ('AudioPrompt' -as [type])) {
        Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class AudioPrompt {
    public delegate bool Proc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(Proc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, Proc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    public static bool DismissFor(System.Collections.Generic.HashSet<uint> targets) {
        bool dismissed = false;
        EnumWindows((h, p) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (!targets.Contains(pid)) return true;
            bool mentionsAudio = false;
            IntPtr noButton = IntPtr.Zero;
            EnumChildWindows(h, (ch, cp) => {
                var t = new StringBuilder(512); GetWindowText(ch, t, 512);
                var c = new StringBuilder(128); GetClassName(ch, c, 128);
                if (t.ToString().Contains("audio device")) mentionsAudio = true;
                if (c.ToString() == "Button" && t.ToString().Trim('&', ' ') == "No") noButton = ch;
                return true;
            }, IntPtr.Zero);
            if (mentionsAudio && noButton != IntPtr.Zero) {
                SendMessage(noButton, 0x00F5, IntPtr.Zero, IntPtr.Zero);
                dismissed = true;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return dismissed;
    }
}
"@
    }
    $pids = New-Object 'System.Collections.Generic.HashSet[uint32]'
    Get-CimInstance Win32_Process -Filter "Name='reaper.exe'" | ForEach-Object {
        if ($_.CommandLine -and $_.CommandLine -like '*OpenVST-test\profile*') { [void]$pids.Add([uint32]$_.ProcessId) }
    }
    if ($pids.Count -eq 0) { return 'no-test-instance' }
    if ([AudioPrompt]::DismissFor($pids)) { return 'dismissed' }
    return 'no-prompt'
}
function Shot($name) {
    $b = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
    $path = Join-Path $WorkDir ($name + '.png')
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    return $path
}
'SessionId=' + (Get-Process -Id $PID).SessionId | Out-File -FilePath $ready -Encoding utf8
Note 'agent started'
while ($true) {
    if (Test-Path (Join-Path $WorkDir 'stop.txt')) { Note 'stop requested'; break }
    if (-not (Test-Path $cmdFile)) { Start-Sleep -Milliseconds 500; continue }
    Start-Sleep -Milliseconds 200
    $lines = @(Get-Content -LiteralPath $cmdFile -Encoding UTF8)
    Remove-Item -LiteralPath $cmdFile -Force -ErrorAction SilentlyContinue
    foreach ($line in $lines) {
        $line = $line.Trim()
        if (-not $line) { continue }
        $parts = $line -split ' ', 3
        $verb = $parts[0].ToLower()
        Note ('cmd ' + $line)
        try {
            switch ($verb) {
                'launch' {
                    $exe = $parts[1]
                    $a = @()
                    if ($parts.Count -ge 3) { $a = $parts[2] -split '\|' }
                    $p = Start-Process -FilePath $exe -ArgumentList $a -PassThru
                    ('launch ok pid=' + $p.Id) | Out-File -FilePath $resFile -Append -Encoding utf8
                }
                'reaper' {
                    # Fixed path on purpose: "C:\Program Files\REAPER (x64)" has spaces.
                    $exe = Join-Path $env:ProgramFiles 'REAPER (x64)\reaper.exe'
                    $a = @()
                    if ($parts.Count -ge 2) { $a = $parts[1] -split '\|' }
                    $p = Start-Process -FilePath $exe -ArgumentList $a -PassThru
                    ('reaper ok pid=' + $p.Id) | Out-File -FilePath $resFile -Append -Encoding utf8
                }
                'focus' {
                    $p = Get-Process -Name $parts[1] -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
                    if (-not $p) { throw 'no window for ' + $parts[1] }
                    [Native]::ShowWindow($p.MainWindowHandle, 9) | Out-Null
                    [Native]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
                    ('focus ok ' + $p.MainWindowTitle) | Out-File -FilePath $resFile -Append -Encoding utf8
                }
                'max' {
                    $p = Get-Process -Name $parts[1] -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
                    if (-not $p) { throw 'no window for ' + $parts[1] }
                    [Native]::ShowWindow($p.MainWindowHandle, 3) | Out-Null
                    [Native]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
                    Start-Sleep -Milliseconds 400
                    ('max ok ' + $p.MainWindowTitle) | Out-File -FilePath $resFile -Append -Encoding utf8
                }
                'rect' {
                    $p = Get-Process -Name $parts[1] -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
                    if (-not $p) { throw 'no window for ' + $parts[1] }
                    $r = New-Object Native+RECT
                    [Native]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
                    ('rect ' + $r.Left + ' ' + $r.Top + ' ' + $r.Right + ' ' + $r.Bottom) | Out-File -FilePath $resFile -Append -Encoding utf8
                }
                'key' {
                    [System.Windows.Forms.SendKeys]::SendWait($parts[2])
                    ('key ok ' + $parts[2]) | Out-File -FilePath $resFile -Append -Encoding utf8
                }
                'type' {
                    [System.Windows.Forms.SendKeys]::SendWait($parts[2])
                    ('type ok') | Out-File -FilePath $resFile -Append -Encoding utf8
                }
                'click' {
                    $x = [int]$parts[1]
                    $y = [int]$parts[2]
                    [Native]::SetCursorPos($x, $y) | Out-Null
                    Start-Sleep -Milliseconds 120
                    [Native]::mouse_event(0x02, 0, 0, 0, [IntPtr]::Zero)
                    [Native]::mouse_event(0x04, 0, 0, 0, [IntPtr]::Zero)
                    ('click ok ' + $x + ',' + $y) | Out-File -FilePath $resFile -Append -Encoding utf8
                }
                'shot' {
                    $path = Shot $parts[1]
                    ('shot ' + $path) | Out-File -FilePath $resFile -Append -Encoding utf8
                }
                'shotwin' {
                    # Capture one application window, not the whole desktop, so the
                    # user's other windows never end up in the artefact.
                    $procName = $parts[1]
                    $rest = $parts[2] -split ' ', 2
                    $name = $rest[0]
                    $p = Get-Process -Name $procName -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
                    if (-not $p) { throw 'no window for ' + $procName }
                    [Native]::ShowWindow($p.MainWindowHandle, 9) | Out-Null
                    [Native]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
                    Start-Sleep -Milliseconds 600
                    $r = New-Object Native+RECT
                    [Native]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
                    $w = $r.Right - $r.Left
                    $h = $r.Bottom - $r.Top
                    if ($w -le 0 -or $h -le 0) { throw 'invalid window rect' }
                    $bmp = New-Object System.Drawing.Bitmap $w, $h
                    $g = [System.Drawing.Graphics]::FromImage($bmp)
                    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size $w, $h))
                    $path = Join-Path $WorkDir ($name + '.png')
                    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
                    $g.Dispose(); $bmp.Dispose()
                    ('shotwin ' + $path + ' ' + $w + 'x' + $h) | Out-File -FilePath $resFile -Append -Encoding utf8
                }
                'clickreaper' {
                    # Click at window-relative coordinates inside the test
                    # REAPER window (title bar included, like the screenshot).
                    $target = Get-CimInstance Win32_Process -Filter "Name='reaper.exe'" |
                        Where-Object { $_.CommandLine -and $_.CommandLine -like '*OpenVST-test\profile*' } |
                        Select-Object -First 1
                    if (-not $target) { throw 'no test REAPER instance' }
                    $p = Get-Process -Id $target.ProcessId -ErrorAction Stop
                    [Native]::ShowWindow($p.MainWindowHandle, 9) | Out-Null
                    [Native]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
                    Start-Sleep -Milliseconds 400
                    $r = New-Object Native+RECT
                    [Native]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
                    $x = $r.Left + [int]$parts[1]
                    $y = $r.Top + [int]$parts[2]
                    [Native]::SetCursorPos($x, $y) | Out-Null
                    Start-Sleep -Milliseconds 150
                    [Native]::mouse_event(0x02, 0, 0, 0, [IntPtr]::Zero)
                    [Native]::mouse_event(0x04, 0, 0, 0, [IntPtr]::Zero)
                    ('clickreaper ok ' + $x + ',' + $y + ' window=' + $r.Left + ',' + $r.Top) | Out-File -FilePath $resFile -Append -Encoding utf8
                }
                'dblclickreaper' {
                    $target = Get-CimInstance Win32_Process -Filter "Name='reaper.exe'" |
                        Where-Object { $_.CommandLine -and $_.CommandLine -like '*OpenVST-test\profile*' } |
                        Select-Object -First 1
                    if (-not $target) { throw 'no test REAPER instance' }
                    $p = Get-Process -Id $target.ProcessId -ErrorAction Stop
                    [Native]::ShowWindow($p.MainWindowHandle, 9) | Out-Null
                    [Native]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
                    Start-Sleep -Milliseconds 400
                    $r = New-Object Native+RECT
                    [Native]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
                    $x = $r.Left + [int]$parts[1]
                    $y = $r.Top + [int]$parts[2]
                    [Native]::SetCursorPos($x, $y) | Out-Null
                    Start-Sleep -Milliseconds 120
                    [Native]::mouse_event(0x02, 0, 0, 0, [IntPtr]::Zero)
                    [Native]::mouse_event(0x04, 0, 0, 0, [IntPtr]::Zero)
                    Start-Sleep -Milliseconds 80
                    [Native]::mouse_event(0x02, 0, 0, 0, [IntPtr]::Zero)
                    [Native]::mouse_event(0x04, 0, 0, 0, [IntPtr]::Zero)
                    ('dblclickreaper ok ' + $x + ',' + $y) | Out-File -FilePath $resFile -Append -Encoding utf8
                }
                'shotreaper' {
                    # Capture the test REAPER window only: the instance whose
                    # command line names the OpenVST test profile. Arthur's own
                    # session is never captured.
                    $target = Get-CimInstance Win32_Process -Filter "Name='reaper.exe'" |
                        Where-Object { $_.CommandLine -and $_.CommandLine -like '*OpenVST-test\profile*' } |
                        Select-Object -First 1
                    if (-not $target) { throw 'no test REAPER instance' }
                    $p = Get-Process -Id $target.ProcessId -ErrorAction Stop
                    [Native]::ShowWindow($p.MainWindowHandle, 9) | Out-Null
                    [Native]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
                    Start-Sleep -Milliseconds 500
                    $r = New-Object Native+RECT
                    [Native]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
                    $w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
                    if ($w -le 0 -or $h -le 0) { throw 'invalid window rect' }
                    $bmp = New-Object System.Drawing.Bitmap $w, $h
                    $g = [System.Drawing.Graphics]::FromImage($bmp)
                    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size $w, $h))
                    $path = Join-Path $WorkDir ($parts[1] + '.png')
                    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
                    $g.Dispose(); $bmp.Dispose()
                    ('shotreaper ' + $path + ' ' + $w + 'x' + $h + ' pid=' + $p.Id) | Out-File -FilePath $resFile -Append -Encoding utf8
                }
                'dismiss' {
                    # Answer No to REAPER's "no audio device selected" modal on
                    # test instances only (their command line names the profile).
                    $state = Dismiss-AudioPrompt
                    ('dismiss ' + $state) | Out-File -FilePath $resFile -Append -Encoding utf8
                }
                'windows' {
                    Get-Process | Where-Object { $_.MainWindowTitle } |
                        ForEach-Object { $_.ProcessName + '|' + $_.Id + '|' + $_.MainWindowTitle } |
                        Out-File -FilePath $resFile -Append -Encoding utf8
                    'windows ok' | Out-File -FilePath $resFile -Append -Encoding utf8
                }
                'kill' {
                    Stop-Process -Name $parts[1] -Force -ErrorAction Stop
                    ('kill ok ' + $parts[1]) | Out-File -FilePath $resFile -Append -Encoding utf8
                }
                'sleep' {
                    Start-Sleep -Milliseconds ([int]$parts[1])
                    'sleep ok' | Out-File -FilePath $resFile -Append -Encoding utf8
                }
                default { ('unknown ' + $verb) | Out-File -FilePath $resFile -Append -Encoding utf8 }
            }
        } catch {
            ('error ' + $verb + ' :: ' + $_.Exception.Message) | Out-File -FilePath $resFile -Append -Encoding utf8
        }
    }
    ('done ' + (Get-Date).ToString('HH:mm:ss')) | Out-File -FilePath $resFile -Append -Encoding utf8
}
Note 'agent stopped'
