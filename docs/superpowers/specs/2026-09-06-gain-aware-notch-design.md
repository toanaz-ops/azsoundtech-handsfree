# Lane G — Gain-aware notch: depth theo nhu cầu, nhả dần, nối ring-risk

**Ngày:** 2026-09-06. **Roadmap:** [`2026-09-04-anti-feedback-v2-roadmap.md`](2026-09-04-anti-feedback-v2-roadmap.md) (lane G, chờ S + D + R — cả ba đã hạ cánh, main `6b8d084`, 1.1.3 alpha, suite 454/454).
**Sổ quyết định:** [`../decisions/2026-09-06-lane-g-gain-aware-notch.md`](../decisions/2026-09-06-lane-g-gain-aware-notch.md) (Q1–Q14, đừng hỏi lại).
**Trạng thái:** **đã thực thi, 1.2.0 alpha** (plan: [`../plans/2026-09-07-gain-aware-notch.md`](../plans/2026-09-07-gain-aware-notch.md), 10 task + fix rounds, suite 546/546 tại `fba2626`, chưa đóng gói — xem §8). Spec v2 sau phản biện vòng 1 + ruling Q7–Q12 + Q13, cập nhật 2026-09-07 sau phản biện vòng 2 (B-5 fixture ramp, M-B kẹp lại vượt trần) và Q14 (nhớ phòng chỉ được đào sâu). Q1–Q13 không đổi. **Đụng audio path:** có (`Biquad`, `NotchChain`). **Release:** 1.2.0 (`-Part minor`).

## 1. Vấn đề

Đọc code 2026-09-06:

- Depth là **một số cố định** đọc từ preset lúc đặt
  (`NotchController::placeConfirmed`, `notchDepthDb_`, mặc định −18, kẹp
  [−24, −6]). Không có gì đo notch *cần* bao nhiêu; phòng hú nhẹ vẫn bị cắt
  18 dB, tone mất nhiều hơn cần.
- Nhả là **cụt**: `runOnce` bước 3, `liveMs_ − lastDetectedMs > 30 s` →
  `Clear`. Tone không hồi dần; hú quay lại thì phải dò lại từ đầu.
- `Biquad::setNotchFilter` **gọi `reset()`** mỗi lần đổi hệ số
  (`Biquad.cpp:126`). Đổi depth trên notch đang chạy là xóa trạng thái filter
  giữa dòng tín hiệu: click ra loa. Không có đường retune nào giữ state.
- Lane R đã publish `ringRiskScore / ringRiskValid / ringRiskThreshold` vào
  `SnapshotBuffer`; chưa ai đọc nó ngoài GUI.
- Công thức "τ ringing → loop gain vượt X dB → depth = −(X + 3)" trong
  research §4 cần **round-trip delay** của vòng hú; app không đo (việc lane
  M). G không dùng công thức đó: depth được **đo thực nghiệm** (đặt nông,
  còn hú thì sâu thêm) — Q2.

## 2. Mục tiêu và phi mục tiêu

Mục tiêu:

1. Notch đặt ở **−6 dB** và chỉ đào sâu (bước 6 dB) chừng nào bin đó còn hú,
   không sâu hơn **trần = depth preset** (Q1, Q2).
2. Nhả **theo thang** −18 → −12 → −6 → Clear với đồng hồ 30 s cho bậc đầu và
   10 s mỗi bậc sau; hú quay lại thì **kẹp lại ngay** về bậc sâu nhất đã đứng
   (Q3).
3. "Phòng nhớ": hú quay lại cùng tần số trong 5 phút sau Clear → đặt lại từ
   bậc sâu cũ (Q3, Q6).
4. Đồng hồ nhả **đóng băng** khi chip RING RISK ở RISING/CRITICAL (Q4).
5. Mọi đổi depth trên notch đang chạy đi qua **ramp hệ số giữ state 10 ms**
   trong `Biquad` (Q5). Không click.

Phi mục tiêu (ghi để plan không lấn):

- Không ước lượng loop gain từ τ (chờ lane M có delay).
- Không đổi thuật toán dò, ngưỡng, scorer (trừ 1 field đọc thêm, §4.2).
- Không đổi GUI ngoài số depth đã hiện sẵn; không thêm widget.
- Không gộp quyết định treo lane R (I-3, `setSampleRate`, A-R7) — owner đã
  nói không, 2026-09-06.
- Không đổi hành vi notch Soundcheck (KD-7 giữ nguyên).

## 3. Mức thay đổi level dự kiến (bắt buộc theo CLAUDE.md)

So với 1.1.3 (đặt −18 cố định, Clear cụt ở 30 s):

| Thời điểm | Tại bin của notch | So với hôm nay | Ngoài bin |
|---|---|---|---|
| Đặt notch | cắt **−6** (hoặc −12 nếu rise dốc). Tới −18 sau ~600 ms (2 bước × 300 ms), ~300 ms nếu nhảy −12 | **nông hơn 12 dB** (hoặc 6) trong ≤ 0.6 s: hú được nén ít hơn | 0 dB |
| Đặt notch từ "phòng nhớ" | cắt thẳng ở `deepestDb` cũ (tới trần, tối đa −24) ngay block đầu | bằng hoặc sâu hơn hôm nay ngay lập tức | 0 dB |
| Đào sâu | mỗi bước −6 dB, trải 10 ms | — | 0 dB |
| Kẹt ở bậc nông (M-9) | notch dừng ở bậc đầu tiên làm bin hết vượt ngưỡng peakiness, có thể là −6 hoặc −12, **không tự tới trần** | nông hơn hôm nay 6–12 dB *suốt đời notch* — đây là mục tiêu "tone tốt hơn", nhưng phải biết | 0 dB |
| Nhả, 30 s → 50 s sau lần hú cuối | −12 rồi −6, mỗi bậc +6 dB, cách nhau ≥ 10 s | **sâu hơn hôm nay** (hôm nay đã Clear = 0 dB ở 30 s): tone mất thêm 12 rồi 6 dB trong 20 s | 0 dB |
| Nhả khi RING RISK ≥ RISING | đồng hồ đứng, notch giữ bậc | có thể sâu hơn hôm nay **không giới hạn thời gian** chừng nào phòng còn căng (xem Q9) | 0 dB |
| Kẹp lại | về `deepestDb` trong một bước, trải 10 ms | — | 0 dB |
| Hạ trần sống | về trần trong một bước, trải 10 ms | — | 0 dB |

Master, limiter, clamp output, NaN/denormal guard: **không đụng** (đã kiểm:
`ScopedNoDenormals` `AudioEngine.cpp:487`, `isfinite` + clamp
`AudioEngine.cpp:605-642`). Chỗ tester phải nghe ở âm lượng thấp trước:
**hàng "đặt notch"** — phòng hú bùng nhanh sẽ nghe hú lâu hơn hôm nay tới
~0.3–0.6 s trước khi bị nén hết. Hàng "nhả 30→50 s" và "kẹt ở bậc nông" là
hai chỗ tester sẽ **nghe khác** rõ nhất về tone.

## 4. Thiết kế

### 4.1 Bậc depth và trần

- Bậc: `kDepthLadderDb[] = {−6, −12, −18, −24}`. Bậc *sâu hơn* = số âm hơn.
  Notch Detector **chỉ đứng trên bậc**, không bao giờ ở giá trị lẻ.
- **Trần** = `notchDepthDb_` (preset, kẹp [−24, −6], không nhất thiết bội
  của 6) đọc **sống** mỗi tick. **Thang hiệu lực = các bậc nông hơn trần,
  cộng chính trần làm bậc cuối** (Q13): trần −10 (preset Music) ⇒ −6 → −10;
  trần −13.7 ⇒ −6 → −12 → −13.7; trần −18 ⇒ −6 → −12 → −18; trần −6 ⇒
  không bao giờ đào. Bậc cuối = trần, gọi là `ceilingRung` (có thể là giá
  trị lẻ — ngoại lệ duy nhất của "chỉ đứng trên bậc"). Bậc kế sâu hơn từ
  depth d = `min` giữa bậc thang kế và trần; bậc kế nông hơn từ trần = bậc
  thang nông hơn gần nhất.
- Hạ trần giữa chừng: notch Detector đang sâu hơn `ceilingRung` mới →
  `Set(ceilingRung)` trong tick kế (reason `Ceiling`), `deepestDb` cũng kẹp
  về đó. **Và mỗi tick, với MỌI notch Detector, `deepestDb = max(deepestDb,
  ceilingRung)` — không phụ thuộc `depthDB` hiện tại.** Chỉ kẹp khi `depthDB
  < ceilingRung` là chưa đủ: một notch đã nhả lên bậc nông hơn trần mới
  (ví dụ đang ở −6, trần vừa hạ xuống −12) không kích hoạt nhánh Set, nên
  `deepestDb` cũ (−24) sống sót và lần kẹp lại kế tiếp sẽ đưa notch xuống
  −24, sâu hơn trần 12 dB (vi phạm Q1 và invariant 2 §4.10). Nâng trần:
  không tự đào, chờ reinforce như bình thường. Notch
  Preset/Manual: **slider không chạm** (Q8) — trần riêng của chúng là
  depth được cho, lưu trong `ceilingDb` của `ModelNotch`; Detector có
  `ceilingDb = NaN` nghĩa là "theo slider".
- Ghi nhận (không phải việc G): `PresetManager` mặc định `notchDefaults.depthDB
  = −12` (`PresetManager.h:146`) còn controller mặc định −18
  (`NotchController.h:95`); preset không có `notchDefaults` âm thầm đổi trần.
  Có từ trước; nêu cho owner, không sửa trong G.

### 4.2 Trạng thái mỗi notch (`ModelNotch`, thread detector)

Thêm:

| Trường | Nghĩa |
|---|---|
| `deepestDb` | bậc sâu nhất từng đứng kể từ khi đặt (cũng là đích của kẹp lại) |
| `stageChangedAtMs` | `liveMs_` lần đổi depth gần nhất (đào, nhả, kẹp, trần) |
| `quietMs` | thời gian **tích lũy** bin yên (không reinforce) kể từ lần đổi depth/reinforce gần nhất; đóng băng được |
| `releasedSteps` | số bậc đã nhả kể từ `deepestDb` (0 = chưa nhả bậc nào); thay cho so sánh bằng double (m-3) |
| `ceilingDb` | trần riêng: Preset/Manual = depth được cho; Detector = NaN ("theo slider") (Q8) |

`lastDetectedMs` giữ nguyên cho snapshot/log; auto-release **không** đọc nó
nữa mà đọc `quietMs`.

**Khởi tạo (B-2)**: `pushClearLocked` chỉ hạ `active = false` và để nguyên
mọi trường khác (`NotchController.cpp:200`), nên slot tái dùng mang
`deepestDb` cũ. Vì vậy **`setNotchImpl`** — điểm duy nhất làm slot
`active = true`, cho MỌI Origin và mọi đường (`placeConfirmed`,
`adoptPreset`, `setNotch` từ GUI, unwind) — khởi tạo lại cả năm trường:
`deepestDb = depthDB`, `stageChangedAtMs = liveMs_`, `quietMs = 0`,
`releasedSteps = 0`, `ceilingDb = (origin == Detector ? NaN : depthDB)`.
`placeConfirmed` chỉ chọn `depthDB` khởi điểm rồi gọi `setNotchImpl` như
hôm nay.

**Kẹp −24 lúc adopt (Q12)**: `setNotchImpl` kẹp `depthDB = max(depthDB,
−24.0)` cho mọi Origin trước khi validate, và `adoptPreset` đếm số notch bị
kẹp vào log (`juce::Logger`, cùng chỗ log `skipped`). Invariant 1 nhờ đó
đúng cho mọi Origin, không chỉ Detector.

Đọc thêm từ scorer: `CandidateScorer::ScoreBreakdown` thêm `riseRatio`
(tỉ số thô `mag_now / mag_ref`, chưa chuẩn hóa; 1.0 khi chưa có history —
cùng nhánh trả `rNorm`). `rNorm` hiện bão hòa ở rise 1.5 nên không phân
biệt được "dốc". `score` **không đổi**; một đường số học, chỉ lộ thêm một
số trung gian (giữ nguyên tắc lane D).

### 4.3 Đặt (`placeConfirmed`)

1. `depth = −6`.
2. Nếu `pc.breakdown.riseRatio ≥ kSteepRiseRatio (2.0)` → `depth = −12`.
3. Nếu "phòng nhớ" (§4.6) có mục cùng làn, **cùng bin** (Q10), chưa hết
   hạn → `depth = min(depth, deepestDb_nhớ)` — phòng nhớ **chỉ được làm sâu
   hơn**, không bao giờ nông hơn bậc Q2 đã chọn (Q14: một notch −6 chưa đào
   hay Manual −3 để tự nhả không được kẹp lần đặt sau).
4. `depth = max(depth, trần)` (kẹp: không sâu hơn trần).
5. `deepestDb = depth`, `stageChangedAtMs = liveMs_`, `quietMs = 0`.
6. Phần còn lại (index, LINKED, event Set, ctx) như hôm nay.

Origin Preset/Manual (`adoptPreset`, `setNotch` từ GUI): `deepestDb =`
depth được cho, và **trần riêng** của notch đó = depth được cho — không đào
(preset đã nói rõ muốn bao nhiêu). Origin Soundcheck: không có thang, không
đào, không nhả (KD-7).

### 4.4 Đào (trong vòng reinforce của `processSpectrumForDetection`)

Vòng reinforce thật ở `NotchController.cpp:737-763`, chạy **dưới
`modelMutex_`** (dòng 738). `setNotch()`/`setNotchImpl()` tự lấy cùng mutex
(`.cpp:162`), mutex không đệ quy ⇒ **không được** gọi chúng từ đây (B-1).

Thêm hàm **`pushRetuneLocked(c, i, newDepthDb, reason)`**, anh em với
`pushClearLocked` (`.cpp:195`), gọi khi đã cầm `modelMutex_`:

- Kiểm đủ 4 predicate validate-before-send của header (`NotchController.h:24-27`):
  `sampleRate > 0`, `Q > 0`, `0 < freq < sr/2`, `depthDB ≤ 0`; thêm
  `depthDB ≥ −24`. Không đạt → không push, không đổi model.
- Đẩy `NotchCommand::Set{channel = c, index = i, frequency = n.frequency,
  Q = n.Q, depthDB = newDepthDb, slot}` — **freq/Q lấy từ `n` đã lưu**,
  không lấy `notchQ_` hiện hành (m-2: khác Q một bit là `NotchChain` rơi
  xuống đường reset = click).
- Cập nhật `n.depthDB`; **giữ nguyên `lockedAtMs`, `origin`,
  `lastDetectedMs`** (`lockedAtMs` là `ageMs` của Clear, nhãn lane D).
- Push `NotchEvent{Kind::Retune, fromDepthDb, depthDb, retuneReason, ageMs
  = liveMs_ − lockedAtMs}`.

Trong vòng reinforce, khi reinforce **trúng** một notch (đã có test
`peakinessAt(bin) > la.analyzer.getThreshold()`):

- `quietMs = 0`, `lastDetectedMs = liveMs_` (như hôm nay).
- Nếu `releasedSteps > 0` (đang nhả; mọi origin trừ Soundcheck): kẹp lại
  **ngay** — `pushRetuneLocked(c, i, max(deepestDb, ceilingRung), Reclamp)`,
  `releasedSteps = 0`, `stageChangedAtMs = liveMs_`. Không chờ 300 ms.
  **Đích kẹp lại bị trần kẹp**, không phải `deepestDb` trần trụi: trần đọc
  sống, và giữa lúc nhả với lúc hú quay lại người vận hành có thể đã kéo
  slider nông đi. `max` chọn giá trị NÔNG hơn (ít âm hơn). Với Preset/Manual
  `ceilingRung` chính là depth của chúng nên `max` là phép đồng nhất; chỉ
  notch Detector mới thấy khác biệt. Xem §4.1 (kẹp `deepestDb` mỗi tick) —
  hai chỗ này cùng bảo vệ một invariant và phải cùng có.
- Ngược lại, nếu `origin == Detector` **và** `depthDB > ceilingRung` (còn
  nông hơn) **và** `liveMs_ − stageChangedAtMs ≥ kDeepenAfterMs (300)`:
  `pushRetuneLocked(c, i, bậc kế sâu hơn, Deepen)`, `deepestDb = depth
  mới`, `stageChangedAtMs = liveMs_`.

Đã kiểm (CONFIRMED): `liveMs_` chỉ tăng ở bước 2 của `runOnce`, SAU vòng
drain, nên cổng 300 ms không nổ hai lần trong một lần drain, và LINKED
(reinforce cả hai làn từ một frame, `.cpp:740-742`) luôn giữ hai làn cùng
bậc.

Ghi nhận M-9 (không phải lỗi, là hệ quả của Q2): tiêu chí đào và tiêu chí
"còn hú" là cùng một phép thử trên spectrum **sau** notch. Với threshold
mặc định 10, hú confirm ở peakiness > 73 đọc còn 36.6 / 18.4 / 9.2 qua
−6/−12/−18 nên tới −18 sau ~600 ms; với threshold preset ≥ ~18 thang dừng ở
−12; với trần −24, hú peakiness < 79 không bao giờ tới −24. Xem Q7.

LINKED: hai làn cùng index được reinforce từ cùng frame (chính sách lane S
giữ nguyên) nên cùng bậc; test pin điều này.

### 4.5 Nhả (`runOnce` bước 3, thay khối auto-release)

Chỉ khi `tapAlive`. Với `dt` = bước live clock của tick này:

1. **Đóng băng**: đọc **`frameMaxScore_` / `frameScoreValid_`**
   (`NotchController.h:433-434`) — member chỉ-detector-thread, chứa đúng hai
   số vừa publish vào snapshot; **không** lấy `snapshotMutex_` (M-5: hôm nay
   hai mutex không bao giờ lồng nhau, G không tạo thứ tự khóa mới).
   `frozen = frameScoreValid_ && frameMaxScore_ ≥ kRiskFreezeFraction ×
   CandidateScorer::kConfirmScore` = score ≥ 0.385, đúng ranh RISING của
   `SpectrumView::riskForScore`. `frozen` ⇒ không cộng `quietMs` cho notch
   nào tick này. `frameScoreValid_ == false` (detection tắt) ⇒ **không**
   đóng băng — nhả vẫn chạy như hôm nay khi detection tắt.
   Ghi nhận M-8: chip GUI còn qua `RingRiskHysteresis` hold 750 ms và chỉ
   đọc `displayedSlot_`, nên chip có thể báo RISING khi đồng hồ đã chạy lại,
   và slot không hiển thị có thể đang đóng băng mà màn hình không nói.
   **Q9: không trần thời gian**; publish `bool releaseFrozen` per-slot vào
   `SnapshotBuffer` (cạnh `ringRiskValid`) = giá trị `frozen` của tick nhả
   gần nhất, để GUI/log dùng sau. 1.2.0 không vẽ nó.
2. Với mỗi notch active, origin ≠ Soundcheck: **trước hết là trần sống**
   (§4.1). Với notch `origin == Detector`, LUÔN `deepestDb = max(deepestDb,
   ceilingRung)` — vô điều kiện, mỗi tick, kể cả khi `depthDB` đã nông hơn
   trần và nhánh `Set(ceilingRung)` không chạy. Sau đó, nếu `depthDB <
   ceilingRung` thì `Set(ceilingRung)` (reason `Ceiling`), `stageChangedAtMs
   = liveMs_`, `quietMs = 0`, và tick này dừng ở đó cho notch đó (một lần
   đổi depth mỗi notch mỗi tick). Notch Preset/Manual không đi qua nhánh
   này (Q8). Rồi: nếu `!frozen`, `quietMs += dt`.
3. Ngưỡng nhả: `quietMs ≥ kReleaseFirstMs (30 000)` khi `releasedSteps ==
   0`, `≥ kReleaseStepMs (10 000)` khi `releasedSteps > 0`.
4. Đủ ngưỡng: nếu `depthDB < −6` → `pushRetuneLocked(c, i, bậc kế nông
   hơn, Release)` (bậc kế = bậc thang nông hơn gần nhất; giá trị lẻ của
   Preset/Manual lấy bậc nông hơn gần nhất), `releasedSteps += 1`,
   `stageChangedAtMs = liveMs_`, `quietMs = 0`. Nếu `depthDB ≥ −6` →
   `pushClearLocked(c, i, AutoRelease)` và ghi "phòng nhớ" (§4.6).

Seam test (M-6): `setRingRiskOverrideForTest(std::optional<std::pair<bool,
float>>)` ép `frameScoreValid_/frameMaxScore_` sau mỗi drain, và
`quietMsForTest(c, i)`. Không có seam thì test đóng băng không viết được:
`mNorm` là log-ratio so với EMA 3 s (`CandidateScorer.cpp:115-118`) nên tone
tĩnh tự tụt score về 0 sau vài giây, không giữ được ≥ 0.385 suốt 30 s bằng
audio.

`kAutoReleaseMs` (30 000) giữ tên/giá trị làm `kReleaseFirstMs`; doc string
đổi. `kTapSilenceTimeoutMs` không đổi.

### 4.6 Phòng nhớ (`ReleasedMemory`, thread detector)

- Mỗi làn một mảng cố định 16 mục `{frequencyHz, deepestDb, clearedAtMs}`,
  ghi vòng (đè cũ nhất). Không allocate.
- Ghi khi Clear do `AutoRelease` từ bậc −6 (không ghi Manual/ClearAll/
  WidthChange/VerdictFalse/PartialApplyUnwind).
- Đọc ở §4.3 bước 3: cùng làn, **cùng bin** (`lround(f / binWidth)` bằng
  nhau, ±0 — Q10; lệch một bin là hú mới, bắt đầu −6), `liveMs_ −
  clearedAtMs ≤ kMemoryTtlMs (300 000)`. Trúng thì **xóa mục** (đã dùng).
- LINKED (M-11): một Clear LINKED ghi mục ở **cả hai làn**; một placement
  LINKED tra và **xóa ở cả hai làn** (khớp ở làn nào cũng lấy `deepestDb`
  lớn nhất). Không để mục mồ côi ở làn kia.
- Xóa toàn bộ khi `setWidth` (ranh thật của đổi thiết bị/SR qua
  `onAfterRestart`, `NotchController.cpp:106-114`) và `clearAll`.
  `setSampleRate` không có caller production (m-7) — vẫn xóa ở đó cho nhất
  quán. Không persist ra preset/đĩa.

### 4.7 DSP: retune giữ state có ramp (`src/dsp`)

`Biquad`:

- Thêm `bool rampNotchDepth(double freq, double Q, double sampleRate,
  double depthDB, int rampSamples)`. Tính bộ hệ số đích bằng **cùng công
  thức và cùng 4 điều kiện từ chối** của `setNotchFilter` 4 tham số
  (`sampleRate > 0`, `Q > 0`, `0 < freq < sr/2`, `depthDB ≤ 0`; code là 5
  `if` vì `freq` kiểm hai đầu, `Biquad.cpp:78-101`). Bị từ chối → `false`,
  filter (hệ số, state, ramp đang chạy) **không đổi**.
- Hợp lệ → **không** `reset()`. Lưu `target[5]`, `delta[5] = (target −
  **current đang nội suy**) / rampSamples`, `rampRemaining = rampSamples`.
  Lệnh thứ hai đến giữa ramp ⇒ ramp mới khởi động từ bộ hệ số hiện tại
  (không nhảy về đích cũ). `rampSamples ≤ 0` → gán thẳng target, không reset.
- `processSample`: nếu `rampRemaining > 0`: cộng `delta` vào 5 hệ số, giảm
  đếm; khi về 0 **gán thẳng `target`** (không để sai số tích lũy). Nhánh
  này là một so sánh + 5 phép cộng, không allocate.
- `setNotchFilter` (cả hai dạng) và `reset()` giữ nguyên hành vi, và **hủy
  ramp đang chạy** (`rampRemaining = 0`) — bộ hệ số mới thắng.
- **Ổn định và không-boost trong ramp** (M-4 — tam giác `(a1, a2)` chỉ phủ
  mẫu số; cần cả tử số). Cố định `freq/Q/sr`, với mỗi đầu `i` đặt
  `u_i = α/A_i`, `d_i = 1 + u_i`. Mọi tổ hợp lồi (trọng số `w_i`) của các
  bộ hệ số chuẩn hóa vẫn có `b1 == a1` và thỏa đồng nhất thức
  `P + αK = 1` với `P = Σ w_i/d_i`, `K = Σ w_i/(A_i d_i)`. Thay vào, bộ nội
  suy **chính là** một peaking RBJ với gain tử `A_n = Σ(w_i A_i/d_i)/P` và
  gain mẫu `1/A_d = Σ(w_i/(A_i d_i))/P`:

  `|H(ω)|² = [(cosω − cosω₀)² + α²A_n² sin²ω] / [(cosω − cosω₀)² + α²A_d⁻² sin²ω]`

  Mọi `A_i ≤ 1` (depth ≤ 0) ⇒ `A_n ≤ 1 ≤ 1/A_d` ⇒ `|H| ≤ 1` tại **mọi** ω,
  cực bán kính `sqrt((1 − α/A_d)/(1 + α/A_d)) < 1`. Phủ luôn ca ramp khởi
  động lại giữa chừng (tổ hợp lồi 3 điểm). Đo bởi phản biện 2026-09-06:
  lưới 3 SR × 5 f × 3 Q × 7 cặp depth × 101 điểm, và 20 000 tổ hợp lồi ngẫu
  nhiên × 400 tần số: **max gain 0 dB** (1.9e-15). Denormal: delta hệ số
  ~1e-6/mẫu, xa vùng subnormal. Ghi chứng minh vào doc string `Biquad.h`;
  test §5.1 **đo gain** chứ không chỉ đo click.
- `clearNotch` không hủy ramp (`NotchChain.cpp:55-63`): slot Idle có thể
  mang `rampRemaining > 0` không ai tick; vô hại vì `setNotch` trên slot
  Idle đi đường reset và hủy ramp — ghi vào doc string (m-4). Ramp bị cắt
  do dừng thiết bị để filter ở depth trung gian trong khi `NotchInfo.depthDB`
  báo đích — 10 ms, chấp nhận, ghi (m-5).

`NotchChain::setNotch(index, freq, Q, depthDB)`:

- Slot **Active** và `freq == info.frequency` và `Q == info.Q` (so sánh
  bằng, cùng giá trị double controller gửi lại) → `rampNotchDepth(...,
  rampSamples = lround(kRampMs × sampleRate_ / 1000))`, `kRampMs = 10.0`.
  Bị từ chối → slot không đổi (như hôm nay).
- Mọi trường hợp khác (Idle, đổi freq/Q) → đường `setNotchFilter` + reset
  như hôm nay. `setSampleRate` retarget giữ nguyên (không ramp; chạy trước
  callback).
- `NotchInfo.depthDB` cập nhật ngay khi nhận lệnh (giá trị đích), không
  theo ramp.

`NotchCommand`: **không thêm trường**. `AudioEngine::drain`: **không đổi**.
Đổi depth = `Set` lên cùng `channel/index/slot` với `depthDB` mới.

### 4.8 Sự kiện và log (lane D)

- `NotchEvent::Kind` thêm `Retune`; `NotchEvent` thêm `RetuneReason
  retuneReason ∈ {Deepen, Release, Reclamp, Ceiling}` và `float
  fromDepthDb`. `depthDb` = depth mới. `hasScore = false`, `ctx = nullptr`
  (không allocate).
- Khóa schema thật là **`ev`**, không phải `kind` (data-loop design
  §108-109). Writer là `MainComponent::notchEventToVar`
  (`MainComponent.cpp:562`), một ternary `Set ? "notch_set" : "notch_clear"`
  — thêm `Kind::Retune` mà không sửa nó thì retune ghi thành `notch_clear`,
  và `tools/logstats.py:59-63` sẽ **đóng bản ghi notch ở lần đào 300 ms**
  (B-3). Vì vậy:
  - `notchEventToVar` phát `ev: "notch_retune"` với `reason`
    (`deepen|release|reclamp|ceiling`), `from_db`, `depth_db`, `age_ms`,
    cùng `slot/lane/index/hz/q` như Set.
  - `tools/logstats.py` xử lý `notch_retune` là **cập nhật** bản ghi đang
    mở (depth hiện tại, số lần retune), không đóng, không mở.
  - Docs data-loop: schema thêm `notch_retune`; ghi rõ reader if/elif bỏ qua
    tên lạ nên file cũ đọc bằng tool mới vẫn chạy.
- Snapshot: `SnapshotNotch.depthDB` đã có → panel hiện số đang chạy. Thêm
  `SnapshotNotch.deepestDb` (cho `savePreset`) và `SnapshotBuffer.releaseFrozen`
  (Q9). GUI không vẽ hai field mới trong 1.2.0.
- **`savePreset` (Q11, M-10)**: `MainComponent::savePreset`
  (`MainComponent.cpp:957`) ghi `pn.depthDB = sn.deepestDb` thay vì
  `sn.depthDB` — preset lưu "phòng đã cần bao nhiêu", không phải bậc đang
  đứng lúc yên. Và ghi `preset.notchDefaults.depthDB/Q` từ
  `getNotchDepthDb()/getNotchQ()` để **trần round-trip** (hôm nay
  `savePreset` không set `notchDefaults`, nạp lại rơi về mặc định −12 của
  `PresetManager.h:146`). Test `test_presetmanager`/GUI: round-trip trần và
  `deepestDb`.

### 4.9 Hằng số (một chỗ, `NotchController.h`; cố định cho 1.2.0, không lên GUI)

| Tên | Giá trị | Nguồn |
|---|---|---|
| `kDepthLadderDb` | −6, −12, −18, −24 | Q2 |
| `kDepthStepDb` | 6 | Q2 |
| `kDeepenAfterMs` | 300 | Q2 |
| `kSteepRiseRatio` | 2.0 | Q6 |
| `kReleaseFirstMs` | 30 000 (= `kAutoReleaseMs` cũ) | Q3 |
| `kReleaseStepMs` | 10 000 | Q3 |
| `kMemoryTtlMs` | 300 000 | Q6 |
| `kMemoryEntriesPerLane` | 16 | §4.6 |
| `kRiskFreezeFraction` | 0.55 (= băng RISING của GUI; một hằng chung, không hardcode hai chỗ) | Q4 |
| `kRampMs` (`NotchChain`) | 10.0 | Q5 |

### 4.10 Invariant an toàn (test pin từng dòng)

1. Controller không bao giờ phát `Set` với `depthDB > 0` hoặc `< −24`
   (kẹp trước khi push; `setNotchImpl` đã từ chối `> 0`).
2. Notch Detector không bao giờ sâu hơn trần; Preset/Manual không bao giờ
   sâu hơn depth được cho. **Kể cả đường kẹp lại**: đích Reclamp là
   `max(deepestDb, ceilingRung)`, và `deepestDb` của notch Detector bị kẹp
   về trần mỗi tick (§4.1, §4.4, §4.5 bước 2). Không có đường nào để một
   `deepestDb` ghi lại dưới trần cũ quay lại làm depth dưới trần mới.
3. Một lần đổi depth theo hướng **sâu** ≤ 6 dB. Theo hướng **nông** có thể
   > 6 dB một lần (hạ trần nhiều bậc) — được phép, nông đi không nguy hiểm.
   Mọi lần đều trải `kRampMs` ⇒ ≤ 0.6 dB/ms theo hướng sâu.
4. Bộ hệ số đích bị `Biquad` từ chối ⇒ slot không đổi, ramp cũ (nếu có)
   không đổi.
5. Ramp chỉ giữa hai bộ cùng `freq`, `Q`, `sampleRate`; đổi bất kỳ cái nào
   ⇒ reset như hôm nay.
6. `processSample` không allocate, không log, không branch ngoài
   `rampRemaining > 0`.
7. Đóng băng chỉ khi `ringRiskValid`; detection tắt ⇒ nhả như hôm nay.
8. Soundcheck: không đào, không nhả, không kẹp (KD-7).

## 5. Kiểm thử (headless, thêm vào suite 454)

### 5.1 `test_biquad`
Cảnh báo M-7: các test so hệ số, đo `max|y|`, hay đo suy giảm steady-state
**đều pass nếu ramp âm thầm `reset()` state**. Hai test đầu dưới đây là hai
test bắt được điều đó; không được bỏ.
- **State không đổi qua `rampNotchDepth`**: accessor test
  `stateForTest() -> {z1, z2}`; chạy sine tới mid-cycle, gọi ramp, assert
  `z1/z2` bit-identical ngay sau lời gọi.
- **Không click**: sine 1 kHz biên 0.5 @44.1k qua ramp −6 → −12 tại mẫu
  10 000; `max|y[n] − y[n−1]|` trong cửa sổ ramp ≤ **1.05×** giá trị cùng
  đại lượng đo trong 441 mẫu ngay trước ramp (số ghi cứng trong test, không
  phải "2× bước lớn nhất").
- **Không boost** (M-4): quét sine 40 tần số trải 20 Hz–20 kHz + 20 tần số
  dày quanh `f0` ± 3 băng thông, đo biên độ steady-state ở **5 điểm giữa
  ramp** (dừng ramp bằng `rampSamples` lớn rồi đo) ⇒ tất cả ≤ 0 dB + 1e-9.
- `rampNotchDepth` tới đúng target: sau `N` mẫu, 5 hệ số == bộ
  `setNotchFilter(4 tham số)` tính riêng (so sánh bit).
- Ramp không diverge: 4 bậc × 2 chiều × 3 sample rate, `max|y| ≤ 1.0` với
  input `|x| ≤ 1`.
- Lệnh thứ hai giữa ramp ⇒ khởi động lại từ hệ số hiện tại, tới đích mới.
- Bị từ chối (freq ≥ sr/2, depth > 0…) ⇒ hệ số, state, `rampRemaining`
  không đổi.
- `setNotchFilter` giữa ramp ⇒ ramp hủy, hệ số = bộ mới, state reset.

### 5.2 `test_notchchain`
- Active cùng freq/Q, `setNotch` depth khác ⇒ ramp (state không reset: đưa
  sine, đầu ra liên tục); khác freq ⇒ reset.
- **Suy giảm đo được tại f0** ở từng bậc −6/−12/−18/−24 trong ±0.5 dB —
  verifier độc lập phải nêu số này (CLAUDE.md: mức level đo trong test).
- `NotchInfo.depthDB` = đích ngay sau lệnh.

### 5.3 `test_notchcontroller`
Fixture (M-12): `tests/test_notchcontroller.cpp:438` hiện assert
`depthDB == -18` cho placement Detector — sẽ đỏ, sửa theo spec. Fixture bật
tone đột ngột sau 64 block noise (`:310-327`, `SineSource` biên cố định) nên
`riseRatio ≫ 2.0` ⇒ mọi test hiện có sẽ ra **−12**, không phải −6. Test
"khởi điểm −6" cần một nguồn **lên biên độ chậm** — thêm `RampSineSource`
vào fixture.

**B-5 (2026-09-07, sửa ví dụ sai của v2):** ví dụ "+3 dB/250 ms" ở bản
trước KHÔNG chạy được, và độ dốc không phải là biến quyết định. Hai điều
kiện phải cùng đúng: (a) rise đủ để `rNorm` bão hòa (score là tích, cần
`rNorm ≥ 0.7` ⇒ `rise ≥ 1.35`; +3 dB/250 ms chỉ cho `rise ≈ 1.18`), và
(b) rise **không bao giờ** là tỉ số tone-trên-noise. Điều (b) mới là cái
giết fixture v2: khung tham chiếu là khung ≥ 112,5 ms tuổi, nên trong ~11
block tone đầu tiên tham chiếu vẫn là NOISE; một tone bật lên ở biên 0.02
trên nền noise 0.01 cho `rise ≈ 64` bất kể dốc bao nhiêu. Vì peakiness là
**bất biến tỉ lệ**, tone thuần đủ peaky ngay khi cửa sổ 2048 mẫu toàn tone
(~4 block) nên confirm rơi đúng vào vùng đó ⇒ luôn ra −12.
Cách duy nhất đúng: **ramp bắt đầu ĐÚNG ở sàn noise** (biên độ sine có
magnitude bin bằng magnitude bin của noise, ≈ 3e-4 với fixture hiện tại),
dốc ~+9,5 dB/250 ms. Khi đó ngay từ frame ĐẦU TIÊN mà scorer chấm điểm
(peakiness phải > 10 trước đã, tức tone ≈ 20 dB trên sàn ≈ 0,53 s) tham
chiếu đã nằm trong tone, `riseRatio ≈ 1,67` ổn định. Chi tiết số học và
cách chỉnh nằm trong plan Task 5.
- Khởi điểm −6 (nguồn chậm); `riseRatio ≥ 2.0` (nguồn đột ngột) → −12;
  `riseRatio` 1.9 → −6.
- Slot tái dùng (B-2): Detector đào tới −24, Clear, đặt tay −6 cùng index
  ⇒ frame reinforce kế **không** Reclamp; `deepestDb == −6`.
- `pushRetuneLocked` giữ `lockedAtMs`: Clear sau 3 lần retune có `ageMs`
  = tuổi từ lúc đặt, không phải từ lần retune cuối.
- Đào: reinforce liên tục ⇒ Set −12 tại ≥ 300 ms, −18 tại ≥ 600 ms, dừng ở
  trần −18; trần −24 ⇒ thêm bậc −24; trần −6 ⇒ không Set nào.
- Hạ trần sống −18 → −12 khi notch ở −18 ⇒ Set −12 reason `Ceiling`, không
  đào lại khi trần nâng về −18 cho tới khi reinforce.
- Thang nhả: yên 29.9 s ⇒ không đổi; 30 s ⇒ −12; +9.9 s ⇒ không; +10 s ⇒
  −6; +10 s ⇒ Clear(AutoRelease).
- Kẹp lại: đang ở −12 (từ deepest −18), reinforce một block ⇒ Set −18 ngay,
  reason `Reclamp`; `quietMs` về 0.
- **Kẹp lại không vượt trần đã hạ** (M-B, 2026-09-07): đào tới −24 dưới trần
  −24 → yên cho tới khi nhả hết thang về −6 (30 + 10 + 10 = 50 s, trước mốc
  Clear ở 60 s) → hạ trần xuống −12 → vài tick yên: `deepestDb` phải đã là
  −12 (nhánh `Set(ceilingRung)` KHÔNG chạy vì −6 nông hơn −12, nên đây là
  test duy nhất bắt được phép kẹp vô điều kiện của §4.5 bước 2) → một block
  hú ⇒ Reclamp về **−12**, không phải −24.
- Đóng băng: snapshot `ringRiskValid=true, score ≥ 0.55×thr` ⇒ `quietMs`
  không tăng; `valid=false` ⇒ tăng bình thường.
- Phòng nhớ: Clear ở f, đặt lại cùng bin trong 5 phút ⇒ khởi điểm =
  deepest cũ (kẹp trần); lệch đúng 1 bin ⇒ −6; 5 phút + 1 ms ⇒ −6; `setWidth`/`clearAll` xóa nhớ;
  mục đã dùng không dùng lần hai.
- Preset: `adoptPreset` depth −12 ⇒ không đào; nhả thang; kẹp lại về −12.
- Soundcheck: không có Set/Clear nào ngoài hôm nay.
- LINKED: hai làn cùng index cùng bậc ở mọi bước; INDEP: làn kia không đổi.
- Event: mỗi đổi depth một `Retune` với reason đúng, `fromDepthDb` đúng,
  `ctx == nullptr`; SessionLogger ghi `ev: "notch_retune"`; `logstats.py` giữ
  bản ghi mở qua retune.
- Không có `Set` nào với depth ngoài [−24, 0] trong toàn bộ fixture (fuzz
  nhỏ: 200 block ngẫu nhiên reinforce/yên/frozen).

### 5.4 Snapshot tool / ảnh
- `HandsFreeSnapshot` không cần stage mới; ảnh `console-live.png` gửi owner
  theo luật GUI, số depth trên panel là −6/−12 thay vì −18.

## 6. Việc phải làm ngoài code

- `docs/GIOI-THIEU.md`, `docs/KY-THUAT-CHONG-HU.md`: mục depth/nhả viết lại
  theo thang; nêu "nghe ở âm lượng thấp trước".
- `docs/spec-ring-risk.md`: thêm §"Người đọc thứ hai: thang nhả lane G".
- Code ngoài `NotchController`/`dsp` phải đổi (B-3): `src/app/MainComponent.cpp`
  (`notchEventToVar`), `tools/logstats.py`, `tests/test_notchcontroller.cpp:438`.
- Docs data-loop: schema thêm `ev: "notch_retune"`.
- `installer/TESTER-NOTES.md`: mục 1.2.0 — cái gì đổi, nghe gì, báo gì.
- `docs/release-notes/1.2.0-alpha.md`.
- Roadmap v2: lane G → "đã hạ cánh 1.2.0"; lane A ghi "fallback notch = thang G".
- `memory/` note nếu học được gì; sổ quyết định cập nhật nếu phản biện lật.

## 7. Quyết định đã chốt trong spec (để plan không hỏi lại)

- Trần đọc **sống**, không chụp lúc đặt (user hạ slider phải có tác dụng ngay).
- `quietMs` là **bộ đếm tích lũy**, không phải mốc — để đóng băng được.
- Kẹp lại **không** chờ 300 ms; đào **có** chờ 300 ms.
- Phòng nhớ **không** persist; TTL 5 phút; cùng bin; mục dùng một lần.
- `riseRatio` là field mới trong `ScoreBreakdown`, không đổi `score`.
- `NotchCommand`/`AudioEngine` không đổi; retune là `Set` lên index đang chạy.
- Hằng số cố định cho 1.2.0, không đưa lên GUI.
- Hạ trần nhiều bậc một lần theo hướng nông là hợp lệ (invariant 3).
- Trần lẻ: thang = bậc nông hơn trần + chính trần làm bậc cuối (Q13, thay
  cho lượng tử về bậc nông hơn của v2).
- Thang **dừng ở điểm cân bằng** (bậc đầu tiên làm bin hết vượt ngưỡng), không
  ép về trần theo đồng hồ (Q7).
- Slider depth chỉ kéo notch Detector; Preset/Manual giữ `ceilingDb` riêng (Q8).
- Đóng băng không có trần thời gian; `releaseFrozen` publish, không vẽ (Q9).
- Phòng nhớ khớp cùng bin ±0 (Q10, lật ±1 bin của Q6); chỉ làm sâu hơn,
  `min(depth, remembered)` (Q14).
- `savePreset` lưu `deepestDb` và round-trip trần vào `notchDefaults` (Q11).
- Depth < −24 kẹp về −24 lúc `setNotchImpl`, mọi Origin, có log (Q12).
- Không phải việc G, chỉ ghi nhận: default `notchDefaults.depthDB = −12` của
  `PresetManager` lệch với −18 của controller (M-3); I-3/`setSampleRate`/A-R7
  của lane R vẫn treo.

## 8. Phản biện đã xử lý

Vòng 1, 2026-09-06, agent read-only `az-harness:verifier` (opus), 39 lần
đọc code, tự chạy số. Kết quả: 4 BLOCKER, 12 MAJOR, 8 MINOR, 1 chứng minh
DSP xác nhận.

| ID | Nội dung | Xử lý |
|---|---|---|
| B-1 | `processBlock` không tồn tại; vòng reinforce và auto-release chạy dưới `modelMutex_`, gọi `setNotch` = deadlock; đi qua `setNotchImpl` thì ghi đè `lockedAtMs` (nhãn lane D) | §4.4: `pushRetuneLocked`, giữ `lockedAtMs/origin` |
| B-2 | 3 trường mới không khởi tạo khi tái dùng slot ⇒ notch Manual −6 bị Reclamp về −24 cũ | §4.2: khởi tạo trong `setNotchImpl`, mọi Origin; test §5.3 |
| B-3 | Khóa log là `ev`; ternary `notchEventToVar` ghi Retune thành `notch_clear`; `logstats.py` đóng bản ghi ở 300 ms | §4.8: `notch_retune`, sửa writer + reader, §6 |
| B-4 | Bảng §3 sai chiều hàng nhả (30→50 s **sâu hơn** hôm nay), thiếu hàng phòng nhớ, thiếu cận trên khi đóng băng | §3 viết lại, cột "so với hôm nay" |
| M-1 | Preset depth < −24 không bị chặn hôm nay; invariant 1 đã sai sẵn | Q12 |
| M-2 | Trần sống vs trần riêng Preset/Manual mâu thuẫn; thiếu `ceilingDb` | Q8 |
| M-3 | Trần không phải bội của 6; default preset −12 ≠ controller −18 | §4.1: `ceilingRung`; ghi nhận default lệch |
| M-4 | Tam giác `(a1,a2)` chỉ phủ mẫu số; boost giữa ramp chưa chứng minh | §4.7: chứng minh `A_n ≤ 1 ≤ 1/A_d`, đo 0 dB; test gain §5.1 |
| M-5 | §4.5 tạo thứ tự khóa mới model→snapshot | §4.5: đọc `frameMaxScore_/frameScoreValid_` |
| M-6 | Test đóng băng không bơm được score ≥ 0.385 suốt 30 s | §4.5: seam `setRingRiskOverrideForTest`, `quietMsForTest` |
| M-7 | 4 test §5 pass dù ramp reset state | §5.1: assert `z1/z2`, ngưỡng click ghi số |
| M-8 | Chip hold 750 ms và chỉ theo `displayedSlot_` ⇒ chip và thang lệch | Q9 |
| M-9 | Đào và "còn hú" cùng phép thử ⇒ thang kẹt ở bậc nông; threshold ≥ 18 kẹt −12 | §3 hàng "kẹt", §4.4 ghi số; Q7 |
| M-10 | `savePreset` lưu bậc đang đứng; `notchDefaults` (trần) không round-trip | Q11 |
| M-11 | Phòng nhớ per-lane vs LINKED mồ côi; ±1 bin = ±21.5 Hz | §4.6 LINKED cả hai làn; dung sai Q10 |
| M-12 | Test hiện có assert −18; fixture tone đột ngột ⇒ mọi test ra −12 | §5.3 |
| m-1..m-8 | 5 vs 4 điều kiện; so sánh Q; so sánh bằng double; `clearNotch` không hủy ramp; ramp bị cắt; reader bỏ qua; `setSampleRate` không caller; guard tồn tại | §4.7, §4.4, §4.2 `releasedSteps`, §4.6, §3 |

Vòng 2, 2026-09-07, phiên read-only đọc lại code thật trong khi soát plan.
Chỉ hai mục chạm vào spec; phần còn lại là lỗi của plan và đã sửa trong
plan rev 3.

| ID | Nội dung | Xử lý |
|---|---|---|
| B-5 | Fixture ramp không thể ra −6: cơ chế quyết định không phải ĐỘ DỐC mà là ĐIỂM BẮT ĐẦU — 11 block tone đầu lấy khung tham chiếu là NOISE nên `riseRatio ≈ 64`, và peakiness bất biến tỉ lệ nên confirm rơi ngay trong cửa sổ đó ⇒ luôn −12 | §5.3: ramp phải khởi hành ĐÚNG ở sàn noise (≈ 3e-4); số học trong plan Task 5 |
| M-B | Kẹp lại có thể vượt trần: đào −24 → nhả về −6 → hạ trần −12 (nhánh `Set(ceilingRung)` không chạy vì −6 nông hơn −12, `deepestDb` vẫn −24) → hú quay lại ⇒ Reclamp −24, sâu hơn trần 12 dB. Vi phạm Q1 và invariant 2 có từ v2 | §4.1 + §4.5 bước 2: kẹp `deepestDb` về trần VÔ ĐIỀU KIỆN mỗi tick cho notch Detector; §4.4: đích Reclamp là `max(deepestDb, ceilingRung)`; §4.10 inv. 2 nói rõ; test mới ở §5.3 |

CONFIRMED (không cần verify lại): `reset()` ở `Biquad.cpp:68,126`; default
−18 / clamp / đọc lúc đặt; auto-release `.cpp:404-416`; `rNorm` bão hòa
1.5; magnitude là biên độ nên `riseRatio 2.0 = +6 dB`; ring-risk chỉ GUI đọc;
0.55 và 0.7 đúng chỗ; drain `AudioEngine.cpp:439-442` không nhánh khác;
LINKED cùng bậc; `FakeClock` + `pump()` 10.667 ms có sẵn; ramp không boost,
không mất ổn định, không denormal; 10 ms ramp ≤ 1 block ở 2048, 7 block ở 64.
