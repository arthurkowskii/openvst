# Install the freshly built VST3 bundles once REAPER has released them.
#
# A loaded DLL cannot be deleted or renamed by Windows, so the swap must happen
# while REAPER is closed. This script waits for that instead of asking the user
# to close and reopen twice.
#
# The bundle is copied to a staging name first, integrity-checked on the binary
# itself (not just on the file name), then moved into place. A previous silent
# failure copied the bundle inside the existing one; the explicit checks below
# exist to make that impossible to miss.
param(
    [Parameter(Mandatory = $true)][string]$Stage,
    [int]$WaitSeconds = 1800
)
$ErrorActionPreference = 'Stop'
$dest = Join-Path $env:LOCALAPPDATA 'Programs\Common\VST3'
$build = Join-Path $Stage 'build'
$names = @('OpenVST FX', 'OpenVST Instrument')

$deadline = (Get-Date).AddSeconds($WaitSeconds)
while ((Get-Date) -lt $deadline) {
    if (-not (Get-Process reaper -ErrorAction SilentlyContinue)) { break }
    Start-Sleep -Seconds 5
}
if (Get-Process reaper -ErrorAction SilentlyContinue) { throw 'REAPER stayed open, nothing installed' }

foreach ($name in $names) {
    $src = Get-ChildItem -LiteralPath $build -Filter ($name + '.vst3') -Recurse -Directory |
        Where-Object { $_.Parent.Name -eq 'VST3' } | Select-Object -First 1
    if (-not $src) { throw ('build bundle missing: ' + $name) }
    $sourceBinary = Get-ChildItem -LiteralPath $src.FullName -Recurse -File |
        Where-Object { $_.Name -eq ($name + '.vst3') } | Select-Object -First 1
    if (-not $sourceBinary) { throw ('built binary missing: ' + $name) }

    $target = Join-Path $dest ($name + '.vst3')
    $staging = Join-Path $dest ($name + '.staging.vst3')
    if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
    Copy-Item -LiteralPath $src.FullName -Destination $staging -Recurse
    $stagedBinary = Get-ChildItem -LiteralPath $staging -Recurse -File |
        Where-Object { $_.Name -eq ($name + '.vst3') } | Select-Object -First 1
    if (-not $stagedBinary) { throw ('staging copy incomplete: ' + $name) }
    $sourceHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $sourceBinary.FullName).Hash
    $stagedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $stagedBinary.FullName).Hash
    if ($sourceHash -ne $stagedHash) { throw ('staging hash mismatch: ' + $name) }

    if (Test-Path $target) { Remove-Item -Recurse -Force $target }
    Move-Item -LiteralPath $staging -Destination $target

    $installed = Get-ChildItem -LiteralPath $target -Recurse -File |
        Where-Object { $_.Name -eq ($name + '.vst3') } | Select-Object -First 1
    $text = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($installed.FullName))
    if (-not $text.Contains('Ready - trusted fixture only')) { throw ('installed binary is not the fixed build: ' + $name) }
    if ($text.Contains('Ready ?')) { throw ('installed binary still carries the old status string: ' + $name) }
    Write-Output ($name + ' installed sha256=' + (Get-FileHash -Algorithm SHA256 -LiteralPath $installed.FullName).Hash.ToLower())
}
