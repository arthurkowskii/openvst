# Render the prepared projects with REAPER inside Arthur's interactive session.
#
# REAPER cannot run from an SSH session on Windows: the process lands in
# session 0, never initialises its engine, and neither ReaScript nor
# -renderproject produces anything. A scheduled task with an Interactive
# principal runs in the logged-on session and does work.
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
$text = $text -replace '(?m)^\[reaper\]\r?\n', "[reaper]`r`nrenderclosewhendone=0`r`nloadlastproj=0`r`n"
Set-Content -LiteralPath $ini -Value $text -Encoding ASCII -NoNewline

$names = @('fx', 'synth', 'automation')
Get-ChildItem -LiteralPath $Projects -Filter '*render.wav' -ErrorAction SilentlyContinue | Remove-Item -Force
$cmd = Join-Path $Stage 'render_windows.cmd'
$lines = @('@echo off')
foreach ($name in $names) {
    $lines += ('start "" /wait "' + $reaper + '" -newinst -nosplash -ignoreerrors -cfgfile "' + $ini + '" -renderproject "' + (Join-Path $Projects ($name + '.rpp')) + '"')
}
Set-Content -LiteralPath $cmd -Value $lines -Encoding ASCII

$task = 'OpenVST-Renders'
Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
Register-ScheduledTask -TaskName $task `
    -Action (New-ScheduledTaskAction -Execute 'cmd.exe' -Argument ('/c "' + $cmd + '"') -WorkingDirectory $Stage) `
    -Principal (New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Highest) `
    -Force | Out-Null
try {
    Start-ScheduledTask -TaskName $task
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 5
        $produced = @(Get-ChildItem -LiteralPath $Projects -Filter '*render.wav' -ErrorAction SilentlyContinue)
        if ($produced.Count -ge $names.Count) { break }
    }
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
