# Quy ước Git — `origin/main` là sự thật

**Chốt ngày 2026-09-15, owner ToanAZ.** Trước ngày này quy ước là "master local
là sự thật, merge tại chỗ, push lúc nào đó". Nó hỏng theo đúng cách ai cũng đoán
được: `main` local trôi **107 commit** trước `origin/main` (`9735a78..a6be099`)
mà không phiên nào thấy, và CI trong `.github/workflows/build.yml` — viết từ
2026-08-22 — gần như chưa gác được gì, vì không có gì để gác. Hôm nay main đã
được đẩy lên origin, và PR #1 (`claude_desk/ai-integration-ideas-fc3d8d`) là
Pull Request đầu tiên của repo này. Cú push ấy lập tức làm job macOS đỏ vì một
lỗi đã nằm trong source 10 ngày (xem §6).

Tài liệu này là **nguồn duy nhất** cho quy ước commit/merge. `CLAUDE.md` mục
"Definition of done" và `memory/MEMORY.md` mục Conventions trỏ về đây, không
chép lại.

Phần **cơ khí** của worktree (junction JUCE, `git config --worktree`, thư mục
build ngoài repo) không nằm ở đây — nó ở
[`docs/superpowers/runbooks/parallel-lanes.md`](superpowers/runbooks/parallel-lanes.md).
Tài liệu này nói *khi nào* và *theo đường nào*; runbook kia nói *làm thế nào cho
worktree không nổ*.

---

## 1. Nguyên tắc

1. **`origin/main` là sự thật.** Không phải `main` local, không phải nhánh nào
   trên máy này, không phải một worktree "đã merge rồi".
2. **`main` local chỉ được đi tới bằng `git pull --ff-only`.** Không bao giờ
   `git merge <lane>` vào `main` local nữa. `--ff-only` chính là cái chốt: nếu
   nó từ chối, nghĩa là main local đã có commit mà origin không có — dừng lại và
   hỏi, đừng ép.
3. **Không commit thẳng lên `main`**, kể cả docs, kể cả một dòng. Mọi thay đổi đi
   qua một nhánh và một Pull Request.
4. **CI xanh trước khi merge.** `.github/workflows/build.yml` chạy trên
   `pull_request` tới `main` (và `develop`), matrix `windows-latest` +
   `macos-latest`, build Release rồi `ctest`. Khoảng 25 phút. Đây là **lần duy
   nhất** code này được dịch bằng clang — máy phát triển chỉ có MSVC, và §6 kể
   chuyện điều đó đã che một lỗi suốt 10 ngày.
5. **Người merge phải dán bằng chứng.** Không có branch protection (xem §4), nên
   hàng rào duy nhất là output `gh pr checks` xanh dán vào PR hoặc vào báo cáo.
   Đây là live-sound DSP: một build lọt qua sẽ đi tới PA thật.

---

## 2. Vòng đời một lane

### Bước 1 — Lấy sự thật về trước

```bash
git fetch origin
```

Luôn nhánh ra từ `origin/main`, **không** từ `main` local. Main local có thể
đang lạc hậu, và một nhánh dựng trên nền lạc hậu sẽ kéo theo merge thừa vào PR.

### Bước 2 — Worktree riêng cho lane

```bash
git worktree add .claude/worktrees/<lane>-<mmdd> -b <type>/<lane> origin/main
```

`<type>` là `feat` / `fix` / `docs` / `chore` / `refactor`, khớp tiền tố commit.
Một phiên = một nhánh = một worktree.

Worktree mới **thiếu** những thứ bị gitignore — làm theo
[`parallel-lanes.md`](superpowers/runbooks/parallel-lanes.md) §2–§3 trước khi
build, và xem §6 bên dưới về `external/asiosdk` + `installer/vendor`.

### Bước 3 — Commit nhỏ, đường dẫn tường minh

```bash
git add docs/GIT-WORKFLOW.md
```

Không bao giờ `git add .`, không bao giờ `git add -A`. Lý do không phải là sạch
sẽ: `git commit -- <path>` từng commit nguyên working-tree file và kéo hunk của
lane khác lọt vào (`memory/gui-console-lessons-2026-08-24.md`). Một việc một
commit, tiền tố `feat:` / `fix:` / `docs:` / `chore:` / `refactor:`.

### Bước 4 — Push **sớm**, ngay commit đầu

```bash
git push -u origin <type>/<lane>
```

Đẩy ngay commit đầu tiên chứ không đợi lane xong. Nhánh có trên origin → PR mở
được sớm → CI chạy trên từng push, thay vì dồn 25 phút vào đúng phút cuối khi
mọi người đang đợi merge.

Đây **không** phải chuyện tiện tay. Workflow chỉ chạy trên `push` tới `main`
hoặc trên `pull_request`; một nhánh chưa đẩy thì matrix CI **chưa từng chạy một
lần nào** trên code của nó. Xem §6, bẫy đầu tiên.

### Bước 5 — Mở PR

```bash
gh pr create --base main --title "<type>: <tóm tắt>" --body-file <file>
```

Template `.github/PULL_REQUEST_TEMPLATE.md` tự nạp khi mở PR trên web; mở bằng
`gh` thì điền đúng các mục đó vào body. PR nháp cũng được (`--draft`) nếu lane
còn dài — CI vẫn chạy.

### Bước 6 — Đợi CI

```bash
gh pr checks <n> --watch
```

Dán output xanh vào PR hoặc vào báo cáo. **CI xanh không thay thế `ctest` local**
(và ngược lại) — xem §6 về ASIO. Nếu lệnh in *"no checks reported"*, đừng đợi
tiếp: PR đang xung đột và GitHub không dựng được merge ref, xem §6 bẫy thứ hai.

### Bước 7 — Review

Verifier độc lập **đọc file thật**, không đọc báo cáo của agent đã làm. Cùng một
luật với `docs/superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md` mục "Mỗi
lane phải đạt" điểm 2. Không ai tự chấm bài mình.

### Bước 8 — Merge

```bash
gh pr merge <n> --merge --delete-branch
```

**`--merge`, không `--squash`.** Lịch sử lane có giá trị ở repo này: các note
trong `memory/` và các handoff trong `shared/handoff/` trỏ vào **SHA của từng
task** ("fix round Task 9 (`b8e3f25`)", "lane G 40 commit"). Squash biến mọi con
trỏ đó thành rác cùng một lúc. Merge commit giữ được cả hai: dòng chảy trên
`main`, và từng bước bên trong lane.

Chỉ merge khi **owner nói "merge"** trong hội thoại. Agent không tự merge.

**Khi `main` đang đỏ, thứ tự merge không tự do.** Một PR mở trên nền main đỏ
không thể xanh — nó thừa hưởng cái lỗi ấy. Phải merge PR sửa CI **trước**, rồi
với từng PR còn lại chạy

```bash
gh pr update-branch <n>
```

để nó kéo main mới về và CI chạy lại trên nền đã xanh. Thứ tự của ngày
2026-09-15: **PR #2** (`fix/ci-macos-sessionlogger`) trước, rồi `update-branch`
cho PR #1 và các PR sau.

### Bước 9 — Về main

Trong **checkout gốc** (`D:\DEV CAVE EP3\PROJECT005-AZ-handsfree`), không phải
trong worktree lane:

```bash
git checkout main && git pull --ff-only
```

### Bước 10 — Dọn worktree, theo quy trình an toàn

Đây là bước nguy hiểm nhất của cả vòng đời, và nó đã nổ **ba lần** trên dự án
này. Đọc trước khi gõ:

- [`memory/worktree-junction-incident-2026-08-23.md`](../memory/worktree-junction-incident-2026-08-23.md)
  — `git worktree remove --force` **đi theo junction** `external/JUCE` và xoá
  sạch checkout JUCE dùng chung của main repo lẫn mọi lane khác. Phải `rmdir`
  junction TRƯỚC, rồi chứng minh `external/JUCE/modules` còn nội dung, rồi mới
  remove.
- [`memory/worktree-prune-killed-live-session-2026-09-05.md`](../memory/worktree-prune-killed-live-session-2026-09-05.md)
  — xoá worktree "stale" theo **tên thư mục** đã phá worktree đang SỐNG của một
  phiên khác (desktop app tái dùng tên cũ cho nhánh mới). Tên thư mục không đáng
  tin. `rm` báo *"Device or resource busy"* nghĩa là có tiến trình đang giữ file
  → DỪNG, không retry, không prune.

Thứ tự bắt buộc: `git worktree list` để lấy path + nhánh thật (đừng tin cái
tên) → đối chiếu `cwd` của mọi phiên `isRunning` → kiểm tra junction và `rmdir`
nó → mới remove. Không chắc thì **hỏi owner**, đừng dọn.

---

## 3. Release

`installer\release-alpha.ps1` chạy **trên `main` local sau khi đã
`git pull --ff-only`** PR vừa merge về — không chạy trên nhánh lane. Lý do:
script bump PATCH trong `CMakeLists.txt`, build Release, lấy `ctest` làm cổng,
đóng gói NSIS rồi chép vào `Z:\My Drive\RELEASE\ALPHA TEST`. Một build đi tới
tester phải đúng là thứ đang đứng trên `main`, không phải một nhánh chưa hạ cánh.

```bash
pwsh -File installer\release-alpha.ps1
```

Script sửa `CMakeLists.txt` (dòng `project(HandsFree VERSION x.y.z)`). Commit
bump đó **đi qua một PR nhỏ**, `chore(release): 1.x.y`.

**Đã chọn: PR nhỏ, không commit thẳng.** Phương án kia — coi bump một dòng là
ngoại lệ được push thẳng lên `main` — gọn hơn đúng một lần, rồi mở lại đúng cái
cửa mà quy ước này sinh ra để đóng: một khi có một loại commit được đi thẳng,
ranh giới thành chuyện tự đánh giá. Và bump *có thể* phá build —
`CMakeLists.txt` là file mà theo `CLAUDE.md` bắt buộc reconfigure + build đầy
đủ. Để CI kiểm một dòng ấy tốn 25 phút máy và không tốn phút người nào.

Ngoại lệ duy nhất: khi `release-alpha.ps1` **rollback** version vì suite đỏ —
lúc đó không có gì để commit, và cũng không có gì để release.

`-SkipTests` build vào `installer\dist-local\` và không bao giờ chạm drop folder.
Không dùng cho bất kỳ build nào rời khỏi máy này.

---

## 4. Branch protection — vì sao chưa bật

Repo `toanaz-ops/azsoundtech-handsfree` là **private trên GitHub Free**. API
branch protection trả **403**: gói này không cho bật required review / required
status check trên nhánh của repo private. Nghĩa là:

- Không có gì **chặn** một `git push origin main` thẳng tay.
- Hàng rào là **quy ước tự giác + CI trên PR + người merge dán bằng chứng**.
- Ai merge (người hay agent) phải dán output `gh pr checks <n>` xanh. Không có
  output thì không có bằng chứng, và chưa có bằng chứng thì chưa merge. Đây
  đúng là global rule 1: chưa chạy thì nói "not verified".

**Nếu repo lên GitHub Pro, hoặc chuyển sang public** (cả hai đều mở khoá branch
protection), bật ngay:

1. Settings → Branches → Add branch protection rule, pattern `main`.
2. Bật **Require a pull request before merging** (1 approval).
3. Bật **Require status checks to pass before merging**, chọn check **`build`**
   cho cả `windows-latest` và `macos-latest`.
4. Bật **Require branches to be up to date before merging**.
5. Không bật "Allow force pushes", không bật "Allow deletions".
6. Cập nhật lại mục này để nói rằng nó đã bật — hàng rào tự giác thành hàng rào
   thật, và người merge hết phải dán tay.

---

## 5. Phiên song song và worktree cũ — **chờ người duyệt, không tự xoá**

Ngày 2026-09-15 máy này còn tồn đọng nhánh đã merge và worktree cũ. Đây là
**danh sách chờ owner duyệt từng cái một**, không phải việc để agent tự dọn. Lý
do đứng ngay ở §2 bước 10: tên thư mục không đáng tin, và một trong số này từng
là worktree sống của phiên khác.

| Nhánh local | Tình trạng | Ghi chú |
|---|---|---|
| `claude_desk/lane-d-data-loop-239597` | đã merge main | không còn worktree trỏ vào |
| `claude_desk/merge-branches-subagent-c76c7f` | đã merge main | không còn worktree trỏ vào |
| `claude_desk/lane-g-brainstorm-sdd-f3c568` | đã merge main (`68dd7ef`) | worktree cùng tên nay đang giữ **nhánh khác** |
| `feat/data-loop` | đã merge main (`e9da9c6`) | worktree `peakiness-sweep-tool-c81129` đang trỏ vào |

| Worktree | Nhánh đang giữ | Ghi chú |
|---|---|---|
| `.claude/worktrees/peakiness-sweep-tool-c81129` | `feat/data-loop` | nhánh đã merge, worktree còn sống |
| `.claude/worktrees/chore-infra-prune-orphans-948d0e` | `claude_desk/ai-integration-ideas-fc3d8d` | **đang là nhánh của PR #1** — tên thư mục nói "prune-orphans", ruột là lane khác. Đúng cái bẫy của `worktree-prune-killed-live-session`. |

Quy tắc: **không xoá nhánh hay worktree nào trong bảng này nếu owner không gọi
đúng tên nó.** "Dọn hết mấy cái cũ đi" không phải là duyệt.

Và không bao giờ **làm việc bên trong worktree của phiên khác**, kể cả khi phiên
đó đang rảnh: đó là shared resource theo global rule 7, và auto-mode cũng chặn.
Cần sửa nhánh của PR người khác thì dựng worktree mới — xem §6.

Từ nay `gh pr merge --delete-branch` xoá nhánh ngay lúc merge, nên bảng này
không dài thêm.

---

## 6. Bẫy đã biết

**Build local MSVC xanh KHÔNG phải là cổng CI.** Ngày 2026-09-15, cú push đầu
tiên sau 10 ngày làm job `macos-latest` đỏ sau 2 phút 55. Thủ phạm:
`src/app/SessionLogger.cpp` dòng 44, 45, 72, 73 — `file_ = {};` trên một
`juce::File`, clang từ chối (*"use of overloaded operator '=' is ambiguous
(juce::File and void)"*), MSVC nhận. Code đó vào main từ lane D (05/09/2026).
Trong 10 ngày ấy **mọi** build local xanh, `ctest` 547/547, và **ba bản alpha đã
đi tới tester** — không phép đo nào trong số đó chạm tới clang, vì matrix CI chỉ
chạy khi nhánh được đẩy lên, và không có gì được đẩy. Bài học là §2 bước 4: đẩy
nhánh ở **commit đầu tiên** và để PR chạy CI, đừng để "xanh trên máy" đứng thay
cho "xanh trên cả hai OS". Fix là PR #2 (`fix/ci-macos-sessionlogger`), 4 dòng.

**PR đang xung đột thì KHÔNG có check nào chạy.** Khi GitHub báo `mergeable`
= `CONFLICTING`, nó không dựng được merge ref, nên workflow `pull_request` không
khởi động và `gh pr checks` in *"no checks reported"* — trông y như CI đang
chậm, thực ra CI sẽ không bao giờ tới. PR #1 dính đúng cảnh này (xung đột ở bảng
roadmap). Cách gỡ, **không** đụng vào worktree của phiên khác:

```bash
git worktree add .claude/worktrees/<x>-merge-main -b <x>-merge-main <pr-branch>
```

rồi trong worktree mới đó: `git merge main`, xử lý xung đột, và

```bash
git push origin HEAD:<pr-branch>
```

PR tự nhận commit mới và CI chạy. (`gh pr update-branch <n>` làm được việc này
khi không có xung đột thật; có xung đột thì phải giải bằng tay như trên.)

**`--delete-branch` khi đang đứng trên chính nhánh đó.** `gh pr merge
--delete-branch` cố xoá cả nhánh local; nếu CWD đang checkout đúng nhánh ấy, nó
không xoá được, hoặc kéo theo một lần checkout ngoài ý muốn. Luôn chạy lệnh
merge **từ một worktree khác, hoặc từ checkout gốc đang đứng ở `main`**.

**Worktree lane thiếu dependency bị gitignore.** `external/asiosdk` (ASIO SDK
tải tay, không bao giờ commit) và `installer/vendor` **không** đi theo
`git worktree add` — kiểm chứng ngay trong worktree viết tài liệu này: cả hai
đều vắng. Chép từ checkout gốc trước khi build hoặc release trong worktree.
Thiếu `asiosdk` **không** làm configure fail — nó warn rồi tắt ASIO im lặng
([`memory/asio-sdk-silent-disable-2026-08-24.md`](../memory/asio-sdk-silent-disable-2026-08-24.md)).
Còn `external/JUCE` thì cần junction, xem `parallel-lanes.md` §2.

**CI xanh không chứng minh đường ASIO.** Runner Windows của GitHub không có ASIO
SDK, nên `JUCE_ASIO` tắt ở đó — đúng cơ chế im lặng vừa nói. CI chứng minh code
build sạch trên hai OS và suite `ctest` xanh; nó **không** chứng minh gì về
đường ASIO, về thiết bị thật, hay về âm thanh trong phòng. Cái đó vẫn nằm ở alpha
trên rig. Hai chiều đều thủng: local che lỗi clang, CI che đường ASIO. Cần cả
hai, và cả hai cộng lại vẫn chưa phải một cái tai trong phòng.

**`git stash` dùng chung stack với mọi worktree.** Không bao giờ `git stash` /
`git stash pop` trần. Đặt việc sang một bên bằng một commit WIP; nếu buộc phải
stash thì `git stash push -u -m "<tag>"`, ghi lại SHA, rồi `apply <sha>` chứ
không `pop`.

**Ảnh chụp git là ảnh tức thời.** `rev-list --left-right` nói "đã merged" lúc
10:00 có thể sai lúc 10:20 nếu phiên khác đang commit. Đo lại ngay trước khi
hành động, đừng dựa vào phép đo cũ.

---

## 7. Checklist — một dòng mỗi bước

Copy thẳng vào handoff hoặc báo cáo lane:

```
[ ] git fetch origin
[ ] git worktree add .claude/worktrees/<lane>-<mmdd> -b <type>/<lane> origin/main
[ ] junction JUCE + git config --worktree     (parallel-lanes.md muc 2-3)
[ ] chep external/asiosdk + installer/vendor tu checkout goc (neu can build)
[ ] commit nho, git add <path> tuong minh, khong bao gio git add .
[ ] git push -u origin <type>/<lane>          <-- ngay commit dau, de CI chay
[ ] gh pr create --base main                  (dien template PR)
[ ] build + ctest local, dan output
[ ] anh HandsFreeSnapshot neu doi GUI, doc lai anh
[ ] docs/GIOI-THIEU.md + docs/KY-THUAT-CHONG-HU.md neu doi hanh vi user-visible
[ ] memory/ note + index trong memory/MEMORY.md neu hoc duoc gi
[ ] gh pr checks <n> --watch                  --> dan output xanh
[ ]   "no checks reported" = PR dang CONFLICTING --> merge main qua worktree MOI
[ ]   main dang do? merge PR sua CI truoc, roi gh pr update-branch <n>
[ ] verifier doc lap doc file that, khong doc bao cao
[ ] owner noi "merge" --> gh pr merge <n> --merge --delete-branch (chay tu worktree khac)
[ ] checkout goc: git checkout main && git pull --ff-only
[ ] release: pwsh -File installer\release-alpha.ps1 TREN main, roi PR chore(release): 1.x.y
[ ] don worktree: git worktree list + doi chieu phien song + rmdir junction TRUOC
```
