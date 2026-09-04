### Task C1: Prune verify-after-delete (Google Drive xóa async)

**Files:** Modify `installer/release-alpha.ps1` (khối prune, ~dòng 234-259)

Sự cố 27/08: `Remove-Item` trên `Z:\` (Google Drive) báo thành công nhưng
`...-1.0.0.exe` vẫn nằm lại, phải xóa tay.

- [ ] **Step 1:** sau mỗi Remove-Item trong prune, thêm:

```powershell
        Start-Sleep -Milliseconds 500
        if (Test-Path $stale.FullName) {
            Write-Host "WARN  $($stale.Name) still present after delete (Drive sync lag?) -- retrying once"
            Remove-Item $stale.FullName -Force
            Start-Sleep -Milliseconds 1500
            if (Test-Path $stale.FullName) {
                Write-Host "WARN  $($stale.Name) would not delete -- tidy the drop folder by hand"
            }
        }
```

- [ ] **Step 2:** verify parse (`ParseFile` như lần trước) + mô phỏng bằng
  thư mục tạm local. KHÔNG chạy script thật.
- [ ] **Step 3:** commit.

## IMPORTANT — actual code does not match the snippet variable name verbatim

The plan snippet above was written against a `$stale` loop variable. Read
the real code at `installer/release-alpha.ps1` lines 234-259 first: the
prune block's `foreach` loop currently reads:

```powershell
        foreach ($old in $stale) {
            Remove-Item -LiteralPath $old.FullName -Force
            Write-Host "    pruned $($old.Name)"
        }
```

`$stale` is the array of ALL stale files (from the `Get-ChildItem | Sort-Object |
Select-Object -Skip $Keep` pipeline above it); the loop variable for the
CURRENT item being deleted is `$old`. Adapt the snippet's variable name
accordingly — use `$old` (not `$stale`) to refer to the single file just
removed, and keep using `-LiteralPath` on `Remove-Item`/`Test-Path` to match
the existing style in this file (the existing `Remove-Item -LiteralPath` call
does; use `-LiteralPath` on your `Test-Path` and retry `Remove-Item` calls
too, for consistency and to avoid wildcard-character mis-interpretation in
filenames).

The intent (per the plan): after removing each stale file, wait 500ms and
check with `Test-Path` whether it is still there (Google Drive sync lag can
make `Remove-Item` report success on a file that reappears). If still
present, retry the delete once, wait 1500ms more, and warn if it STILL
would not go — but never throw. This whole block stays inside the existing
`foreach` loop, which itself is inside the existing outer `try { } catch { }`
in section 7 (`# ── 7. prune`) — so an exception from the retry `Remove-Item`
must not escape uncaught in a way that changes today's behavior: today the
outer `catch` at line 255 turns ANY prune exception into a `WARN` and
lets the script continue (a prune failure is deliberately non-fatal, per the
comment above the block). Keep that property: it's fine for a genuinely
failed final retry to throw and be swallowed by the existing outer catch,
since that already produces a WARN — but the intended common path (the
retry succeeds, or the file was just slow to disappear) should not throw at
all.

## Step 2 detail — how to verify WITHOUT running the real script

Task C1 explicitly forbids running `release-alpha.ps1` for real (it has
side effects: version bump, full Release build+ctest, NSIS packaging, and
would try to publish to `Z:\My Drive\RELEASE\ALPHA TEST`). Verify with:

1. **Syntax/parse check** — confirm the edited file still parses as valid
   PowerShell without executing it:
   ```powershell
   $errors = $null
   [System.Management.Automation.Language.Parser]::ParseFile(
       (Resolve-Path 'installer/release-alpha.ps1'), [ref]$null, [ref]$errors) | Out-Null
   if ($errors) { $errors } else { 'parse OK' }
   ```
   (This is literally `[Parser]::ParseFile` — the same technique used the
   last time this file was hardened, per the plan text "ParseFile như lần
   trước". Search git log / this file's own history if you want the exact
   prior invocation style, but the above is sufficient.)

2. **Logic simulation in a local temp dir** — extract JUST the retry logic
   (or a short stand-in script that dot-sources/re-implements the same
   Test-Path/Remove-Item/retry shape) and run it against a scratch directory
   under the OS temp dir (e.g. `Join-Path $env:TEMP "sdd-c1-verify-<random>"`),
   NOT `Z:\` and NOT the real drop folder. Create a few dummy `*.exe` files
   there, delete one, and confirm your simulated retry path behaves as
   expected (no exception when the file to test is genuinely gone once
   Remove-Item succeeds — that's the normal case — and the WARN path
   triggers if you simulate a file that reappears, e.g. by re-creating it in
   your test harness between the first Remove-Item and the Test-Path check).
   This is a throwaway PowerShell snippet you write for verification only —
   it does not need to become a permanent test file in the repo, and this
   task's plan text explicitly says "không cần test riêng" is NOT said here
   (that line is from a different task, T1) — but the plan does NOT ask you
   to add a Pester test either; a manual, reported verification transcript
   is sufficient. Paste the PowerShell you ran and its output into your
   report.
3. Clean up the scratch temp dir afterward.

## Step 3 — commit

`git add installer/release-alpha.ps1` then commit with an explicit path
(never `git add -A` / `git add .`). Suggested message:
`build(installer): retry prune delete once for Drive sync lag`
(the plan's own task title is "Prune verify-after-delete (Google Drive xóa
async)" — pick a concise, accurate message; it does not have to match this
suggestion verbatim).

## Report contract

Write your full report (what you changed, the exact diff region, the
ParseFile output, the temp-dir simulation transcript and its output, and
the commit hash) to the report file path given to you in the dispatch
message. Return to the dispatcher only: status (DONE /
DONE_WITH_CONCERNS / NEEDS_CONTEXT / BLOCKED), the commit hash(es), a
one-line verification summary, and any concerns.
