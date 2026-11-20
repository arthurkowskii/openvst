# OpenVST: install or update the REAPER builds and the module packages.
#
# A VST3 bundle cannot be replaced while REAPER has it loaded (Windows holds
# the DLL, Linux keeps it mapped), so this script is designed to run BEFORE or
# AFTER a REAPER session, or to wait for REAPER to exit. It never closes a
# running DAW.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File openvst_update.ps1
#   powershell -ExecutionPolicy Bypass -File openvst_update.ps1 -WaitForReaper 3600
#   powershell -ExecutionPolicy Bypass -File openvst_update.ps1 -DryRun
#
# Exit codes: 0 success, 1 failure, 20 REAPER still running, 21 wait timed out.
param(
    [string]$Config = (Join-Path $env:LOCALAPPDATA 'OpenVST\update.json'),
    [string]$Kit,
    [string[]]$Packages,
    [int]$WaitForReaper = 0,
    [string]$ProcessName = 'reaper',
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Write-Step([string]$message) {
    Write-Host ("[openvst] " + $message)
}

function Fail([string]$message, [int]$code = 1) {
    Write-Host ("[openvst] FAILED: " + $message)
    exit $code
}

function Get-Manifest([string]$dir) {
    $base = (Get-Item -LiteralPath $dir).FullName
    @(Get-ChildItem -LiteralPath $base -Recurse -File -Force | ForEach-Object {
        [pscustomobject]@{
            file   = $_.FullName.Substring($base.Length + 1)
            bytes  = $_.Length
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        }
    } | Sort-Object file)
}

function Test-SameManifest($left, $right) {
    $a = ConvertTo-Json -InputObject @($left) -Compress -Depth 4
    $b = ConvertTo-Json -InputObject @($right) -Compress -Depth 4
    return ($a -ceq $b)
}

function Get-PackageId([string]$zipPath) {
    $zip = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        $entry = $zip.GetEntry('openvst.json')
        if ($null -eq $entry) { throw "openvst.json missing from $zipPath" }
        $reader = New-Object System.IO.StreamReader($entry.Open())
        try { $doc = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
        if (-not $doc.module -or -not $doc.module.id) { throw "module id missing from $zipPath" }
        return [pscustomobject]@{ id = [string]$doc.module.id; version = [string]$doc.module.version }
    } finally {
        $zip.Dispose()
    }
}

# ---- configuration -------------------------------------------------------

$settings = $null
if (Test-Path -LiteralPath $Config) {
    $settings = Get-Content -LiteralPath $Config -Raw | ConvertFrom-Json
}

if (-not $Kit) {
    if ($settings -and $settings.kit) { $Kit = [string]$settings.kit }
    else { $Kit = Join-Path $env:LOCALAPPDATA 'OpenVST-build' }
}
if (-not $Packages -or $Packages.Count -eq 0) {
    if ($settings -and $settings.packages) { $Packages = @($settings.packages | ForEach-Object { [string]$_ }) }
    else { $Packages = @() }
}
$vst3Dir = if ($settings -and $settings.vst3Dir) { [string]$settings.vst3Dir }
           else { Join-Path $env:LOCALAPPDATA 'Programs\Common\VST3' }
$store = if ($settings -and $settings.store) { [string]$settings.store }
         else { Join-Path $env:APPDATA 'OpenVST\modules' }

# A configuration file may use %LOCALAPPDATA% and friends so it stays portable
# between machines; literal paths are left untouched.
$Kit = [Environment]::ExpandEnvironmentVariables($Kit)
$vst3Dir = [Environment]::ExpandEnvironmentVariables($vst3Dir)
$store = [Environment]::ExpandEnvironmentVariables($store)
$Packages = @($Packages | ForEach-Object { [Environment]::ExpandEnvironmentVariables([string]$_) })

if (-not (Test-Path -LiteralPath $Kit -PathType Container)) { Fail "kit not found: $Kit" }
$probe = Join-Path $Kit 'build\Release\openvst_catalogue_probe.exe'
$runtime = Join-Path $Kit 'runtime\CmajPerformer.dll'
$fixtures = Join-Path $Kit 'fixtures'

$stateRoot = Join-Path $env:LOCALAPPDATA 'OpenVST'
$stagingRoot = Join-Path $stateRoot ('staging\' + [guid]::NewGuid().ToString('N'))
$backupRoot = Join-Path $stateRoot ('backups\' + (Get-Date).ToString('yyyyMMdd-HHmmss'))

$bundleTargets = @(
    [pscustomobject]@{ leaf = 'OpenVST FX.vst3'; artefact = 'OpenVSTFX_artefacts' },
    [pscustomobject]@{ leaf = 'OpenVST Instrument.vst3'; artefact = 'OpenVSTInstrument_artefacts' }
)

Write-Step ("kit      " + $Kit)
Write-Step ("vst3 dir " + $vst3Dir)
Write-Step ("store    " + $store)
if ($DryRun) { Write-Step 'dry run: nothing will be written' }

# ---- REAPER must not hold the DLLs ---------------------------------------

$reaperPath = $null
$processLabel = if ($ProcessName -eq 'reaper') { 'REAPER' } else { $ProcessName }
$running = @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    try { $reaperPath = $running[0].Path } catch { $reaperPath = $null }
    if ($DryRun) {
        Write-Step ($processLabel + " is running (" + $running.Count + " process(es)); a real run would wait")
    } elseif ($WaitForReaper -le 0) {
        Fail ($processLabel + " is running; close it first or pass -WaitForReaper <seconds>") 20
    } else {
        Write-Step ($processLabel + " is running; waiting up to " + $WaitForReaper + "s for it to exit (the update applies afterwards)")
        $deadline = (Get-Date).AddSeconds($WaitForReaper)
        while (@(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue).Count -gt 0 -and (Get-Date) -lt $deadline) {
            Start-Sleep -Seconds 2
        }
        if (@(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue).Count -gt 0) {
            Fail ($processLabel + " did not exit before the timeout; nothing was changed") 21
        }
        Write-Step ($processLabel + ' closed; installing now')
    }
}

# ---- modules first: they do not need the new container --------------------

New-Item -Path $stagingRoot -ItemType Directory -Force | Out-Null
$packageResults = @()
foreach ($zipPath in $Packages) {
    if (-not (Test-Path -LiteralPath $zipPath -PathType Leaf)) { Fail "package not found: $zipPath" }
    $package = Get-PackageId $zipPath
    $digest = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if (-not (Test-Path -LiteralPath $probe -PathType Leaf)) { Fail "installer probe missing: $probe" }
    if (-not (Test-Path -LiteralPath $runtime -PathType Leaf)) { Fail "Cmajor runtime missing: $runtime" }
    if (-not (Test-Path -LiteralPath $fixtures -PathType Container)) { Fail "fixtures missing: $fixtures" }
    Write-Step ("module   " + $package.id + " " + $package.version + " (" + $digest.Substring(0, 12) + ")")
    if ($DryRun) {
        $packageResults += [pscustomobject]@{ id = $package.id; version = $package.version; digest = $digest; action = 'dry-run' }
        continue
    }
    $stateOut = Join-Path $stagingRoot ($package.id + '-state.json')
    $output = & $probe --store $store --runtime $runtime --fixtures $fixtures --id $package.id --package $zipPath --state-out $stateOut 2>&1
    $exit = $LASTEXITCODE
    if ($exit -ne 0) { Fail ("module install failed for " + $package.id + " (exit " + $exit + "): " + (($output | Out-String).Trim())) }
    $marker = "OK id=" + $package.id + " digest=" + $digest
    if (-not (($output | Out-String).Contains($marker))) { Fail ("probe did not confirm " + $package.id + ": " + (($output | Out-String).Trim())) }
    $storeEntry = Join-Path $store ($package.id + '\' + $package.version + '-' + $digest + '\install.json')
    $action = if (Test-Path -LiteralPath $storeEntry) { 'unchanged' } else { 'installed' }
    $packageResults += [pscustomobject]@{ id = $package.id; version = $package.version; digest = $digest; action = $action }
}

# ---- container bundles ---------------------------------------------------

$bundleResults = @()
$swapped = @()
foreach ($target in $bundleTargets) {
    $source = Join-Path $Kit ('build\' + $target.artefact + '\Release\VST3\' + $target.leaf)
    if (-not (Test-Path -LiteralPath $source -PathType Container)) {
        Write-Step ("bundle   " + $target.leaf + " not in this kit; skipped")
        continue
    }
    $staged = Join-Path $stagingRoot $target.leaf
    Copy-Item -LiteralPath $source -Destination $staged -Recurse
    $sourceManifest = @(Get-Manifest $source)
    $stagedManifest = @(Get-Manifest $staged)
    if (-not (Test-SameManifest $sourceManifest $stagedManifest)) { Fail ("staged copy differs from the built bundle: " + $target.leaf) }

    $installed = Join-Path $vst3Dir $target.leaf
    $before = $null
    if (Test-Path -LiteralPath $installed -PathType Container) {
        $before = (Get-Manifest $installed | Where-Object { $_.file -like 'Contents\*' } | Select-Object -First 1).sha256
        $currentManifest = @(Get-Manifest $installed)
        if (Test-SameManifest $currentManifest $stagedManifest) {
            Write-Step ("bundle   " + $target.leaf + " already current; skipped")
            Remove-Item -LiteralPath $staged -Recurse -Force
            $bundleResults += [pscustomobject]@{ leaf = $target.leaf; action = 'unchanged'; before = $before; after = $before; backup = $null }
            continue
        }
    }
    $after = ($stagedManifest | Where-Object { $_.file -like 'Contents\*' } | Select-Object -First 1).sha256
    if ($DryRun) {
        Write-Step ("bundle   " + $target.leaf + " would be replaced (" + $before + " -> " + $after + ")")
        $bundleResults += [pscustomobject]@{ leaf = $target.leaf; action = 'dry-run'; before = $before; after = $after; backup = $null }
        continue
    }

    $backup = Join-Path $backupRoot $target.leaf
    if (Test-Path -LiteralPath $installed -PathType Container) {
        New-Item -Path $backupRoot -ItemType Directory -Force | Out-Null
        [System.IO.Directory]::Move($installed, $backup)
    }
    $placed = $false
    try {
        [System.IO.Directory]::Move($staged, $installed)
        $placed = $true
        $placedManifest = @(Get-Manifest $installed)
        if (-not (Test-SameManifest $placedManifest $stagedManifest)) { throw 'installed bundle does not match the staged copy' }
        if (Test-Path -LiteralPath $backup -PathType Container) {
            if (-not (Test-SameManifest (Get-Manifest $backup) $currentManifest)) { throw 'backup does not match the previous bundle' }
        }
        Write-Step ("bundle   " + $target.leaf + " installed (" + $before + " -> " + $after + ")")
        $bundleResults += [pscustomobject]@{ leaf = $target.leaf; action = 'installed'; before = $before; after = $after; backup = $backup }
        $swapped += [pscustomobject]@{ installed = $installed; backup = $backup; hadPrevious = (Test-Path -LiteralPath $backup -PathType Container) }
    } catch {
        $reason = $_.Exception.Message
        if ($placed -and (Test-Path -LiteralPath $installed)) {
            Remove-Item -LiteralPath $installed -Recurse -Force
        }
        if (Test-Path -LiteralPath $backup) { [System.IO.Directory]::Move($backup, $installed) }
        # A later failure must not leave earlier bundles on the new build while
        # the receipt says the update failed: restore every bundle this run
        # already replaced, newest first.
        foreach ($previous in @($swapped | Sort-Object { $_.installed } -Descending)) {
            if (-not (Test-Path -LiteralPath $previous.installed)) { continue }
            Remove-Item -LiteralPath $previous.installed -Recurse -Force
            if ($previous.hadPrevious -and (Test-Path -LiteralPath $previous.backup)) {
                [System.IO.Directory]::Move($previous.backup, $previous.installed)
            }
        }
        Fail ("bundle swap failed for " + $target.leaf + ": " + $reason + " (previous bundles restored)")
    }
}

# ---- receipt and relaunch ------------------------------------------------

$receipt = [pscustomobject]@{
    when      = (Get-Date).ToUniversalTime().ToString('s') + 'Z'
    host      = $env:COMPUTERNAME
    kit       = $Kit
    dryRun    = [bool]$DryRun
    bundles   = @($bundleResults)
    packages  = @($packageResults)
    reaperWasRunning = [bool]$reaperPath
    relaunched = $false
}
if (-not $DryRun) {
    New-Item -Path $stateRoot -ItemType Directory -Force | Out-Null
    $receiptPath = Join-Path $stateRoot 'last-update.json'
    ConvertTo-Json -InputObject $receipt -Depth 6 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
    Write-Step ("receipt  " + $receiptPath)
    if ($reaperPath -and (Test-Path -LiteralPath $reaperPath -PathType Leaf) -and -not $env:OPENVST_NO_RELAUNCH) {
        Write-Step ("relaunch " + $reaperPath)
        $receipt.relaunched = $true
        ConvertTo-Json -InputObject $receipt -Depth 6 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
        Start-Process -FilePath $reaperPath
    }
}
Write-Step 'done'
exit 0
