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
    # The status line is "Ready - " + the module id since module selection
    # landed, so the old fixed sentence no longer exists. Check what the build
    # must contain instead of a sentence that has moved on: the second module
    # and its permanent automation id prove this is the build under test.
    $text = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($installed.FullName))
    if (-not $text.Contains('Ready - ')) { throw ('installed binary is not the fixed build: ' + $name) }
    # Strings that only the catalogue commit contains: the permanent generic
    # slots, the declarative UI schema and the Discover panel copy. A stale
    # bundle from an earlier commit has none of them.
    if (-not $text.Contains('openvst.ui/0')) { throw ('installed binary lacks the module UI build: ' + $name) }
    if (-not $text.Contains('Press Refresh to list approved modules.')) { throw ('installed binary lacks the catalogue panel: ' + $name) }
    if ($name -eq 'OpenVST FX' -and -not ($text.Contains('openvst.fx.module.8.v1') -and $text.Contains('openvst.ts.attack.v1'))) {
        throw ('installed FX binary lacks the catalogue parameter build: ' + $name)
    }
    if ($name -eq 'OpenVST Instrument' -and -not $text.Contains('openvst.instr.module.8.v1')) {
        throw ('installed Instrument binary lacks the catalogue parameter build: ' + $name)
    }
    if ($text.Contains('Ready ?')) { throw ('installed binary still carries the old status string: ' + $name) }
    Write-Output ($name + ' installed sha256=' + (Get-FileHash -Algorithm SHA256 -LiteralPath $installed.FullName).Hash.ToLower())
}
