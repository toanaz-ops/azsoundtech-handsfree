# Bài học từ lane M (soundcheck đo chủ động: sweep ra PA, loop gain, notch phòng ngừa) — 2026-09-16

**Bối cảnh:** 11 task SDD + fix rounds trên nhánh
`feat/lane-m-active-soundcheck`, commit `a6be099..17f5225` (34 commit), 1.3.0
alpha — **đã hiện thực, CHƯA merge, CHƯA đóng gói**, chờ owner duyệt 9 mục cũ +
6 mục điều phối tự chốt và một lần nghe trên rig. Suite 547 → **712**.
**Đụng audio path nặng nhất từ trước tới nay**: lần đầu app tự sinh tín hiệu và
phát ra PA. Spec
`docs/superpowers/specs/2026-09-15-active-soundcheck-design.md` (rev 5, ba vòng
phản biện read-only), sổ quyết định
`docs/superpowers/decisions/2026-09-15-lane-m-active-soundcheck.md` (Q1–Q23),
ledger SDD `.superpowers/sdd/2026-09-15-active-soundcheck/progress.md`.

---

## A. Bài học kỹ thuật

### 1. Bẫy đơn vị, lần thứ hai trong cùng dự án

Spec rev 2 đặt cổng "phòng đang ngân" bằng cách so `peakinessAt` với
`CandidateScorer::kConfirmScore = 0.7f`. `peakinessAt` là **tỉ số không chặn
trên** (nhiễu đo được ~7,35; tone 1 kHz ~131,70); `kConfirmScore` là ngưỡng của
một **tích 0..1**. Cổng đó **hủy mọi lần chạy, ở mọi phòng, ngay tại kênh đầu
tiên** — và không một test nào đỏ nếu không có test riêng cho nó.

`memory/ring-risk-lane-r-2026-09-06.md` đã ghi **đúng lỗi này ở chiều ngược
lại** ba tuần trước (spec lane R viết "peakiness 10" trong khi `score` là tích
0..1). Hai lane liên tiếp, cùng cặp đại lượng, cùng kiểu sai.

**Luật rút ra: nêu ĐƠN VỊ của CẢ HAI VẾ của mọi phép so sánh, trong một
comment, ngay tại chỗ so sánh.** Đây là bài học đắt nhất của lane này và nó
không phải bài học mới — nó là bài học cũ chưa được viết thành luật.

### 2. Một cờ trở về sentinel giữa lần chạy không làm được khoá theo lần chạy

`scOutChannel_` về −1 ở **mỗi** `Gap`. Khoá việc treo ghi tap theo nó thì tap bật
lại **300 ms một lần**, tức ≈ 0,72 s mỗi kênh, **≈ 11,5 s cho 16 kênh** — vượt
`kReleaseStepMs` (10 s), đủ để thang nhả của lane G bước một bậc giữa lúc đang
đo. Rev 2 viết "~420 ms một lần", sai ~20×.

Cách sửa là **một cờ thứ hai có đúng phạm vi** (`scSuspendTaps_`, giữ từ `Arm`
tới hết đuôi của kênh cuối), không phải một cách đọc khéo hơn cờ cũ.

### 3. Một tính chất an toàn không được suy ra từ dấu của một chỉ số

`scCaptureActive_` tồn tại tách khỏi `scOutChannel_` đúng vì lý do đó:
"`scOutChannel_ >= 0`" là một **chỉ số kênh**, không phải một **giấy phép thu**.
Pha `NoiseFloor` là phản ví dụ sống: thu đang bật mà biên độ phát bằng 0.

### 4. Publish nằm ở đâu quyết định người đọc thấy gì

`copySnapshot()` được làm mới **bên trong vòng drain** của `NotchController`, nên
treo tap là **đông cứng snapshot**. Hệ quả dây chuyền: overlay lúc đang đo phải
vẽ dữ liệu của **chính lane M**, và mọi lần đọc `index` chỉ được xảy ra sau khi
tap đã chạy lại.

Cùng gốc, ở phía test: `copySnapshot()` **không bao giờ** tự làm mới trong
headless, nên mọi test đọc snapshot phải **bơm 512 float vào tap rồi gọi
`runOnce()`** trước đã. Đây là lần thứ hai bài học đó phải viết ra
(`memory/preset-save-roundtrip-2026-09-05.md`).

### 5. "Đọc lại snapshot dùng chung trước mỗi lần ghi" KHÔNG phải một bộ cấp phát

`copySnapshot()` republish trên detector thread theo nhịp hop (~10,7 ms); vòng
`ÁP DỤNG` chạy hết trong vài micro giây trên message thread. Sáu lần đọc lại trả
**cùng một frame**, `firstFreeIndex` trả **cùng một index 15** sáu lần, và
`setNotchImpl` ghi đè không kiểm `n.active` ⇒ **mất 5 trong 6 đề xuất, im
lặng**.

Thứ cấp phát thật phải **cục bộ trong lời gọi** (một `takenThisCall` khởi từ một
snapshot duy nhất ở đầu hàm). Đọc lại vẫn đáng làm — như một cái chắn với người
ghi KHÁC — nhưng **gọi nó là bộ cấp phát đã giấu con bug sau một câu nghe rất
hợp lý**.

### 6. Một đường thu bị chặn bởi bảng định tuyến thì không thu được gì

Mic đo, theo định nghĩa, **không phải một lane**. Điểm thu đặt bên trong vòng
lặp per-lane chỉ thu được khi có một lane đang bật trỏ đúng kênh đó — tức là
gần như không bao giờ. Con trỏ thu phải được **hoist ra ngoài** vòng lặp, cạnh
chỗ kiểm biên.

**Hỏi ở mọi dòng "đọc kênh này": _bảng của ai_ quyết định kênh đó có nhìn thấy
được hay không?**

### 7. Dấu của một đại lượng dẫn xuất đáng một test riêng

Rev 1 viết `needed = -(H_dB + 6)`, ra độ sâu **dương** — thứ mà `setNotchImpl`
từ chối. Cả tính năng sẽ **không đặt được một notch nào, im lặng**.
Cùng họ: `marginDb == -hDb`, nên "sắp xếp nóng nhất trước" nghĩa là margin
**tăng dần**; test của plan khẳng định ngược và sẽ đỏ trên code đúng.

### 8. `std::max(rung, NaN)` trả về `rung`

Trần "chưa đặt" được đánh dấu bằng `NaN` (`SoundcheckCandidates::Input::ceilingDb`
mặc định `quiet_NaN()`), vì 0.0 là **hướng an toàn** nhưng không phân biệt được
với "phòng sạch". Nhưng `depthFor()` áp trần bằng `std::max(rung, ceilingDb)`, và
`std::max` với một `NaN` **trả về toán hạng kia** — sentinel đi qua lặng lẽ và
hàm trả về bậc thô. Sentinel phải được **kiểm bằng `std::isfinite` tại cửa**,
không được trông vào việc phép toán sẽ tự lan truyền nó.

### 9. Phát biểu một bộ lượng tử thành LUẬT, rồi kiểm từng ví dụ theo luật

Rev 2 viết "bậc **sâu nhất** không sâu hơn `depth_raw`" — phát biểu đó cho −6 khi
cần −8, và cho **tập rỗng** khi cần −5, mâu thuẫn với chính ví dụ và chính test
của nó. Sáu ví dụ tính lại bằng tay bắt được. Luật đúng: **bậc nông nhất sâu ít
nhất bằng `depth_raw`**.

### 10. Release/acquire phải nằm trên atomic mà người đọc THẬT SỰ đọc

Comment "đã xoá TRƯỚC khi publish" dựa trên hai store `relaxed` vào **hai atomic
khác nhau** — trên x86 nó chạy đúng và **không test nào bắt được**. Cặp
publish-release / load-acquire phải đi qua đúng cái cờ mà callback dùng làm
cổng. Không mutation-test được trên x86: ghi lý do vào comment thay vì giả vờ có
test.

### 11. Một cờ dọn dẹp cần người dọn, và người dọn phải là callback

`scRampOutAtSample_` ban đầu chỉ được xoá bởi chính callback **bên trong**
`if (scOut >= 0)`. Một abort kiểu `device_changed` / `engine_stopped` làm
controller chờ "cho tới khi về −1" **vĩnh viễn**, và cái mỏ neo cũ còn cắt cụt
lần chạy SAU. Sửa bằng `scRampOutRequested_`: message thread **xin**, callback
**chốt** mỏ neo ở block đầu tiên nó thấy cờ — nên envelope luôn bắt đầu đúng ở
1.0 với **mọi** buffer size, thay vì bắt đầu ở 0 (cắt phựt) khi buffer ≥ 1440.

### 12. Tắt tiếng bằng `continue` là đóng băng state của filter

Bỏ qua một lane trong vòng lặp giữ nguyên state DF1 của `NotchChain` tới 4,5 s;
lúc mở lại, đáp ứng tự do từ state cũ có thể lớn gấp ~5–6× mức lúc tắt — một
tiếng "cạch" tại tần số notch, trên mỗi kênh. Phải gọi `clearState()` **mỗi
block** trong lúc lane bị tắt. Và `clearState()` phải là **state-only**: bản đầu
tiên dùng `reset()`, thứ xoá luôn `rampRemaining_` và làm **mắc kẹt** mọi ramp
độ sâu của lane G đang bay.

### 13. `SnapshotBuffer::linked` là CÔNG TẮC, không phải HÀNH VI

`effectiveLinked()` là `isLinked() || width_ < 2 || taps_[1] == nullptr`. Từ bên
ngoài, tương đương là `snapshot.linked || snapshot.laneCount < 2`. Và snapshot
đó **cũ ~10 ms**, nên mọi control đổi LINK/width phải bị **khoá** suốt
`Results`, không chỉ suốt lúc đo. (Bẫy harness mono đầu độc test sau: lane G mục
18.)

### 14. Sổ (ledger) per-slot thay vì một `Origin` mới

Mode `SOUNDCHECK` 15 giây thụ động **cũng** sinh notch mang `Origin::Soundcheck`.
Một lượt `ÁP DỤNG` dọn "mọi notch origin Soundcheck" sẽ **xoá im lặng lớp bảo vệ
người vận hành đã tự khoá vào**. Thêm một enumerator `Origin` mới thì phải rà
lại mọi chỗ lane G kiểm `origin != Soundcheck`.

Chọn: một **sổ `(lane, index, hz)` mỗi slot**, do caller giữ, khớp theo
`active && origin == Soundcheck && |Δhz| ≤ 1 bin`. Đổi ít code nhất, và phạm vi
dọn trở thành **dữ liệu** chứ không phải một suy luận từ enum.

Giá phải trả, đã ghi vào header: một mục sổ bị mồ côi bởi `CLEAR ALL` hoặc một
lần đổi width sẽ được mang theo vô hạn (chặn trên bởi `kTotalSlots`).

### 15. Bộ đếm thế hệ cho mọi hộp thoại async

Hộp thoại xác nhận chạy async trên message thread. Trong lúc nó mở, device có
thể restart, routing có thể đổi, người vận hành có thể bấm sang slot khác — và
một cái `OK` cũ sẽ **arm một lần quét trên một rig khác**. Cách kiểm
"state != Idle || lock != None" **không bắt được** một lần restart lúc đang
Idle.

Cách đúng: một **bộ đếm thế hệ** tăng ở mọi sự kiện làm câu hỏi cũ mất nghĩa;
callback của hộp thoại so thế hệ của chính nó và tự bỏ qua. Cùng một mẫu dùng
được cho mọi confirm async trong app này.

### 16. Invariant mà không test nào giữ được

Invariant 16 ("không lock, không cấp phát, không log trong callback") là một
tính chất của **văn bản mã nguồn**, không phải một hành vi quan sát được từ
ngoài. Nó nằm trong checklist của reviewer, và spec **nói thẳng** rằng không gì
trong suite cưỡng chế nó. Tốt hơn hẳn việc giả vờ có một test.

---

## B. Bài học về quy trình

### 17. Ba vòng phản biện: 8 / 2 / 0 blocker — và cả 2 của vòng 2 do vòng 1 sinh ra

Vòng 1 trên spec rev 1: 8 BLOCKER / 11 IMPORTANT / 8 MINOR. Rev 2 hấp thụ cả 27.
Vòng 2 trên rev 2: 2 BLOCKER / 3 IMPORTANT / 2 MINOR — **cả bảy đều do chính bản
viết lại sinh ra**, không mục nào sót từ vòng 1. Vòng 3 trên rev 3: không
blocker.

**Hấp thụ tám blocker trong một lượt viết lại sinh lỗi mới với tốc độ đáng kể.
Một vòng phản biện thứ hai trên bản ĐÃ SỬA không phải thủ tục thừa.** Mẫu này
lặp lại y hệt ở tầng code: nhiều fix round 2 tồn tại chỉ để dọn thứ fix round 1
vừa tạo ra.

### 18. Một plan chưa được kiểm chứng cho tới khi có người MỞ file nó trích dẫn

Cross-check read-only trên plan rev 1 (`plan-crosscheck-01.md`) tìm ra **44 mục:
10 BLOCKER / 12 IMPORTANT / 22 MINOR**. Trong đó ít nhất hai mục sẽ ship thành
**lỗi production**, không phải lỗi test: đường `ÁP DỤNG` mất 5 trong 6 đề xuất
(mục 5 trên), và điểm thu bị chặn bởi bảng định tuyến (mục 6).

Bản tự review của chính plan đã **nêu tên bốn helper "chưa kiểm chứng"** — và
"đã đánh dấu là chưa kiểm chứng" **không giống** "đã kiểm chứng". Lane G học bài
này một lần (mục M-1); một lane sau nó vẫn đúng.

### 19. Ảnh render bắt lỗi mà 683 test xanh không bắt

Task 9 (GUI): render bắt **3 lỗi** mà toàn bộ suite đi qua — dải kết quả đè lên
analyser, lớp phủ "độ tin cậy thấp" mờ tới mức vô hình (~3% luminance), và đường
margin vẽ trùng màu peak-hold. Rồi ở fix round, một sửa nét đứt **xoá mất nửa
dưới của đường cong** — cũng do render bắt.

**Một build xanh không nói gì về việc thứ đó có đọc được không.** Đây là lần thứ
ba dự án ghi lại đúng kết luận này (lane S, lane G, lane M).

### 20. Ba tuyên bố không test nào chứng minh được — phải đi vào tay người nghe

(a) −20 dBFS nghe như thế nào trong một phòng thật; (b) notch phòng ngừa có thật
sự nâng gain-before-feedback không (**đo bằng cách mở master từng dB cho tới lúc
hú, trước và sau `ÁP DỤNG`** — con số duy nhất chứng minh lane này đáng làm); và
(c) 4,5 s im mỗi kênh, ~72 s cho hệ lớn, có vừa quy trình soundcheck thật không.
Cả ba nằm trong `installer/TESTER-NOTES.md`, không nằm trong suite.

Thêm một cái thứ tư, tệ hơn: **đường `ÁP DỤNG` không tới được trong headless**.
Lần `ÁP DỤNG` thật đầu tiên trong đời tính năng này sẽ xảy ra **trên một dàn
thật**. Tester notes phải nói câu đó ra.

### 21. Hai bẫy môi trường, ghi lại để khỏi mất giờ

- **Write/Edit từ chối đường dẫn worktree khác.** Viết vào scratchpad rồi `cp`,
  hoặc viết bằng Python với `encoding="utf-8"` tường minh.
- **Heredoc cắt cụt quanh ~15 KB.** Mọi file docs dài phải đi đường file, không
  đi đường heredoc.
- **`git stash` dùng chung stack với mọi worktree.** Dùng commit WIP tạm.
- **Script SDD tự ghi đè `.superpowers/sdd/.gitignore` (`*`) mỗi lần chạy**, âm
  thầm đè chính sách "`.superpowers/` phải TRACKED" của repo này. `rm -f` file
  đó ngay trước **mỗi** commit — lần thứ ba dự án ghi điều này
  (`memory/sdd-workspace-gitignore-trap-2026-09-04.md`).
- **Nhiễu clangd trong scratchpad** làm lẫn kết quả grep của chính session; giữ
  scratchpad ra ngoài mọi đường tìm kiếm.

### 22. Test enum-to-string phải LẶP các enumerator

Một `std::set` gồm tám chuỗi literal chỉ khẳng định rằng tám chuỗi literal khác
nhau. Nó không gọi hàm đang được test. Lặp các enumerator, đòi `size()` đúng, và
đòi không có `"unknown"`.

### 23. Một seam test phải nằm ở PHÍA BÊN KIA thứ nó định vô hiệu

Seam đặt trên `peak` không chứng minh được gì khi tín hiệu kẹp **cả** `peak`
**lẫn** từng mẫu: seam phải **nhân vào cái tín hiệu đã sinh ra**. Một seam không
làm invariant của nó đỏ được là đồ trang trí.

### 24. Một con số fixture cần đại số viết bên cạnh, và đại số đó phải được CHẠY

Plan rev 1 ship kỳ vọng truncation **3,0 dB** cạnh một dẫn xuất cho ra
**0,0035 dB** — lệch 750 lần. Không ai thấy vì dẫn xuất là văn xuôi còn con số
là code.

---

## C. Còn treo, chờ người quyết

- **9 mục owner phải duyệt trước khi phát ra PA thật** (spec header), cộng
  **6 mục điều phối tự chốt trong lúc triển khai** (Q18–Q23).
- **Sentinel "không có mỏ neo"**: `scRampOutAtSample_ < 0` trùng nghĩa với
  "index còn âm" trong pha nền, nên một abort trong pha nền chỉ được nghe thấy
  khi index về 0 (kênh nằm im thêm ≤ ~0,53 s; **không** có phát ra trong cửa sổ
  đó). Đổi sang `INT64_MIN` hoặc một bit hợp lệ: **hoãn** (Q22).
- **`applyModeGating` nay tắt hẳn detector của một slot đang disable** — đúng
  hơn trạng thái cũ, nhưng là **thay đổi hành vi ngoài phạm vi lane M** (Q19).
- Danh sách minor hoãn đầy đủ: ledger SDD, các dòng `minor (deferred)`.
