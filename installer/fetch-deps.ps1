# Downloads the installer's bundled dependencies into installer\vendor\.
#
# Why this is a script and not a committed binary: vc_redist.x64.exe is 24.4 MB
# of incompressible signed binary. Committing it would add that to every clone
# of this repo forever, and git cannot delta-compress it between versions. So
# vendor\ is gitignored and fetched on demand, with the hash pinned below so a
# fetch either produces the exact byte-for-byte file we verified or fails.
#
# Run once before the first `makensis installer\handsfree.nsi`. Re-running is
# cheap: an already-correct file is left alone.
#
#   pwsh -File installer\fetch-deps.ps1

[CmdletBinding()]
param(
    # Force a re-download even if the local file already matches the hash.
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$vendorDir = Join-Path $PSScriptRoot 'vendor'

# aka.ms/vs/17/release/vc_redist.x64.exe is Microsoft's permanent "latest
# supported VC++ 2015-2022 x64 redistributable" link. It is a moving target by
# design, so the hash below records the exact build this project has verified.
# When Microsoft ships a newer one the hash check fails LOUDLY -- that is the
# point. Re-verify the new file, then update Sha256 and Version here in one
# commit, so the change is reviewable rather than silent.
$deps = @(
    @{
        Name    = 'vc_redist.x64.exe'
        Uri     = 'https://aka.ms/vs/17/release/vc_redist.x64.exe'
        Sha256  = 'CC0FF0EB1DC3F5188AE6300FAEF32BF5BEEBA4BDD6E8E445A9184072096B713B'
        Version = '14.44.35211.0'
        Bytes   = 25635768
    }
)

if (-not (Test-Path $vendorDir)) {
    New-Item -ItemType Directory -Path $vendorDir | Out-Null
}

foreach ($dep in $deps) {
    $dest = Join-Path $vendorDir $dep.Name

    if ((Test-Path $dest) -and -not $Force) {
        $have = (Get-FileHash $dest -Algorithm SHA256).Hash
        if ($have -eq $dep.Sha256) {
            Write-Host "OK    $($dep.Name)  (already present, hash matches)"
            continue
        }
        Write-Host "STALE $($dep.Name)  (hash mismatch, re-downloading)"
    }

    Write-Host "GET   $($dep.Name)  <- $($dep.Uri)"
    Invoke-WebRequest -Uri $dep.Uri -OutFile $dest -UseBasicParsing

    $have = (Get-FileHash $dest -Algorithm SHA256).Hash
    if ($have -ne $dep.Sha256) {
        # Leave the bad file on disk so it can be inspected, but make very sure
        # nothing downstream treats it as verified.
        throw @"
Hash mismatch for $($dep.Name).

  expected  $($dep.Sha256)
  got       $have

Microsoft has almost certainly published a newer redistributable behind the
same aka.ms link. Verify the new file, then update Sha256/Version/Bytes in
this script in a single reviewable commit. Do NOT relax the check.
"@
    }

    $size = (Get-Item $dest).Length
    Write-Host ("OK    {0}  ({1:N0} bytes, v{2})" -f $dep.Name, $size, $dep.Version)
}

Write-Host ''
Write-Host 'Dependencies ready. Next:  makensis installer\handsfree.nsi'
