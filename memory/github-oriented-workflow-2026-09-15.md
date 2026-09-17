---
name: github-oriented-workflow
description: chuyển từ "master local là sự thật" sang "origin/main là sự thật, mọi lane qua PR"; main local trôi 107 commit không ai thấy, và CI không gác được gì vì không có gì được push
metadata:
  type: decision
---

Ngày 2026-09-15, owner chốt đổi cách quản lý commit: **`origin/main` là sự
thật**, mọi lane lên main qua Pull Request, CI phải xanh trước khi merge, `main`
local chỉ đi tới bằng `git pull --ff-only`. Quy ước đầy đủ nằm ở
`docs/GIT-WORKFLOW.md`; note này giữ lý do.

## Cái gì đã hỏng

Quy ước cũ là "master local là sự thật, merge tại chỗ, push lúc nào đó". Hậu quả
đo được trong đúng ngày chuyển:

- `main` local đứng trước `origin/main` **107 commit** (`9735a78..a6be099`).
  Không phiên nào phát hiện, vì không có phép đo nào nhìn vào origin. Bản thân
  handoff `shared/handoff/handsfree-sdd-2026-08-22.md` §"Open decisions" đã nêu
  đúng việc này từ 22/08 ("origin/main is at `cabe715` — 22 commits behind") và
  nó nằm đó không ai xử lý suốt ba tuần, chỉ trôi xa thêm.
- `.github/workflows/build.yml` tồn tại từ 22/08 và **gần như chưa gác được gì**
  — không phải vì nó sai, mà vì workflow chỉ chạy trên `push` tới `main` hoặc
  trên `pull_request`, và trong 10 ngày không có cái nào xảy ra.

PR #1 (`claude_desk/ai-integration-ideas-fc3d8d`) là Pull Request đầu tiên của
repo này.

## Cú push đầu tiên làm macOS đỏ ngay

Đẩy 107 commit lên xong, job `macos-latest` đỏ sau **2 phút 55**. Thủ phạm là
`src/app/SessionLogger.cpp` dòng 44, 45, 72, 73:

```cpp
file_      = {};
```

trên một `juce::File`. clang từ chối — *"use of overloaded operator '=' is
ambiguous (juce::File and void)"* — MSVC nhận. Code đó vào main từ lane D
(05/09/2026). Trong 10 ngày sau đó: mọi build local xanh, `ctest` **547/547**,
và **ba bản alpha đã đi tới tester**. Không phép đo nào trong số đó chạm tới
clang.

Bài học không phải "clang khó tính" mà là: **một build MSVC xanh + `ctest` xanh
KHÔNG phải là cổng CI**. Cổng CI là matrix windows + macos, và nó chỉ chạy khi
nhánh được đẩy lên. Vì thế quy ước mới bắt push nhánh ở **commit đầu tiên**, chứ
không phải khi lane xong. Fix là PR #2 (`fix/ci-macos-sessionlogger`), 4 dòng.

## PR đang xung đột thì KHÔNG có check nào

PR #1 lúc mở ra có `mergeable = CONFLICTING` (xung đột ở bảng roadmap). GitHub
không dựng được merge ref, nên workflow `pull_request` **không khởi động** và
`gh pr checks` in *"no checks reported"*. Nhìn y như CI đang chậm; thực ra CI sẽ
không bao giờ tới. Ai đợi tiếp là đợi vô hạn.

Cách gỡ (và **không** được làm bên trong worktree của phiên khác, kể cả khi phiên
đó đang rảnh — global rule 7, auto-mode cũng chặn):

```
git worktree add .claude/worktrees/<x>-merge-main -b <x>-merge-main <pr-branch>
git merge main          # trong worktree mới, giải xung đột
git push origin HEAD:<pr-branch>
```

PR tự nhận commit mới và CI chạy.

## Hệ quả về thứ tự merge

Một PR mở trên nền `main` đỏ **không thể xanh** — nó thừa hưởng lỗi ấy. Nên khi
main đỏ: merge PR sửa CI trước, rồi `gh pr update-branch <n>` cho từng PR còn
lại để chúng chạy lại trên nền đã xanh. Thứ tự của ngày 15/09: PR #2 trước, rồi
update-branch cho PR #1 và các PR sau.

## Branch protection: không bật được

Repo `toanaz-ops/azsoundtech-handsfree` là private trên **GitHub Free** — API
branch protection trả **403**. Không có gì chặn một `git push origin main` thẳng
tay. Hàng rào là quy ước tự giác + CI trên PR + **người merge phải dán output
`gh pr checks` xanh**. Nếu repo lên Pro hoặc chuyển public thì bật required
status check `build` (cả hai OS) + require PR — các bước ghi trong
`docs/GIT-WORKFLOW.md` §4.

## How to apply

1. `git fetch origin`, nhánh ra từ `origin/main` (không từ `main` local).
2. Push nhánh ở commit ĐẦU TIÊN. Mở PR sớm, kể cả `--draft`.
3. Merge bằng `gh pr merge <n> --merge --delete-branch` — **`--merge`, không
   `--squash`**: `memory/` và `shared/handoff/` trỏ vào SHA của từng task, squash
   biến mọi con trỏ đó thành rác.
4. Chạy lệnh merge từ worktree khác hoặc từ checkout gốc đang ở `main`, vì
   `--delete-branch` cố xoá chính nhánh đang checkout.
5. Về main: `git checkout main && git pull --ff-only` trong checkout gốc.
6. Release `installer\release-alpha.ps1` chạy **trên main** sau khi pull, không
   trên nhánh; commit bump `CMakeLists.txt` đi qua PR nhỏ `chore(release): 1.x.y`.
7. Dọn worktree theo [[worktree-junction-incident]] + [[worktree-prune-killed-live-session]]:
   `git worktree list`, đối chiếu phiên đang chạy, `rmdir` junction TRƯỚC.

Danh sách nhánh/worktree tồn đọng trước ngày chuyển nằm ở
`docs/GIT-WORKFLOW.md` §5 — **chờ owner duyệt từng cái**, không tự xoá.
