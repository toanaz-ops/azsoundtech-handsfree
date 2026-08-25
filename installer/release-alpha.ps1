<#
.SYNOPSIS
    Bump the patch version, build, test, package, and drop the installer where
    the alpha testers pick it up.

.DESCRIPTION
    The one command to run after a change is finished. It is deliberately a
    GATE, not a convenience: the suite has to pass before anything reaches the
    drop folder, because this is live-sound DSP and a build the team installs
    reaches a PA system.

    Order matters, and each step exists for a reason:

      1. Bump the PATCH in CMakeLists.txt. That line is the project's single
         source of version truth -- handsfree.nsi reads it with !searchparse at
         compile time, so the installer filename, PRODUCT_VERSION and the
         Apps & Features entry all follow it automatically. Nothing else has to
         be edited, and nothing can drift.
      2. Reconfigure. CMake caches the project() version; without this the
         binary keeps the old one while the installer takes the new one.
      3. Build Release.
      4. Run ctest. STOPS HERE on a failure, and the version bump is rolled
         back so a red build does not silently consume a version number.
      5. makensis.
      6. Copy to the drop folder.

.PARAMETER Part
    Which component to bump: patch (default), minor or major.

.PARAMETER DropFolder
    Where the testers collect builds. Defaults to the team's shared drive.

.PARAMETER SkipTests
    Package without running the suite. There is no good reason to use this for
    a build anyone else will install.

.EXAMPLE
    pwsh -File installer\release-alpha.ps1
    pwsh -File installer\release-alpha.ps1 -Part minor
#>

[CmdletBinding()]
param(
    [ValidateSet('patch', 'minor', 'major')]
    [string]$Part = 'patch',

    [string]$DropFolder = 'Z:\My Drive\RELEASE\ALPHA TEST',

    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'

$repo      = Split-Path $PSScriptRoot -Parent
$cmakeList = Join-Path $repo 'CMakeLists.txt'
$buildDir  = Join-Path $repo 'build'
$makensis  = 'C:\Program Files (x86)\NSIS\makensis.exe'

function Fail([string]$message) {
    Write-Host "FAIL  $message" -ForegroundColor Red
    exit 1
}

function Step([string]$message) {
    Write-Host ""
    Write-Host "==> $message" -ForegroundColor Cyan
}

if (-not (Test-Path $makensis)) {
    Fail "makensis not found at $makensis. Install NSIS: winget install --id NSIS.NSIS --exact"
}

# ── 1. bump ─────────────────────────────────────────────────────────────
Step "Bumping the $Part version"

# ReadAllText/WriteAllText with an explicit no-BOM UTF-8 encoding, NOT
# Get-Content/Set-Content: repo rule 6. CMakeLists.txt has no BOM today and
# adding one would be an invisible whole-file diff.
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$original  = [System.IO.File]::ReadAllText($cmakeList, $utf8NoBom)

$match = [regex]::Match($original, 'project\(HandsFree VERSION (\d+)\.(\d+)\.(\d+)\)')
if (-not $match.Success) {
    Fail "could not find 'project(HandsFree VERSION x.y.z)' in CMakeLists.txt. handsfree.nsi parses that exact line too, so fix it there first."
}

$major = [int]$match.Groups[1].Value
$minor = [int]$match.Groups[2].Value
$patch = [int]$match.Groups[3].Value
$from  = "$major.$minor.$patch"

switch ($Part) {
    'major' { $major++; $minor = 0; $patch = 0 }
    'minor' { $minor++; $patch = 0 }
    'patch' { $patch++ }
}

$version = "$major.$minor.$patch"
$bumped  = $original -replace 'project\(HandsFree VERSION \d+\.\d+\.\d+\)', "project(HandsFree VERSION $version)"
[System.IO.File]::WriteAllText($cmakeList, $bumped, $utf8NoBom)

Write-Host "    $from  ->  $version"

# Any failure from here on puts the version back. A red build must not eat a
# version number -- the next run would then skip one and the testers would
# wonder what happened to it.
function Restore-Version {
    [System.IO.File]::WriteAllText($cmakeList, $original, $utf8NoBom)
    Write-Host "    version rolled back to $from" -ForegroundColor Yellow
}

try {
    # ── 2. reconfigure ──────────────────────────────────────────────────
    Step "Reconfiguring (CMake caches project() VERSION)"
    cmake -B $buildDir -S $repo -G "Visual Studio 18 2026" -A x64 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

    # ── 3. build ────────────────────────────────────────────────────────
    # JUCE generates the Windows version resource ONCE, through juceaide, and
    # CMake does not consider it stale when only the version changed. Leave it
    # and the installer gets the new number while the binary inside keeps the
    # old one -- which is exactly what shipped as 1.0.1 on 2026-08-26. Deleting
    # it forces the regeneration.
    Get-ChildItem -Path $buildDir -Recurse -Filter '*_resources.rc' -ErrorAction SilentlyContinue |
        ForEach-Object { Remove-Item $_.FullName -Force }

    Step "Building Release"
    cmake --build $buildDir --config Release | ForEach-Object {
        if ($_ -match '\berror\b|LNK\d{4}') { Write-Host $_ -ForegroundColor Red }
    }
    if ($LASTEXITCODE -ne 0) {
        throw "build failed. If it is LNK1104 on the .exe, the app is still running -- close it."
    }

    # ── 4. gates ────────────────────────────────────────────────────────
    # The version the BINARY carries, not the one the filename claims. A
    # mismatch here means the resource did not regenerate, and a build whose
    # Apps & Features entry disagrees with its filename is untraceable the
    # moment a tester reports something against it.
    Step "Verifying the binary carries $version"
    $exe = Join-Path $buildDir 'HandsFree_artefacts\Release\AZ Soundtech Hands-free.exe'
    if (-not (Test-Path $exe)) { throw "built executable not found at $exe" }

    $stamped = (Get-Item $exe).VersionInfo.ProductVersion
    Write-Host "    binary reports $stamped"
    if ($stamped -notlike "$version*") {
        throw "the binary reports $stamped but this release is $version -- the version resource did not regenerate"
    }

    if ($SkipTests) {
        Write-Host ""
        Write-Host "!!  tests skipped -- do not hand this to anyone" -ForegroundColor Yellow
    }
    else {
        Step "Running the suite (this is the gate)"
        Push-Location $buildDir
        try {
            $ctest = & ctest -C Release 2>&1
            $ctest | Select-Object -Last 4
            if ($LASTEXITCODE -ne 0) { throw "ctest failed -- nothing is published" }
        }
        finally { Pop-Location }
    }

    # ── 5. package ──────────────────────────────────────────────────────
    Step "Building the installer"
    & $makensis (Join-Path $repo 'installer\handsfree.nsi') | Select-Object -Last 2
    if ($LASTEXITCODE -ne 0) { throw "makensis failed" }

    $setup = Join-Path $repo "installer\AZSoundtech-Handsfree-Setup-$version.exe"
    if (-not (Test-Path $setup)) { throw "expected $setup, which makensis did not produce" }

    # ── 6. drop ─────────────────────────────────────────────────────────
    Step "Publishing to the testers"
    if (-not (Test-Path $DropFolder)) {
        throw "drop folder not reachable: $DropFolder (is the drive mounted?)"
    }

    Copy-Item $setup -Destination $DropFolder -Force
    $dropped = Join-Path $DropFolder "AZSoundtech-Handsfree-Setup-$version.exe"

    Write-Host ""
    Write-Host "OK    $version published" -ForegroundColor Green
    Write-Host "      $dropped"
    Write-Host ""
    Write-Host "      Commit the version bump -- CMakeLists.txt is modified." -ForegroundColor Yellow
}
catch {
    Restore-Version
    Fail $_.Exception.Message
}
