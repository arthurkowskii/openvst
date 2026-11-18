# Install the freshly built VST3 bundles as soon as no REAPER process holds the
# DLLs, then leave a marker. A loaded DLL cannot be replaced, so this waits
# instead of asking the user to close and reopen twice.
param(
    [Parameter(Mandatory = $true)][string]$Stage,
    [int]$WaitMinutes = 480
)
$ErrorActionPreference = 'Stop'
$marker = Join-Path $Stage 'install-waiter-status.txt'
$deadline = (Get-Date).AddMinutes($WaitMinutes)
Set-Content -LiteralPath $marker -Value ('armed ' + (Get-Date -Format o)) -Encoding ASCII
while ((Get-Date) -lt $deadline) {
    if (-not (Get-Process reaper -ErrorAction SilentlyContinue)) { break }
    Start-Sleep -Seconds 10
}
if (Get-Process reaper -ErrorAction SilentlyContinue) {
    Add-Content -LiteralPath $marker -Value 'gave-up reaper still open'
    exit 1
}
try {
    $output = & (Join-Path $Stage 'install_vst3_catalogue.ps1') -Stage $Stage -WaitSeconds 60 2>&1
    Add-Content -LiteralPath $marker -Value 'installed'
    Add-Content -LiteralPath $marker -Value ($output | Out-String)
} catch {
    Add-Content -LiteralPath $marker -Value ('error: ' + $_.Exception.Message)
    exit 2
}
