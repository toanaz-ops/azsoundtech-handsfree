<!-- Fable strategic review 2026-10-21, model claude-fable-5-1 (read-only), dispatched by DEV MASTER on owner request. -->

# PROJECT005 — Fable review & phương án (2026-10-21)

Model: claude-fable-5-1 (read-only). Giới hạn đo: phiên này không có Bash → **không chạy được `gh pr list` / `git log`**; trạng thái PR lấy từ `D:\DEV CAVE EP3\shared\audits\p005-duplicate-pr-verdicts-20261021.md`, nhánh remote lấy từ `.git\FETCH_HEAD` + `.git\refs\remotes\origin\*`, lịch sử `origin/main` từ `.git\logs\refs\remotes\origin\main`. Số PR nào dưới đây chưa được xác nhận bằng `gh` thì ghi "(theo verdict)".

## 1. Hiện trạng đo được

**Release / version**
- Version nguồn: `project(HandsFree VERSION 1.3.0)` — `CMakeLists.txt:3`. README vẫn nói "1.1.3 (1.2.0 in alpha gate)" — `README.md:10-11` (drift, audit đã ghi).
- Bản alpha mới nhất tới tester: **1.3.0**, 2026-09-17, gate 714/714, SHA `C844186E…`, `Z:\My Drive\RELEASE\ALPHA TEST` — `docs/release-notes/1.3.0-alpha.md:3-7`, `installer/TESTER-NOTES.md:3-4`. Header TESTER-NOTES dòng 4-5 bị ghi đè lệch câu (" tới khi owner duyệt**" đứng mồ côi) — drift nhỏ.
- `origin/main` local = `f2cc11e`, fetch gần nhất ~20/10/2026 — `.git/logs/refs/remotes/origin/main:23`. Lần release cuối bump qua PR #5 — `shared/handoff/handoff-20260915-project005-github-workflow-lane-m.md:19`.
- `docs/release-notes/1.4.0-alpha.md` **không tồn tại trên main worktree** (glob `docs/release-notes/` chỉ tới 1.3.0); nó chỉ sống trên nhánh PR (`docs/release-1.4.0-notes` trong `.git/refs/remotes/origin/`), và **mọi PR fix/* đều stack lên hunk `@@ -34,6` của file đó** — verdict:14.

**PR đang mở (theo verdict, 21/10)**: #11–#24, trong đó **5 cặp trùng** giữa lane `fix/*` (cloud) và lane `claude/*` (audit): #14/#21 Bypass sum, #17/#18 scorer threshold, #23/#22 NaN heal, #16/#20 restart rings, #19/#15 docs — verdict:5-11. Thứ tự merge đề xuất: #11 → #15 → #12 → #13 → #20 → #21 → #22 → #24 → #16 → #17 (chờ owner) → #19 rút gọn; đóng #14, #18, #23 — verdict:13.
- Nhánh `claude/*` thấy trong refs local: `scorer-live-threshold`, `notch-commands-survive-restart`, `bypass-sums-shared-outputs`, `nan-heal-replays-design`, `lows-engine-detector` (push ~20/10 17h, `.git/logs/refs/remotes/origin/claude/scorer-live-threshold:1`). Nhánh `fix/*` của cloud lane **không có trong refs local** (chỉ `fix/c4456-shadow`, `fix/ci-macos-sessionlogger`) → số PR #14/#16/#17/#19/#23 chưa tự xác nhận.
- Ngoài kế hoạch: `feat/lane-a-afc`, `feat/lane-l-copilot`, `feat/lane-u-dumb-geek-mode`, `docs/review-research-2026-10-20`, `ci/cheapen-actions` **đã tồn tại trên origin** (`.git/FETCH_HEAD:4,7-11`) nhưng **không có spec/plan nào trong `docs/superpowers/` trên main** (glob specs: file mới nhất 2026-09-15). Lane A/L/U đang chạy ở đâu, ai điều phối — `shared/handoff/handoff-20261020-dev-master-coordination.md:26` ghi "HANDSFREE DEV (cloud) — running, **not coordinated here**".

**Chứng minh trên phần cứng vs chỉ trong test**
- Trong test/CI: suite 714 (gate 1.3.0), CI matrix windows+macos chạy `ctest` — `.github/workflows/build.yml:17-59`. Audit xác nhận audio callback sạch RT (không heap/lock/IO) — `shared/audits/p005-fable-audit-20261021.md:7`.
- Trên PA thật: **không có bằng chứng nào được ghi lại**. 1.2.0 (lane G, độ sâu đổi theo thời gian) và 1.3.0 (lane M, app tự phát sweep ra PA) đều "chưa ai nghe trên rig" — `docs/superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md:73-74`; 1.1.1 cũng vậy — `docs/release-notes/1.1.1-alpha.md:27`. Grep `tester báo|phản hồi tester|kết quả nghe` trên `docs/ memory/ installer/` → **0 kết quả**. Câu "Confirmed in the real rig" ở `src/dsp/PeakinessAnalyzer.h:60` nghĩa là chạy qua Detector+JUCE FFT thật trong `tests/test_peakiness.cpp`, không phải loa/mic. "RING RISK đã có dữ liệu thật" (1.1.3) nghĩa là chip đọc số detector sống, level 0 dB — `docs/release-notes/1.1.3-alpha.md:1-7`.
- Hạ tầng gate: GitHub Actions **ngừng chạy từ 16/09 vì billing** ("recent account payments have failed") — `handoff-20260915…:48`; chưa thấy ghi nhận đã giải quyết (nhánh `ci/cheapen-actions` gợi ý đang xử lý). Branch protection không bật được (GitHub Free) — `memory/github-oriented-workflow-2026-09-15.md:75-82`.

**Audit 21/10** (`shared/audits/p005-fable-audit-20261021.md`): 3 HIGH (scorer gate theo hằng compile-time → nút DETECTION < 10 chết, preset AGGRESSIVE vô hiệu — :11-17; lệnh notch bị `queue.clear()` khi device mở muộn → GUI Active, chain rỗng — :19-25; BYPASS copy thay vì cộng → mất một làn trên output dùng chung — :27-32), 3 MED (:34-50), ~10 LOW.

## 2. Mục tiêu dự án như repo tự nói — và độ lệch

- **Mục tiêu**: app insert ASIO tự động chống hú cho live sound, đặt notch siêu hẹp trong ~1 s, 8 slot, freeware alpha (D-07) — `docs/GIOI-THIEU.md:3-34`, `README.md:102-104`. Roadmap v2: 7 lane S→D→G→M→C/L→A, +U; owner "làm tất cả" 05/09 — roadmap:3,19-28. Phần dùng chung phải tách thư viện cho sản phẩm thứ hai (voice isolator) — `CLAUDE.md:62-82`. Mỗi lane = một release alpha, "lane đụng audio path nêu mức level dự kiến" — roadmap:52-66.
- **Lệch 1 — vòng dữ liệu không khép**: 4 lane (S, D, G, M) đã merge và 6 bản alpha đã rời máy, nhưng chưa một bản nào được nghe trên rig (mục 1). Lane C chờ "≥ 300 verdict từ ≥ 3 session" (roadmap:75), L chờ D — cả hai chờ dữ liệu **không ai đang sinh**. 15 mục Q của lane M "chốt tạm chờ nghe rig" (`docs/superpowers/decisions/2026-09-15-lane-m-active-soundcheck.md:573-580`) — 5 tuần chưa lật lại mục nào.
- **Lệch 2 — roadmap vs nhánh**: roadmap nói A "chờ M merge", L "chờ D", U "chưa brainstorm" (roadmap:76-78), nhưng cả ba nhánh đã có trên origin (mục 1) mà không có spec trên main. Nguyên tắc "mỗi lane một spec + plan + release" (roadmap:14) đang bị bỏ qua ở ít nhất 3 lane.
- **Lệch 3 — luật thư viện chưa có hành động**: `CMakeLists.txt` chỉ có một target app `juce_add_gui_app(HandsFree)` (:49), không `add_library` nào; đúng với luật "chỉ tách khi có sản phẩm thứ hai thật" (CLAUDE.md:76-79), nhưng cần nói rõ để không ai tách "phòng xa" giữa lúc đang fix.
- **Lệch 4 — docs**: README version, TESTER-NOTES header, `KY-THUAT-CHONG-HU.md:202` hứa ngưỡng tunable [5,20] trong khi scorer khoá ở 10 (audit HIGH 1).

## 3. Rủi ro lớn nhất

1. **An toàn khi diễn live — BYPASS là nút cứu show mà đang sai trên output dùng chung.** `FloatVectorOperations::copy` thay vì `add` (audit :27-32): bấm BYPASS giữa show trên rig 2 slot → 1 output thì một mic biến mất khỏi PA. Cộng HIGH 2 (preset/notch áp dụng khi ASIO chưa sẵn sàng bị xoá im lặng, GUI vẫn báo Active — :19-25): người vận hành tin là phòng đã được bảo vệ. Và app **không có limiter**, chỉ clipper ±1.0 — `docs/release-notes/1.3.0-alpha.md:87-90` — trong khi 1.3.0 là bản đầu tiên tự phát tín hiệu ra PA (:9-12). Tất cả đang nằm trong bản 1.3.0 ở drop folder.
2. **Hai lane làm trùng, không ai điều phối.** 5 cặp PR trùng, cùng đụng một hunk release-note và `KY-THUAT-CHONG-HU.md` (verdict:5-14); #18 xung đột #24 tại `NotchController.cpp:1497`. Nếu cả hai lane tiếp tục tự merge theo "God mode" (`handoff-20261020…:37`), rủi ro là hai bản vá cùng một lỗi đi vào main với hai ngữ nghĩa sensitivity khác nhau (#17 vs #18) — lỗi DSP dạng này không hiện trong build xanh (`CLAUDE.md:15-16`). Chi tiết: verdict file.
3. **Vòng phản hồi phần cứng đứt.** 6 alpha, 0 phản hồi ghi lại (mục 1). Mọi quyết định độ nhạy (pNorm), mức sweep −20 dBFS (Q2), im kênh 72 s (Q15) đang là "mặc định hợp lý, chờ lần đầu chạy thật" — decisions:271-272. Càng merge thêm lane trước khi nghe, càng khó quy lỗi về lane nào.
4. **Gate CI mong manh.** Actions dừng vì billing (handoff 0915:48), không branch protection, MSVC xanh ≠ clang xanh (memory github-oriented:30-47). Nếu 14 PR được merge bằng `ctest` local thay CI thì cổng duy nhất là người dán output.
5. **Nút độ nhạy đã ship là nút chết + quyết định pNorm hai chiều.** Preset AGGRESSIVE (`TuningPanel.cpp:63`, thr 8) không đặt thêm notch nào (audit :11-17); sửa theo #17 làm SAFE (thr 12) **kém nhạy hơn hôm nay** (cần p>88 thay vì 73), AGGRESSIVE nhạy hơn (p>58) — verdict:8. Đây là thay đổi hành vi hai chiều trên preset đã ship, không phải "fix".

## 4. Phương án

**A — Ổn định trước (stabilise-then-listen).** Đóng 3 PR trùng, merge theo thứ tự verdict (#11→…→#16), giữ #17 chờ owner, thêm 4 test còn thiếu (audit :52-57), sửa README/TESTER-NOTES, ra **1.4.0 alpha chỉ mang fix** kèm note "expected level change" cho Bypass + NaN-heal (CLAUDE.md:17, :162-165). Đóng băng A/L/U tới khi có một buổi nghe. Được: bản tới tester không còn 3 HIGH; mọi fix có test; nguồn lỗi tách bạch. Mất: 3 lane đang chạy phải chờ ~2 tuần. Công sức **S–M** (agent). Không đảo ngược: không (toàn bộ là PR, revert được). Phụ thuộc: owner chốt pNorm; owner/tester 1 buổi soundcheck.

**B — Nghe trước, sửa sau.** Dừng mọi merge, owner chạy 1.3.0 trên rig theo 10 bước TESTER-NOTES + 6 câu hỏi `1.3.0-alpha.md:152-166`, rồi mới quyết thứ tự fix. Được: dữ liệu thật trước khi đổi thêm DSP. Mất: tester đang nghe một bản có BYPASS sai trên output dùng chung và lệnh notch bị xoá — kết quả nghe sẽ nhiễm hai lỗi đó; 14 PR tiếp tục mốc, rebase ngày càng đắt. Công sức **S** agent / **M** owner. Phụ thuộc hoàn toàn vào lịch owner + phần cứng.

**C — Chạy tiếp roadmap song song (hiện trạng).** Merge fix và cho A/L/U tiếp tục; mỗi lane tự ra alpha. Được: tốc độ. Mất: lane A (AFC, "nặng nhất", roadmap:27) chồng lên nền chưa nghe; ba bản alpha liên tiếp mà không ai quy được lỗi; rủi ro 2 ở mục 3 tiếp diễn. Công sức **L**. Không đảo ngược: tester mất niềm tin vào drop folder (CLAUDE.md:113-116) — cái này không revert bằng git.

**D — Gộp về một lane (bổ trợ, không thay thế).** Chọn **một** lane sửa lỗi (khuyên: `claude/*` vì test rõ hơn — verdict:7,9), đóng lane `fix/*` cloud cho P005 sau khi cherry-pick phần GIOI-THIEU của #19. Được: hết trùng, hết xung đột hunk. Mất: ~1 ngày cherry-pick. Công sức **S**. Phụ thuộc: owner cho phép đóng lane cloud.

**KHUYẾN NGHỊ: A + D.** Lý do: (1) đây là live-sound DSP, bản ở drop folder đang có lỗi ở chính nút khẩn cấp — phải thay bản đó trước khi ai nghe; (2) mọi fix trong A đều S, agent-safe, có test (audit :121); (3) B mất giá trị vì đo trên bản lỗi; C lặp lại đúng sai lầm 107-commit-không-ai-thấy của tháng 9 ở tầng sản phẩm; (4) D gần như miễn phí và xoá rủi ro 2. Lane A/L/U **không bị huỷ**, chỉ chờ một buổi nghe — là điều roadmap vốn đã yêu cầu (roadmap:24, 77).

## 5. Kế hoạch 2 tuần

**Agent làm ngay (không chờ owner):**
- [ ] Xác nhận bằng `gh pr list` + `gh pr view` số PR/nhánh của lane fix/* (mục 1 còn "theo verdict").
- [ ] Merge theo verdict: #11 → #15 → #12 → #13 → #20 → #21 → #22 → #24 → #16 (rebase sau mỗi lần vì cùng hunk); đóng #14, #23; rút gọn #19 (bỏ số version khỏi README).
- [ ] Thêm 4 test audit :52-57 (threshold < 10 đặt được; Bypass + shared output; lệnh sống qua `audioDeviceAboutToStart`; NaN-heal không kẹt ramp).
- [ ] Fix LOW agent-safe: `arm()` tính reference một lần (:59-62), alloc trên detector thread (:64-67), detector không chạy khi engine chưa start (:69-72), `stopThread` ≥ 5 s (:74-78), ramp-out kẹt (:80-83), `stop()` sau `audioDeviceError` (:85-88), README/TESTER-NOTES/stale line refs (:105-113).
- [ ] Viết `docs/release-notes/1.4.0-alpha.md` trên main với bảng "expected level change" cho Bypass (shared route lấy lại làn bị đè) và NaN-heal (heal về đúng depth đích).
- [ ] Ghi vào roadmap trạng thái thật của A/L/U (nhánh tồn tại, chưa spec) để roadmap không nói sai.

**Chờ owner / soundcheck:**
- [ ] Chốt pNorm (#17) → merge → `release-alpha.ps1 -Part minor` trên main (GIT-WORKFLOW:179-190) → 1.4.0 alpha.
- [ ] Một buổi nghe 1.4.0 ở âm lượng thấp, tối thiểu: (a) BYPASS trên rig 2 slot → 1 output, (b) mở app trước khi cắm ASIO rồi LOAD preset → bảng notch phải khớp chain, (c) preset AGGRESSIVE đặt notch sớm hơn BALANCED, (d) nút `ĐO` một kênh + bấm `DỪNG` giữa sweep (6 câu `1.3.0-alpha.md:152-166`), gửi log `%APPDATA%\AZSoundtech\HandsFree\logs\`.
- [ ] Sau khi có log: lật lại Q nào cần trong sổ lane M; mở lại A/L/U theo thứ tự roadmap (U trước — không đụng audio; A cuối).
- [ ] Hai MED cần owner (tap trên output dùng chung :40-44; `changeSlotConfig` reset mọi slot :46-50) và LOW hardware (mở device hai lần :90-93) — làm sau buổi nghe.

## 6. Câu hỏi owner cần chốt (kèm khuyến nghị)

1. **pNorm anchor — #17 (neo theo ngưỡng sống) hay #18 (giữ neo 10)?** Khuyên **#17**: #18 không đóng HIGH 1 (candidate trong [thr,10] vẫn điểm 0 — verdict:8); #17 đúng với lời hứa `KY-THUAT:202` và `TuningPanel`; BALANCED (mặc định) không đổi; AGGRESSIVE cắt nhiều hơn (hướng an toàn), SAFE cắt ít hơn (người chọn SAFE đã chấp nhận). Phải có dòng level change trong note 1.4.0 và test `setPeakinessThreshold(6)`.
2. **Gộp về một lane sửa lỗi, đóng lane fix/* cloud cho P005?** Khuyên **có** (phương án D).
3. **Đóng băng A/L/U tới sau buổi nghe 1.4.0?** Khuyên **có**; nhánh giữ nguyên, không xoá.
4. **CI billing: nạp tiền / bỏ macOS khỏi matrix PR / public repo?** Khuyên **bỏ macOS khỏi `pull_request`, giữ ở `push: main`** (macOS ×10 phút, app chỉ Windows; clang vẫn bắt được trên main) — tuỳ chọn (b) của handoff 0915:48. Nếu hết quota → fallback `ctest` local ghi rõ trong PR (global rule 16).
5. **Tap per-lane trên output dùng chung: scratch riêng (M) hay tài liệu hoá "tap = output"?** Khuyên **scratch per-lane**, nhưng **sau** buổi nghe — đổi ngữ nghĩa detector không nên đi cùng bản fix.
6. **`changeSlotConfig` có được reset ring-risk + room memory của cả 8 slot (A-R4)?** Khuyên **không** — chỉ slot vừa sửa (audit :46-50).
7. **Ai nghe 1.4.0, khi nào, tối thiểu gửi về gì?** Khuyên: owner hoặc một tester có tên, trong 2 tuần, gửi log session + trả lời 4 mục (b) ở §5. Không có buổi này thì mọi Q lane M và lane A tiếp tục treo.
8. **Dọn 4 nhánh + 2 worktree cũ** (`docs/GIT-WORKFLOW.md:240-250`)? Khuyên: owner nêu tên từng cái; agent không tự xoá (memory worktree-prune-killed-live-session).

Nguồn chính: `D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\CLAUDE.md`, `README.md`, `CMakeLists.txt`, `docs/superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md`, `docs/superpowers/decisions/2026-09-15-lane-m-active-soundcheck.md`, `docs/release-notes/1.3.0-alpha.md`, `docs/GIT-WORKFLOW.md`, `installer/TESTER-NOTES.md`, `memory/github-oriented-workflow-2026-09-15.md`, `.github/workflows/build.yml`, `src/dsp/CandidateScorer.cpp:52-58`, `src/dsp/PeakinessAnalyzer.h:60-65`, `.git/FETCH_HEAD`; `D:\DEV CAVE EP3\shared\audits\p005-fable-audit-20261021.md`, `D:\DEV CAVE EP3\shared\audits\p005-duplicate-pr-verdicts-20261021.md`, `D:\DEV CAVE EP3\shared\handoff\handoff-20261020-dev-master-coordination.md`, `D:\DEV CAVE EP3\shared\handoff\handoff-20260915-project005-github-workflow-lane-m.md`.
