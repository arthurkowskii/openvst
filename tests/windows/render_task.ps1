# Render the prepared projects with REAPER inside Arthur's interactive session.
#
# REAPER cannot run from an SSH session on Windows: the process lands in
# session 0, never initialises its engine, and neither ReaScript nor
# -renderproject produces anything. This script (run from SSH) prepares the
# test profile, registers an Interactive scheduled task that runs
# render_worker.ps1 in the logged-on session, waits for the WAVs, and deletes
# the task. The worker dismisses REAPER's "no audio device selected" modal on
# its own instance; that dialog is what blocks an otherwise valid render.
#
# renderclosewhendone must be 0, otherwise REAPER keeps its render window open
# and the next invocation blocks.
param(
    [Parameter(Mandatory = $true)][string]$Stage,
    [Parameter(Mandatory = $true)][string]$Projects,
    [int]$TimeoutSeconds = 240
)
$ErrorActionPreference = 'Stop'
$reaper = Join-Path ${env:ProgramFiles} 'REAPER (x64)\reaper.exe'
if (-not (Test-Path $reaper)) { throw 'REAPER not found' }
$ini = Join-Path $Stage 'profile\reaper.ini'
$text = Get-Content -LiteralPath $ini -Raw -ErrorAction SilentlyContinue
if (-not $text) { $text = "[reaper]`r`n" }
$text = $text -replace '(?m)^renderclosewhendone=.*$', ''
$text = $text -replace '(?m)^loadlastproj=.*$', ''
$text = $text -replace '(?m)^vstpath64=.*$', ''
$text = $text -replace '(?m)^\[reaper\]\r?\n', "[reaper]`r`nrenderclosewhendone=0`r`nloadlastproj=0`r`n"
# Scan the per-user installed folder only: this render must exercise the copy
# REAPER will load on this machine, not a build tree or a stale test copy.
$text = $text -replace '(?m)^\[reaper\]\r?\n', ("[reaper]`r`nvstpath64=" + (Join-Path $env:LOCALAPPDATA 'Programs\Common\VST3') + "`r`n")
Set-Content -LiteralPath $ini -Value $text -Encoding ASCII -NoNewline
Write-Output ('vstpath64=' + ((Select-String -LiteralPath $ini -Pattern '^vstpath64=').Line))

$names = @('fx', 'synth', 'automation', 'tremolo')
Get-ChildItem -LiteralPath $Projects -Filter '*render.wav' -ErrorAction SilentlyContinue | Remove-Item -Force
Remove-Item (Join-Path $Stage 'render-worker.log') -Force -ErrorAction SilentlyContinue

$task = 'OpenVST-Renders'
Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
Register-ScheduledTask -TaskName $task `
    -Action (New-ScheduledTaskAction -Execute 'powershell.exe' -Argument (
        '-NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $Stage 'render_worker.ps1') +
        '" -Stage "' + $Stage + '" -Projects "' + $Projects + '"')) `
    -Principal (New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Highest) `
    -Force | Out-Null
try {
    Start-ScheduledTask -TaskName $task
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 3
        $produced = @(Get-ChildItem -LiteralPath $Projects -Filter '*render.wav' -ErrorAction SilentlyContinue)
        if ($produced.Count -ge $names.Count) { break }
        if ((Test-Path (Join-Path $Stage 'render-worker.log')) -and
            ((Get-Content (Join-Path $Stage 'render-worker.log') -Raw) -match 'done')) { break }
    }
    $workerLog = Join-Path $Stage 'render-worker.log'
    if (Test-Path $workerLog) { Get-Content $workerLog }
    $produced = @(Get-ChildItem -LiteralPath $Projects -Filter '*render.wav' -ErrorAction SilentlyContinue)
    foreach ($item in $produced) { Write-Output ('wav=' + $item.Name + ' ' + $item.Length) }
    Write-Output ('count=' + $produced.Count)
    if ($produced.Count -lt $names.Count) { throw 'not every project rendered' }
}
finally {
    Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
    Get-CimInstance Win32_Process -Filter "Name='reaper.exe'" | ForEach-Object {
        if ($_.CommandLine -and $_.CommandLine -like '*OpenVST-test*') { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    }
}
