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
$runtimeReqs = Join-Path $PSScriptRoot 'requirements-runtime.txt'

New-Item -ItemType Directory -Force $out | Out-Null
if (-not (Test-Path $venvPy)) {
    Invoke-Checked 'venv creation' { python -m venv $venv }
}
Invoke-Checked 'pip install' { & $venvPy -m pip install --disable-pip-version-check -q -r $reqs -r $runtimeReqs }

$distDir = Join-Path $out 'HandsFreeCopilot'
if (Test-Path $distDir) { Remove-Item -Recurse -Force $distDir }

Invoke-Checked 'PyInstaller' {
    & $venvPy -m PyInstaller --onedir --name HandsFreeCopilot --console --noconfirm --clean `
        --paths (Join-Path $root 'tools') `
        --distpath $out --workpath (Join-Path $out 'work') --specpath $out $script
}
if (-not (Test-Path $exe)) { Fail "missing $exe" }

# --- smoke gate: compare raw bytes, never console-decoded text ---------------
function Get-StdoutBytes([string]$file, [string[]]$argList, [hashtable]$envMap = @{}, [string]$stdinText = $null) {
    $psi = [System.Diagnostics.ProcessStartInfo]::new($file)
    foreach ($a in $argList) { $psi.ArgumentList.Add($a) }
    foreach ($k in $envMap.Keys) { $psi.Environment[$k] = [string]$envMap[$k] }
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError  = $true
    $psi.RedirectStandardInput  = ($null -ne $stdinText)
    if ($null -ne $stdinText) { $psi.StandardInputEncoding = [System.Text.UTF8Encoding]::new($false) }
    $psi.UseShellExecute = $false
    $p = [System.Diagnostics.Process]::Start($psi)
    if ($null -ne $stdinText) { $p.StandardInput.Write($stdinText); $p.StandardInput.Close() }
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

# The frozen bundle must carry the credential code: with an empty store, --has-key says 1.
$credDir = Join-Path $out 'smoke-credentials'
if (Test-Path $credDir) { Remove-Item -Recurse -Force $credDir }
New-Item -ItemType Directory -Force $credDir | Out-Null
$env:HANDSFREE_COPILOT_CREDENTIALS_DIR = $credDir
try { $hk = Get-StdoutBytes $exe @('--has-key', '--provider', 'anthropic') }
finally { Remove-Item Env:\HANDSFREE_COPILOT_CREDENTIALS_DIR -ErrorAction SilentlyContinue }
if ($hk.Exit -ne 1) { Fail "credential smoke: --has-key exit $($hk.Exit), expected 1: $($hk.Err)" }
Write-Host 'credential smoke OK: --has-key on an empty store exits 1'

# --- frozen-bundle gates: the exe must really make a request, and really import the SDK ---
$fakeServer = Join-Path $PSScriptRoot 'smoke_fake_server.py'
$utf8 = [System.Text.UTF8Encoding]::new($false)

# (a) OpenAI-compatible round trip against a local fake server (stdlib path of the bundle).
$srvPsi = [System.Diagnostics.ProcessStartInfo]::new($venvPy)
$srvPsi.ArgumentList.Add($fakeServer)
$srvPsi.RedirectStandardOutput = $true
$srvPsi.UseShellExecute = $false
$srv = [System.Diagnostics.Process]::Start($srvPsi)
try {
    $portTask = $srv.StandardOutput.ReadLineAsync()
    if (-not $portTask.Wait(10000)) { Fail 'ask gate: fake server printed no port' }
    $port = $portTask.Result.Trim()
    $askCred = Join-Path $out 'smoke-ask-credentials'
    if (Test-Path $askCred) { Remove-Item -Recurse -Force $askCred }
    New-Item -ItemType Directory -Force $askCred | Out-Null
    $r = Get-StdoutBytes $exe @($fixture, '--ask', 'Vì sao hú?', '--json', '--provider', 'openai_compatible',
                                '--base-url', "http://127.0.0.1:$port", '--model', 'smoke') `
                         @{ HANDSFREE_COPILOT_CREDENTIALS_DIR = $askCred }
    if ($r.Exit -ne 0) { Fail "ask gate: frozen exe exit $($r.Exit): $($r.Err)" }
    try { $doc = $utf8.GetString($r.Bytes) | ConvertFrom-Json } catch { Fail "ask gate: stdout is not JSON: $($_.Exception.Message)" }
    if ([string]::IsNullOrWhiteSpace($doc.answer)) { Fail 'ask gate: empty answer' }
    if ($doc.blocked -ne $false) { Fail 'ask gate: blocked is not false' }
    Write-Host "ask gate OK: frozen exe round-tripped a fake OpenAI-compatible server (answer: $($doc.answer))"
}
finally {
    if (-not $srv.HasExited) { $srv.Kill() }
    $srv.Dispose()
}

# (a2) --supervise in the bundle, against the schema-2 state fixture the C++ serializer test shares.
# The fake server's one fixed answer carries no tool call, so the run ends with the default
# no_action: that still proves state loading, the supervise loop, --deadline-s and the JSON shape.
$stateFixture = Join-Path $root 'tests\fixtures\ai-state-schema2.json'
$srvPsi = [System.Diagnostics.ProcessStartInfo]::new($venvPy)
$srvPsi.ArgumentList.Add($fakeServer)
$srvPsi.RedirectStandardOutput = $true
$srvPsi.UseShellExecute = $false
$srv = [System.Diagnostics.Process]::Start($srvPsi)
try {
    $portTask = $srv.StandardOutput.ReadLineAsync()
    if (-not $portTask.Wait(10000)) { Fail 'supervise gate: fake server printed no port' }
    $port = $portTask.Result.Trim()
    $supCred = Join-Path $out 'smoke-supervise-credentials'
    if (Test-Path $supCred) { Remove-Item -Recurse -Force $supCred }
    New-Item -ItemType Directory -Force $supCred | Out-Null
    $r = Get-StdoutBytes $exe @($fixture, '--supervise', '--state', $stateFixture, '--json',
                                '--provider', 'openai_compatible', '--base-url', "http://127.0.0.1:$port",
                                '--model', 'smoke', '--timeout', '30', '--deadline-s', '60') `
                         @{ HANDSFREE_COPILOT_CREDENTIALS_DIR = $supCred }
    if ($r.Exit -ne 0) { Fail "supervise gate: frozen exe exit $($r.Exit): $($r.Err)" }
    try { $doc = $utf8.GetString($r.Bytes) | ConvertFrom-Json } catch { Fail "supervise gate: stdout is not JSON: $($_.Exception.Message)" }
    if ($doc.supervise -ne $true -or $doc.generation -ne 7) { Fail 'supervise gate: wrong supervise / generation fields' }
    if (@($doc.actions).Count -lt 1) { Fail 'supervise gate: empty actions list' }
    Write-Host "supervise gate OK: frozen exe supervised the schema-2 fixture ($(@($doc.actions).Count) action: $($doc.actions[0].tool))"
}
finally {
    if (-not $srv.HasExited) { $srv.Kill() }
    $srv.Dispose()
}

# (a3) a state file the exe must refuse: exit 2 and {"error": ...} on stdout, no model call.
$badState = Join-Path $out 'smoke-bad-state.json'
[System.IO.File]::WriteAllText($badState, '{"schema": 1}', $utf8)
$r = Get-StdoutBytes $exe @($fixture, '--supervise', '--state', $badState, '--json',
                            '--provider', 'openai_compatible', '--base-url', 'http://127.0.0.1:9',
                            '--model', 'smoke') @{ HANDSFREE_COPILOT_CREDENTIALS_DIR = $supCred }
if ($r.Exit -ne 2) { Fail "bad-state gate: expected exit 2, got $($r.Exit): $($r.Err)" }
try { $doc = $utf8.GetString($r.Bytes) | ConvertFrom-Json } catch { Fail "bad-state gate: stdout is not JSON: $($_.Exception.Message)" }
if ([string]::IsNullOrWhiteSpace($doc.error)) { Fail 'bad-state gate: no error field' }
Write-Host 'bad-state gate OK: an invalid state file exits 2 with {"error": ...}'

# (b) `import anthropic` must work INSIDE the bundle: a connection failure is expected and
# is told apart from "SDK missing in the bundle" by the message and the absence of import errors.
$anthCred = Join-Path $out 'smoke-anthropic-credentials'
if (Test-Path $anthCred) { Remove-Item -Recurse -Force $anthCred }
New-Item -ItemType Directory -Force $anthCred | Out-Null
$credEnv = @{ HANDSFREE_COPILOT_CREDENTIALS_DIR = $anthCred }
$sk = Get-StdoutBytes $exe @('--store-key', '--provider', 'anthropic') $credEnv 'smoke-dummy-key-not-real'
if ($sk.Exit -ne 0) { Fail "sdk gate: --store-key exit $($sk.Exit): $($sk.Err)" }
$clock = [System.Diagnostics.Stopwatch]::StartNew()
$r = Get-StdoutBytes $exe @($fixture, '--ask', 'Vì sao hú?', '--provider', 'anthropic', '--timeout', '5') `
        @{ HANDSFREE_COPILOT_CREDENTIALS_DIR = $anthCred; ANTHROPIC_BASE_URL = 'http://127.0.0.1:9' }
$clock.Stop()
if ($r.Exit -ne 3) { Fail "sdk gate: expected exit 3 (connection failed), got $($r.Exit): $($r.Err)" }
foreach ($bad in @('Traceback', 'ModuleNotFoundError', 'ImportError', 'cần gói')) {
    if ($r.Err -like "*$bad*") { Fail "sdk gate: bundle is missing the SDK or a dependency ('$bad'): $($r.Err)" }
}
if ($r.Err -notlike '*kết nối*') { Fail "sdk gate: stderr is not a connection failure: $($r.Err)" }
Write-Host ("sdk gate OK: import anthropic works in the bundle (connection refused as expected, {0:N1} s)" -f $clock.Elapsed.TotalSeconds)

$mb = (Get-ChildItem $distDir -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
Write-Host ("bundle: {0} ({1:N1} MB)" -f $distDir, $mb)
exit 0
