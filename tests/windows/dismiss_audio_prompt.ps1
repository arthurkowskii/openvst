# Dismiss REAPER's "no audio device selected" startup modal on the test
# instance only. The dialog belongs to the automated render instance; its pid
# is matched against our own command line so Arthur's session is never touched.
param(
    [Parameter(Mandatory = $true)][int]$ProcessId,
    [int]$TimeoutSeconds = 12
)
$ErrorActionPreference = 'Stop'
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class Dlg {
    public delegate bool Proc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(Proc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, Proc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    public static string Dismiss(uint target) {
        string result = null;
        EnumWindows((h, p) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid != target) return true;
            var text = new StringBuilder(1024);
            bool mentionsAudio = false;
            IntPtr noButton = IntPtr.Zero;
            EnumChildWindows(h, (ch, cp) => {
                var t = new StringBuilder(512); GetWindowText(ch, t, 512);
                var c = new StringBuilder(128); GetClassName(ch, c, 128);
                if (t.ToString().Contains("audio device")) mentionsAudio = true;
                if (c.ToString() == "Button" && t.ToString().Trim() == "No") noButton = ch;
                return true;
            }, IntPtr.Zero);
            if (mentionsAudio && noButton != IntPtr.Zero) {
                SendMessage(noButton, 0x00F5 /* BM_CLICK */, IntPtr.Zero, IntPtr.Zero);
                result = "dismissed";
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return result;
    }
}
"@
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$outcome = 'not-found'
while ((Get-Date) -lt $deadline) {
    $outcome = [Dlg]::Dismiss([uint32]$ProcessId)
    if ($outcome -eq 'dismissed') { break }
    if (-not (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)) { $outcome = 'process-exited'; break }
    Start-Sleep -Milliseconds 400
}
Write-Output ('audio_prompt=' + $outcome)
