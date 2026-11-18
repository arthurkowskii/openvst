# Inner render loop. Runs inside Arthur's interactive session (registered by
# render_task.ps1), because REAPER started from SSH lands in session 0 and
# renders nothing.
#
# `-newinst` relaunches itself, so the process returned by Start-Process is not
# the one that owns the window. Instances are therefore identified by their
# command line: only our test profile contains 'OpenVST-test\profile'. The
# "no audio device selected" modal is dismissed with BM_CLICK on the '&No'
# button, and never on Arthur's own instance, which does not use that profile.
param(
    [Parameter(Mandatory = $true)][string]$Stage,
    [Parameter(Mandatory = $true)][string]$Projects,
    [int]$TimeoutSeconds = 180
)
$ErrorActionPreference = 'Stop'
$reaper = Join-Path ${env:ProgramFiles} 'REAPER (x64)\reaper.exe'
$ini = Join-Path $Stage 'profile\reaper.ini'
$names = @('fx', 'synth', 'automation', 'tremolo')
$log = Join-Path $Stage 'render-worker.log'
Remove-Item $log -Force -ErrorAction SilentlyContinue

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

function Test-Pids {
    $pids = New-Object 'System.Collections.Generic.HashSet[uint32]'
    Get-CimInstance Win32_Process -Filter "Name='reaper.exe'" | ForEach-Object {
        if ($_.CommandLine -and $_.CommandLine -like '*OpenVST-test\profile*') { [void]$pids.Add([uint32]$_.ProcessId) }
    }
    return $pids
}

foreach ($name in $names) {
    $project = Join-Path $Projects ($name + '.rpp')
    $wav = Join-Path $Projects ($name + '-render.wav')
    Start-Process -FilePath $reaper -ArgumentList @(
        '-newinst', '-nosplash', '-ignoreerrors', '-noactivate',
        '-cfgfile', $ini, '-renderproject', $project) | Out-Null
    Add-Content -LiteralPath $log -Value ('start ' + $name)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 700
        $pids = Test-Pids
        if ($pids.Count -gt 0 -and [AudioPrompt]::DismissFor($pids)) {
            Add-Content -LiteralPath $log -Value ('dismissed audio prompt for ' + $name)
        }
        if ((Test-Path $wav) -and $pids.Count -eq 0) { break }
    }
    $pids = Test-Pids
    if ($pids.Count -gt 0) {
        $pids | ForEach-Object { Stop-Process -Id $_ -Force -ErrorAction SilentlyContinue }
        Add-Content -LiteralPath $log -Value ('killed leftover instances for ' + $name)
    }
    Add-Content -LiteralPath $log -Value ('exit ' + $name + ' wav=' + (Test-Path $wav))
}
Add-Content -LiteralPath $log -Value 'done'
