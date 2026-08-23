# End-to-end verification of the built installer.
#
# This is the lane's test. It really installs, really launches the app, and
# really uninstalls, on this machine. It is deliberately not a checklist: this
# project's ledger already contains a task recorded as "21/21 PASS" while
# nothing anywhere instantiated the class under test, and a checklist ticked by
# hand is that same failure mode wearing a different hat.
#
#   pwsh -File installer\verify-installer.ps1
#
# Needs elevation (the installer writes to Program Files). If run unelevated it
# re-launches itself once via UAC and relays the child's output, so the operator
# sees exactly one prompt.
#
# Exit code 0 = every assertion passed. Non-zero = the count that failed.

[CmdletBinding()]
param(
    # Internal: set when re-launching elevated, so the child can hand its
    # transcript back to the parent.
    [string]$LogFile
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# --- locate the subject, before asking for anything -----------------------
#
# Deliberately ahead of the elevation check: "you never built it" needs no
# administrator rights to discover, and popping a UAC prompt only to report it
# would train the operator to click through prompts.

$installerDir = $PSScriptRoot
$setup = Get-ChildItem -Path $installerDir -Filter 'AZSoundtech-Handsfree-Setup-*.exe' -ErrorAction SilentlyContinue |
         Sort-Object LastWriteTime -Descending | Select-Object -First 1

if (-not $setup) {
    Write-Host 'FATAL: no AZSoundtech-Handsfree-Setup-*.exe in installer\.'
    Write-Host '       Build it first:  makensis installer\handsfree.nsi'
    exit 1
}

# --- elevation ------------------------------------------------------------

$isElevated = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()
).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isElevated) {
    $log = Join-Path ([IO.Path]::GetTempPath()) "hf-verify-$PID.log"
    Write-Host 'Not elevated -- re-launching via UAC (approve the prompt).'
    # Both paths must be quoted here. -ArgumentList joins the array with spaces
    # and does NOT quote for you, and this repo lives under "DEV CAVE EP3" --
    # an unquoted path makes the child exit 64 (PowerShell's command-line
    # parsing error) before it runs a single assertion.
    $p = Start-Process -FilePath (Get-Process -Id $PID).Path `
        -ArgumentList @('-NoProfile', '-File', "`"$PSCommandPath`"", '-LogFile', "`"$log`"") `
        -Verb RunAs -Wait -PassThru
    if (Test-Path $log) {
        Get-Content $log | Write-Host
        Remove-Item $log -Force
    }
    exit $p.ExitCode
}

if ($LogFile) { Start-Transcript -Path $LogFile -Force | Out-Null }

# --- harness --------------------------------------------------------------

$script:Pass = 0
$script:Fail = 0

function Check([string]$What, [scriptblock]$Predicate) {
    $ok = $false
    try { $ok = [bool](& $Predicate) } catch { $ok = $false }
    if ($ok) { $script:Pass++; Write-Host "  PASS  $What" }
    else     { $script:Fail++; Write-Host "  FAIL  $What" }
}

function Wait-Until([scriptblock]$Predicate, [int]$TimeoutSec = 60) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        try { if (& $Predicate) { return $true } } catch { }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

# --- subjects under test --------------------------------------------------

$installDir = Join-Path $env:ProgramFiles 'AZSoundtech\HandsFree'
$appExe     = Join-Path $installDir 'AZ Soundtech Hands-free.exe'
$uninstExe  = Join-Path $installDir 'Uninstall.exe'
$uninstKey  = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\AZSoundtechHandsFree'
$startMenu  = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\AZ Soundtech'
$deskLnk    = Join-Path ([Environment]::GetFolderPath('CommonDesktopDirectory')) 'AZ Soundtech Hands-free.lnk'

Write-Host ''
Write-Host ("Installer : {0}  ({1:N0} bytes)" -f $setup.Name, $setup.Length)
Write-Host "Target    : $installDir"
Write-Host ''

# --- precondition: nothing installed --------------------------------------

if ((Test-Path $uninstExe)) {
    Write-Host 'Pre-existing install found -- removing it first.'
    Start-Process -FilePath $uninstExe -ArgumentList '/S' -Wait
    Wait-Until { -not (Test-Path $installDir) } 60 | Out-Null
}

# --- install --------------------------------------------------------------

Write-Host 'INSTALL (silent)'
$proc = Start-Process -FilePath $setup.FullName -ArgumentList '/S' -Wait -PassThru
Check "setup exits 0 (got $($proc.ExitCode))" { $proc.ExitCode -eq 0 }

Check 'app exe installed to Program Files'  { Test-Path -LiteralPath $appExe }
Check 'uninstaller written'                 { Test-Path -LiteralPath $uninstExe }
Check 'install dir is under Program Files (not VirtualStore)' {
    $appExe -like "$env:ProgramFiles\*" -and (Test-Path -LiteralPath $appExe)
}

# The registry entry is what makes the app appear in Apps & Features. NSIS is
# a 32-bit process, so the script must SetRegView 64 to land here rather than
# under WOW6432Node -- this assertion is what proves it did.
Check 'Apps & Features entry exists in the 64-bit registry view' { Test-Path $uninstKey }
if (Test-Path $uninstKey) {
    $r = Get-ItemProperty $uninstKey
    Check "  DisplayName    = 'AZ Soundtech Hands-free'" { $r.DisplayName -eq 'AZ Soundtech Hands-free' }
    Check '  DisplayVersion is set'                      { -not [string]::IsNullOrWhiteSpace($r.DisplayVersion) }
    Check "  Publisher      = 'AZ Soundtech'"            { $r.Publisher -eq 'AZ Soundtech' }
    Check '  UninstallString points at a real file'      { Test-Path -LiteralPath ($r.UninstallString -replace '"','') }
    Write-Host "        (DisplayVersion = $($r.DisplayVersion), EstimatedSize = $($r.EstimatedSize) KB)"
}

# A shortcut whose target does not resolve is the classic symptom of the
# VirtualStore redirection that RequestExecutionLevel admin exists to prevent.
$shell = New-Object -ComObject WScript.Shell
Check 'Start Menu shortcut exists'   { Test-Path (Join-Path $startMenu 'AZ Soundtech Hands-free.lnk') }
Check 'Start Menu shortcut resolves to the installed exe' {
    $shell.CreateShortcut((Join-Path $startMenu 'AZ Soundtech Hands-free.lnk')).TargetPath -eq $appExe
}
Check 'Start Menu uninstall shortcut exists' { Test-Path (Join-Path $startMenu 'Uninstall AZ Soundtech Hands-free.lnk') }
Check 'Desktop shortcut exists'      { Test-Path -LiteralPath $deskLnk }

# --- the installed app actually runs --------------------------------------
#
# This is the assertion that catches a missing VC++ runtime. A binary whose
# imports cannot be resolved dies during loading, so "still alive after a few
# seconds" is direct evidence the redistributable step did its job.

Write-Host ''
Write-Host 'LAUNCH'
$app = $null
try {
    $app = Start-Process -FilePath $appExe -PassThru
    Start-Sleep -Seconds 6
    $app.Refresh()
    Check 'installed app is still running after 6 s (imports resolved)' { -not $app.HasExited }
    if ($app.HasExited) {
        Write-Host ("        exit code 0x{0:X8} -- 0xC0000135 means a DLL was missing" -f $app.ExitCode)
    }
} finally {
    # Wait for the process to be genuinely gone before uninstalling. Stop-Process
    # returns as soon as termination is *requested*; Windows can still hold the
    # image open for a moment afterwards, and the uninstaller would then fail to
    # delete it. The uninstaller now survives that case via /REBOOTOK, but the
    # assertions below are about the ordinary path -- app closed, then removed --
    # so make the test actually exercise that path rather than a race.
    if ($app -and -not $app.HasExited) {
        Stop-Process -Id $app.Id -Force
        $app.WaitForExit(15000) | Out-Null
    }
    Wait-Until { -not (Get-Process -Id $app.Id -ErrorAction SilentlyContinue) } 15 | Out-Null
    Start-Sleep -Seconds 1
}

# --- uninstall ------------------------------------------------------------
#
# Run the real user path: "/S" with no _?= . NSIS then copies the uninstaller
# to %TEMP% and returns immediately, so -Wait proves nothing; poll for the
# directory to actually disappear instead. Using _?=$INSTDIR would make it
# wait, but it also tells NSIS to skip deleting itself -- which would let a
# broken uninstaller pass this test.

Write-Host ''
Write-Host 'UNINSTALL (silent)'
Start-Process -FilePath $uninstExe -ArgumentList '/S' -Wait
$gone = Wait-Until { -not (Test-Path -LiteralPath $appExe) } 60

Check 'app exe removed'                    { -not (Test-Path -LiteralPath $appExe) }
Check 'install directory removed'          { -not (Test-Path -LiteralPath $installDir) }
Check 'Apps & Features entry removed'      { -not (Test-Path $uninstKey) }
Check 'vendor registry key removed'        { -not (Test-Path 'HKLM:\SOFTWARE\AZ Soundtech\HandsFree') }
Check 'Start Menu folder removed'          { -not (Test-Path $startMenu) }
Check 'Desktop shortcut removed'           { -not (Test-Path -LiteralPath $deskLnk) }

# --- report ---------------------------------------------------------------

Write-Host ''
Write-Host ('=' * 60)
Write-Host "  $script:Pass passed, $script:Fail failed"
Write-Host ('=' * 60)

if ($LogFile) { Stop-Transcript | Out-Null }
exit $script:Fail
