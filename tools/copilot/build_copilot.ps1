#Requires -Version 7
<#
Builds the standalone HandsFreeCopilot sidecar (PyInstaller, onedir) and gates it
with a smoke test: the frozen exe and the source script must print byte-identical
JSON on the sample session.

    pwsh -File tools/copilot/build_copilot.ps1

Output: build/copilot/HandsFreeCopilot/HandsFreeCopilot.exe  (build/ is gitignored)
#>
$ErrorActionPreference = 'Stop'
$PSDefaultParameterValues['*:Encoding'] = 'utf8'

function Fail([string]$msg) { Write-Host "BUILD FAILED: $msg" -ForegroundColor Red; exit 1 }
function Invoke-Checked([string]$what, [scriptblock]$cmd) {
    & $cmd
    if ($LASTEXITCODE -ne 0) { Fail "$what (exit $LASTEXITCODE)" }
}

$root    = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out     = Join-Path $root 'build\copilot'
$venv    = Join-Path $out 'venv'
$venvPy  = Join-Path $venv 'Scripts\python.exe'
$exe     = Join-Path $out 'HandsFreeCopilot\HandsFreeCopilot.exe'
$fixture = Join-Path $root 'tests\fixtures\session-sample.jsonl'
$script  = Join-Path $root 'tools\copilot\copilot.py'
$reqs    = Join-Path $PSScriptRoot 'requirements-build.txt'

New-Item -ItemType Directory -Force $out | Out-Null
if (-not (Test-Path $venvPy)) {
    Invoke-Checked 'venv creation' { python -m venv $venv }
}
Invoke-Checked 'pip install' { & $venvPy -m pip install --disable-pip-version-check -q -r $reqs }

$distDir = Join-Path $out 'HandsFreeCopilot'
if (Test-Path $distDir) { Remove-Item -Recurse -Force $distDir }

Invoke-Checked 'PyInstaller' {
    & $venvPy -m PyInstaller --onedir --name HandsFreeCopilot --console --noconfirm --clean `
        --exclude-module anthropic --paths (Join-Path $root 'tools') `
        --distpath $out --workpath (Join-Path $out 'work') --specpath $out $script
}
if (-not (Test-Path $exe)) { Fail "missing $exe" }

# --- smoke gate: compare raw bytes, never console-decoded text ---------------
function Get-StdoutBytes([string]$file, [string[]]$argList) {
    $psi = [System.Diagnostics.ProcessStartInfo]::new($file)
    foreach ($a in $argList) { $psi.ArgumentList.Add($a) }
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError  = $true
    $psi.UseShellExecute = $false
    $p = [System.Diagnostics.Process]::Start($psi)
    $ms = [System.IO.MemoryStream]::new()
    $errTask = $p.StandardError.ReadToEndAsync()
    $p.StandardOutput.BaseStream.CopyTo($ms)
    if (-not $p.WaitForExit(60000)) { $p.Kill(); Fail "timeout running $file" }
    [pscustomobject]@{ Bytes = $ms.ToArray(); Exit = $p.ExitCode; Err = $errTask.Result }
}

$frozen = Get-StdoutBytes $exe @($fixture, '--json', '--low-priority')
$source = Get-StdoutBytes 'python' @($script, $fixture, '--json')
if ($frozen.Exit -ne 0) { Fail "frozen exe exit $($frozen.Exit): $($frozen.Err)" }
if ($source.Exit -ne 0) { Fail "source script exit $($source.Exit): $($source.Err)" }
if ($frozen.Bytes.Length -eq 0) { Fail 'frozen exe printed nothing' }
if (-not [System.Linq.Enumerable]::SequenceEqual([byte[]]$frozen.Bytes, [byte[]]$source.Bytes)) {
    Fail "smoke gate: frozen stdout ($($frozen.Bytes.Length) B) differs from source ($($source.Bytes.Length) B)"
}
Write-Host "smoke gate OK: $($frozen.Bytes.Length) identical bytes, exit 0"

$mb = (Get-ChildItem $distDir -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
Write-Host ("bundle: {0} ({1:N1} MB)" -f $distDir, $mb)
exit 0
