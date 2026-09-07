# Bài học từ lane G (gain-aware notch: thang độ sâu, nhả dần, nhớ phòng) — 2026-09-07

**Bối cảnh:** 10 task SDD + fix rounds trên nhánh
`claude_desk/lane-g-brainstorm-sdd-f3c568`, commit `01ecb4c..b8e3f25`, 1.2.0
alpha — đã hiện thực và qua gate ctest, chưa đóng gói. Suite 454 → **539**.
**Đụng audio
path** (`Biquad`, `NotchChain`, `NotchController`). Spec
`docs/superpowers/specs/2026-09-06-gain-aware-notch-design.md` (v2 + Q13 + Q14),
sổ quyết định `docs/superpowers/decisions/2026-09-06-lane-g-gain-aware-notch.md`
(Q1–Q14), plan `docs/superpowers/plans/2026-09-07-gain-aware-notch.md` (rev 3).

---

## A. Bài học kỹ thuật

### 1. `Biquad::setNotchFilter` gọi `reset()` — đổi độ sâu bằng nó là một cú "cạch" vào PA

Reset đúng khi tần số hoặc Q đổi (design mới không sở hữu state cũ), **sai** khi
chỉ độ sâu đổi: xóa `z1/z2` giữa dòng tín hiệu là bước nhảy bậc. `rampNotchDepth`
giữ state và nội suy 5 hệ số trong 10 ms.

**Và:** một phép đo hộp đen (RMS, suy giảm tại f0, thậm chí "không có bước nhảy
biên độ") **không bắt được một `reset()` lén** — nó vẫn cho đúng đáp số sau khi
ramp xong. Test phải khẳng định **danh tính state**: đọc `z1/z2` trước và sau
lệnh và đòi chúng không đổi (M-7). Cùng lý do, "ramp có thật không" phải đo bằng
hệ số **đang chạy giữa ramp**, không phải kết quả cuối.

### 2. Chứng minh nội suy hệ số không bao giờ khuếch đại

Cố định `freq/Q/sr`, mọi tổ hợp lồi của các peaking RBJ đã chuẩn hóa **vẫn là**
một peaking RBJ, với `A_n ≤ 1 ≤ 1/A_d` (vì mọi `A_i ≤ 1` khi depth ≤ 0), nên
`|H| ≤ 1` ở **mọi** tần số — không riêng tần số đang nhắm — và bán kính cực
`sqrt((1 − α/A_d)/(1 + α/A_d)) < 1` nên không phân kỳ. Ramp khởi lại giữa chừng
là trường hợp 3 điểm của cùng lập luận; khởi từ biquad identity chưa cấu hình
(A = 1) cũng nằm trong tam giác lồi đó. Đo: 1,9e-15 dB tối đa; identity −3,5e-11 dB.
**Ba dòng đại số rẻ hơn một vòng build.**

### 3. `rNorm` bão hòa ở 1.5 nên phải phơi `riseRatio` ra

Trục rise của scorer chuẩn hóa `(rise − 1)/0.5` rồi clamp, nên từ điểm số không
đọc ngược ra được "đỉnh lên dốc bao nhiêu". Phải thêm `ScoreBreakdown::riseRatio`
(thô, **không chặn trên** — dùng để *so sánh*, không bao giờ để *scale*) thì
tiêu chí đặt −12 mới viết được.

### 4. Fixture "lên dốc chậm" phải khởi động ĐÚNG NGƯỠNG NHIỄU NỀN

Đây là chỗ tốn nhất của cả lane. Muốn một fixture rơi vào băng `riseRatio < 2`
thì **độ dốc không phải tham số quyết định — biên độ khởi đầu mới là.** Trong
~11 hop đầu, frame tham chiếu của trục rise vẫn là một frame **NHIỄU**, nên một
tone bật lên trên nền nhiễu cho `riseRatio ≈ 64` ở bất kỳ độ dốc nào; trong khi
peakiness (bất biến theo thang) confirm chỉ sau ~4 block, tức **bên trong** cửa
sổ đó. Sửa: cho ramp bắt đầu đúng bằng magnitude mỗi bin của nhiễu nền → rise
luôn là tone-vs-tone.

Giá trị đo được cuối cùng (1,7507) khớp `r^12` chứ không phải `r^11`: pipeline
lệch **một hop** giữa frame được chấm và mốc thời gian tính ra nó. Nếu một con
số "gần đúng nhưng lệch một bậc lũy thừa", hãy nghi độ trễ một hop trước khi
nghi công thức.

### 5. `modelMutex_` KHÔNG đệ quy — nên retune phải là anh em `*Locked`

Vòng reinforce và bước 3 của `runOnce` đều **đang cầm** `modelMutex_`. Gọi
`setNotch`/`setNotchImpl` từ trong đó vừa deadlock vừa ghi đè `lockedAtMs` (nhãn
tuổi lane D ghi vào mọi `notch_clear`). Đường duy nhất là `pushRetuneLocked`,
anh em của `pushClearLocked`.

### 6. `pushClearLocked` chỉ hạ cờ `active` — "depth < 0" là phép thử SỐNG/CHẾT SAI

Mọi trường khác (`depthDB`, `deepestDb`, …) giữ nguyên sau khi clear. Sáu test
trải ba task đã được viết dựa trên "depth < 0 nghĩa là notch còn sống" và **cả
sáu đều sẽ xanh trên slot đã bị clear**. Accessor `activeForTest` tồn tại chỉ vì
lý do này. Hệ quả song song: tái dùng slot phải **khởi tạo lại đủ 5 trường**
thang ở `setNotchImpl`, không dựa vào giá trị còn sót.

### 7. `ev` — không phải `kind` — là khóa dispatch của log (B-3)

`MainComponent::notchEventToVar` từng dùng ternary hai nhánh, nên mọi `Retune`
được ghi thành `notch_clear`. `tools/logstats.py` **pop** bản ghi notch ở dòng
clear đầu tiên → mỗi notch được đào sâu biến thành "notch sống 300 ms", tức false
positive giả trên mọi dòng thống kê, cộng với mất luôn clear thật và verdict.
Lỗi này sống qua 5 task vì không có caller production nào cho tới Task 6. **Log
từ build nhánh trước `af5201e` là rác đối với logstats** (không bản nào phát
hành như vậy).

### 8. "Đào sâu" và "còn hú" là CÙNG một phép thử (M-9)

Nên thang tự dừng ở bậc đầu tiên làm bin hết vượt ngưỡng — đó là toàn bộ điểm
của lane. **Và không test nào trong suite chứng minh được nó:** harness ghi tone
THÔ vào `h.tap` (`tests/test_notchcontroller.cpp:350-355`), analyser không bao
giờ thấy phổ đã bị notch, nên trong test thang **luôn** leo tới trần. Mọi test
đào sâu khẳng định "leo tới trần" — đúng cho fixture đó, và không nói gì về bậc
mà hệ thống thật dừng lại. Đã ghi vào tester notes; **đừng dựng test giả** (một
source tự trừ mô hình notch khỏi chính nó sẽ test cái mô hình, không test vòng
lặp, và sẽ xanh trong khi chuỗi thật sai).

### 9. Test thang phải khẳng định THEO TỪNG FRAME

Bão hòa che độ trễ: nếu chỉ kiểm tra sau vòng pump, một thang đi chậm gấp đôi
vẫn cho cùng đáp số cuối. `LinkedLanesStayOnTheSameRung` ban đầu bão hòa cả hai
làn ở −24 trước khi so — không thể đỏ theo đúng tên của nó.

### 10. Đóng băng nhả là **theo slot**, không theo notch

Một bin ở RISING giữ **mọi** notch của slot đó ở nguyên bậc, vô hạn định (Q9
không có trần thời gian). Câu mô tả mức level phải nói "mọi notch của slot",
không phải "notch đang hú".

### 11. Kẹp một chiều cho một giá trị có HAI đường trở nên cũ = nửa cái kẹp (M-B — bài học an toàn của lane)

Nhánh trần chỉ kẹp `deepestDb` khi nó **đồng thời** phát một `Set`, tức chỉ khi
notch đang sâu hơn trần. Một notch đã **NHẢ** lên bậc nông hơn bỏ qua nhánh đó
và giữ nguyên `deepestDb` sinh ra dưới trần cũ — rồi lần kẹp lại kế tiếp tôn
trọng con số đó: **sâu hơn 12 dB so với slider của người vận hành, trên một PA
đang chạy.** Sửa mất hai dòng và **cần cả hai**: kẹp giá trị nhớ vô điều kiện
mỗi tick, VÀ kẹp mục tiêu reclamp ngay lúc dùng. Hễ giữ một "sâu nhất từng có" /
high-water mark cạnh một giới hạn có thể di chuyển, hãy hỏi: điều gì xảy ra khi
giới hạn đổi trong lúc giá trị kia đang không được dùng?

### 12. Q13: lượng tử hóa trần về bậc làm preset `Music` nông hơn 1.1.3 4 dB

Vô hình trong spec, trong plan và trong mọi test — vì **không test nào dùng một
trần không phải bội số của 6**. Thang hiệu lực phải là "các bậc nông hơn trần,
cộng chính trần làm bậc cuối". Khi một hằng số có thể nhận giá trị ngoài lưới mà
test đang dùng, hãy test một giá trị như vậy.

### 13. Bộ nhớ phòng chỉ được ĐÀO SÂU (Q14)

`depthDb = remembered` ghi đè bước 1–2 theo **cả hai** chiều: một notch −6 chưa
bao giờ được reinforce (hoặc một Manual −3 để tự nhả) ghi một ký ức NÔNG, và ký
ức đó **chặn** lần hú kế tiếp ở bin ấy xuống dưới bậc −12 của đỉnh lên dốc.
Sửa: `min(depth, remembered)`, cộng gate không cho Soundcheck tiêu ký ức.

### 14. Một test cần một trạng thái thì phải dựng nó theo đường production

Rev 2 tới "đã nhả" bằng `retuneForTest(..., Release)` — hàm này chỉ set `depthDB`
và không set gì khác, nên nhánh đang được test **không bao giờ được vào** và test
đọc đáp số của đường đào sâu. Cái seam trông như đã dựng đúng trạng thái chỉ vì
tham số của nó tên là `Release`.

---

## B. Bài học quy trình

### 15. Thời điểm trong test thang phải suy từ BẬC, không từ số tròn

`55000.0` trông như "quá mốc nhả 50 s" và thiếu 5 s so với 60 s mà một notch −24
thật sự cần. **Viết phép tính vào comment**, không thì nó trôi.

### 16. Một plan chưa được kiểm chứng cho tới khi có người MỞ file nó trích dẫn

Ba helper Task 9 dùng (`TempDir`, `pumpOneBlockThroughSlotZero`,
`notchControllerForTest`) **không tồn tại ở đâu trong repo**. Chính plan đã gắn
cờ "chưa verify" và cờ đó nằm im cho tới khi người đọc thứ hai mở file ra.

### 17. Reviewer sai số dòng, implementer đúng — grep, đừng tin bên nào

Ba lần: `logstats_fixture` (reviewer nói 112, thực 113), `NotchDefaultsSurvive…`
(497 vs **496**), cờ `--expect-*` (128-131 vs **124-127**, nay **148-152** sau
`af5201e` thêm `--expect-retunes`). Cross-check cũng là một nguồn cần verify.

### 18. `effectiveLinked()` bật trên harness mono làm placement tràn sang làn 1

Harness mono có `width_ == 2` nên một placement ghi cả một entry làn 1 chết. Rác
làn 1 đó **đầu độc penalty harmonic** ở các test sau trong cùng file — một test
đỏ vì test trước đó, không vì code.

### 19. Hai bẫy quy trình lặp lại

- **Một implementer đã dùng `git stash`** để lấy bằng chứng RED. Stash stack là
  **dùng chung giữa các worktree** — cấm tuyệt đối; phải ghi "never git stash"
  vào mọi brief dispatch.
- **Script SDD tự ghi đè `.superpowers/sdd/.gitignore`** (`*`) mỗi lần chạy, âm
  thầm đè chính sách "`.superpowers/` phải TRACKED" của repo. Xóa file đó ngay
  trước commit cuối, mọi session SDD (đã có note riêng
  `sdd-workspace-gitignore-trap-2026-09-04.md`).

---

## C. Còn treo sau lane G

- **NaN self-heal trong `AudioEngine`** reset chuỗi filter mỗi khi output
  non-finite, hủy mọi ramp đang bay và **không** có replay hệ số theo sau → filter
  đứng ở độ sâu trung gian trong khi `NotchInfo.depthDB` đọc giá trị đích. Tự lành
  ở lệnh `Set` kế tiếp. Follow-up ngoài lane G.
- **`PresetNotchDefaults::depthDB = -12` vẫn lệch `kDefaultNotchDepthDb = -18`.**
  File do app LƯU nay mang `notchDefaults` nên không dính; preset viết tay không
  có khối đó vẫn im lặng nhận trần −12.
- **`NotchController::setSampleRate` vẫn không có caller production** (carry-over
  từ lane R). Lane G vẫn xóa bộ nhớ phòng ở đó vì đó là chỗ đúng của nó.
- **`releaseFrozen` publish nhưng chưa vẽ** (Q9): chip và đồng hồ nhả có thể lệch
  nhau, và một slot không hiển thị có thể đang đóng băng mà màn hình không nói gì.
- **`kDepthStepDb = 6` không có code nào đọc** — tài liệu dưới dạng hằng số, và
  dưới Q13 nó hơi gây hiểu nhầm (bậc cuối có thể nhỏ hơn 6 dB).
