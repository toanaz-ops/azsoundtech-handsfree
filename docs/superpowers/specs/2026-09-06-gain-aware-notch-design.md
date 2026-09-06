# Lane G — Gain-aware notch: depth theo nhu cầu, nhả dần, nối ring-risk

**Ngày:** 2026-09-06. **Roadmap:** [`2026-09-04-anti-feedback-v2-roadmap.md`](2026-09-04-anti-feedback-v2-roadmap.md) (lane G, chờ S + D + R — cả ba đã hạ cánh, main `6b8d084`, 1.1.3 alpha, suite 454/454).
**Sổ quyết định:** [`../decisions/2026-09-06-lane-g-gain-aware-notch.md`](../decisions/2026-09-06-lane-g-gain-aware-notch.md) (Q1–Q6, đừng hỏi lại).
**Trạng thái:** spec, chờ phản biện read-only rồi owner duyệt. **Đụng audio path:** có (`Biquad`, `NotchChain`). **Release:** 1.2.0 (`-Part minor`).

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

| Thời điểm | Tại bin của notch | Ngoài bin |
|---|---|---|
| Đặt notch (hôm nay −18) | cắt **−6** (hoặc −12 nếu rise dốc). Hú được nén **ít hơn 12 dB** trong tối đa ~600 ms (2 bước × 300 ms) trước khi tới −18; ~300 ms nếu nhảy −12 | 0 dB |
| Đào sâu | mỗi bước −6 dB, trải 10 ms | 0 dB |
| Nhả | mỗi bậc **+6 dB**, cách nhau ≥ 10 s (bậc đầu ≥ 30 s), không nhả khi RING RISK ≥ RISING | 0 dB |
| Kẹp lại | về `deepestDb` trong một bước, trải 10 ms | 0 dB |
| Hạ trần sống | về trần trong một bước, trải 10 ms | 0 dB |

Master, limiter, clamp output, NaN/denormal guard: **không đụng**. Chỗ tester
phải nghe ở âm lượng thấp trước: **hàng "đặt notch"** — phòng hú bùng nhanh
sẽ nghe hú lâu hơn hôm nay tới ~0.3–0.6 s trước khi bị nén hết.

## 4. Thiết kế

### 4.1 Bậc depth và trần

- Bậc: `kDepthLadderDb[] = {−6, −12, −18, −24}`. Bậc *sâu hơn* = số âm hơn.
- **Trần** = `notchDepthDb_` (preset, kẹp [−24, −6]) đọc **sống** mỗi tick.
  Notch Detector không bao giờ sâu hơn trần. Trần −6 ⇒ không bao giờ đào.
- Hạ trần giữa chừng: notch đang sâu hơn trần mới → `Set(trần)` trong tick
  kế (reason `Ceiling`), `deepestDb` cũng kẹp về trần. Nâng trần: không tự
  đào, chờ reinforce như bình thường.

### 4.2 Trạng thái mỗi notch (`ModelNotch`, thread detector)

Thêm:

| Trường | Nghĩa |
|---|---|
| `deepestDb` | bậc sâu nhất từng đứng kể từ khi đặt (cũng là đích của kẹp lại) |
| `stageChangedAtMs` | `liveMs_` lần đổi depth gần nhất (đào, nhả, kẹp, trần) |
| `quietMs` | thời gian **tích lũy** bin yên (không reinforce) kể từ lần đổi depth/reinforce gần nhất; đóng băng được |

`lastDetectedMs` giữ nguyên cho snapshot/log; auto-release **không** đọc nó
nữa mà đọc `quietMs`.

Đọc thêm từ scorer: `CandidateScorer::ScoreBreakdown` thêm `riseRatio`
(tỉ số thô `mag_now / mag_ref`, chưa chuẩn hóa; 1.0 khi chưa có history —
cùng nhánh trả `rNorm`). `rNorm` hiện bão hòa ở rise 1.5 nên không phân
biệt được "dốc". `score` **không đổi**; một đường số học, chỉ lộ thêm một
số trung gian (giữ nguyên tắc lane D).

### 4.3 Đặt (`placeConfirmed`)

1. `depth = −6`.
2. Nếu `pc.breakdown.riseRatio ≥ kSteepRiseRatio (2.0)` → `depth = −12`.
3. Nếu "phòng nhớ" (§4.6) có mục cùng làn, `|f − f_nhớ| ≤ 1 bin`, chưa hết
   hạn → `depth = deepestDb_nhớ`.
4. `depth = max(depth, trần)` (kẹp: không sâu hơn trần).
5. `deepestDb = depth`, `stageChangedAtMs = liveMs_`, `quietMs = 0`.
6. Phần còn lại (index, LINKED, event Set, ctx) như hôm nay.

Origin Preset/Manual (`adoptPreset`, `setNotch` từ GUI): `deepestDb =`
depth được cho, và **trần riêng** của notch đó = depth được cho — không đào
(preset đã nói rõ muốn bao nhiêu). Origin Soundcheck: không có thang, không
đào, không nhả (KD-7).

### 4.4 Đào (trong vòng reinforce của `processBlock`)

Vòng reinforce hiện có đã xét từng notch active không-Soundcheck và test
`peakinessAt(bin) > la.analyzer.getThreshold()`. Thêm tại chỗ đó, khi
reinforce **trúng**:

- `quietMs = 0`, `lastDetectedMs = liveMs_` (như hôm nay).
- Nếu `origin == Detector` **và** `depthDB > trần` (còn nông hơn) **và**
  `liveMs_ − stageChangedAtMs ≥ kDeepenAfterMs (300)`: `depth = max(depth −
  6, trần)`, `deepestDb = min(deepestDb, depth)`, `stageChangedAtMs =
  liveMs_`, push `Set(depth)` reason `Deepen`.
- Nếu **đang nhả** (`depthDB > deepestDb`, mọi origin trừ Soundcheck): kẹp
  lại **ngay** `depth = deepestDb`, `stageChangedAtMs = liveMs_`, push
  `Set` reason `Reclamp`. Không chờ 300 ms.

LINKED: hai làn cùng index được reinforce từ cùng frame (chính sách lane S
giữ nguyên) nên cùng bậc; test pin điều này.

### 4.5 Nhả (`runOnce` bước 3, thay khối auto-release)

Chỉ khi `tapAlive`. Với `dt` = bước live clock của tick này:

1. **Đóng băng**: đọc `ringRiskValid`/`ringRiskScore`/`ringRiskThreshold`
   của snapshot mới nhất (cùng `snapshotMutex_`, hoặc bản sao đã giữ ở bước
   publish). `frozen = ringRiskValid && ringRiskScore ≥ 0.55 ×
   ringRiskThreshold` (đúng ranh RISING của `SpectrumView::riskForScore`,
   để chip và thang nhả không cãi nhau). `frozen` ⇒ không cộng `quietMs`
   cho notch nào tick này. `ringRiskValid == false` (detection tắt) ⇒
   **không** đóng băng — nhả vẫn chạy như hôm nay khi detection tắt.
2. Với mỗi notch active, origin ≠ Soundcheck: nếu `!frozen`, `quietMs += dt`.
3. Ngưỡng nhả: `quietMs ≥ kReleaseFirstMs (30 000)` khi `depthDB ==
   deepestDb` (bậc đầu), `≥ kReleaseStepMs (10 000)` khi đã nông hơn
   `deepestDb`.
4. Đủ ngưỡng: nếu `depthDB < −6` → `depth += 6` (kẹp về bậc gần nhất của
   thang, không vượt −6), `stageChangedAtMs = liveMs_`, `quietMs = 0`, push
   `Set` reason `Release`. Nếu `depthDB ≥ −6` → `pushClearLocked(c, i,
   AutoRelease)` và ghi "phòng nhớ" (§4.6).

`kAutoReleaseMs` (30 000) giữ tên/giá trị làm `kReleaseFirstMs`; doc string
đổi. `kTapSilenceTimeoutMs` không đổi.

### 4.6 Phòng nhớ (`ReleasedMemory`, thread detector)

- Mỗi làn một mảng cố định 16 mục `{frequencyHz, deepestDb, clearedAtMs}`,
  ghi vòng (đè cũ nhất). Không allocate.
- Ghi khi Clear do `AutoRelease` từ bậc −6 (không ghi Manual/ClearAll/
  WidthChange/VerdictFalse/PartialApplyUnwind).
- Đọc ở §4.3 bước 3: cùng làn, `|Δf| ≤ 1 bin` (bin = `sampleRate /
  kFftSize`), `liveMs_ − clearedAtMs ≤ kMemoryTtlMs (300 000)`. Trúng thì
  **xóa mục** (đã dùng).
- Xóa toàn bộ khi `setWidth`, `clearAll`, `setSampleRate` (cùng chỗ reset
  `blocksSinceReset`). Không persist ra preset/đĩa.

### 4.7 DSP: retune giữ state có ramp (`src/dsp`)

`Biquad`:

- Thêm `bool rampNotchDepth(double freq, double Q, double sampleRate,
  double depthDB, int rampSamples)`. Tính bộ hệ số đích bằng **cùng công
  thức và cùng 5 điều kiện từ chối** của `setNotchFilter` 4 tham số
  (`sampleRate > 0`, `Q > 0`, `0 < freq < sr/2`, `depthDB ≤ 0`). Bị từ
  chối → `false`, filter (hệ số, state, ramp đang chạy) **không đổi**.
- Hợp lệ → **không** `reset()`. Lưu `target[5]`, `delta[5] = (target −
  current) / rampSamples`, `rampRemaining = rampSamples`. `rampSamples ≤ 0`
  → gán thẳng target, không reset (đường "giữ state, không ramp").
- `processSample`: nếu `rampRemaining > 0`: cộng `delta` vào 5 hệ số, giảm
  đếm; khi về 0 **gán thẳng `target`** (không để sai số tích lũy). Nhánh
  này là một so sánh + 5 phép cộng, không allocate.
- `setNotchFilter` (cả hai dạng) và `reset()` giữ nguyên hành vi, và **hủy
  ramp đang chạy** (`rampRemaining = 0`) — bộ hệ số mới thắng.
- Ổn định trong ramp: `(a1, a2)` chuẩn hóa của biquad ổn định nằm trong tam
  giác `|a2| < 1`, `|a1| < 1 + a2`. Tam giác lồi; hai đầu ramp đều ổn định
  (cùng validation) ⇒ mọi điểm nội suy tuyến tính đều ổn định. Ghi chứng
  minh này vào doc string; test §5.1 khẳng định bằng đo.

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
- `SessionLogger` ghi `kind: "retune"`, `reason`, `from_db`, `depth_db`,
  `age_ms`. Schema docs data-loop thêm một dòng; reader cũ gặp kind lạ thì
  bỏ qua (kiểm tra và ghi rõ trong docs nếu không phải vậy).
- Snapshot: `SnapshotNotch.depthDB` đã có → panel hiện số đang chạy. Không
  thêm field.

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
   sâu hơn depth được cho.
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
- `rampNotchDepth` tới đúng target: sau `N` mẫu, 5 hệ số == bộ
  `setNotchFilter(4 tham số)` tính riêng (so sánh bit).
- Sine tại `f0` qua ramp −6 → −12: biên độ sau ramp trong ±0.5 dB của −12;
  **không có bước nhảy mẫu-kề-mẫu** lớn hơn 2× bước lớn nhất của tín hiệu
  ổn định (đo `max|y[n] − y[n−1]|` trong ramp so với ngoài ramp) — đây là
  test "không click".
- Ramp không diverge: 5 bậc × 2 chiều × 3 sample rate, `max|y| ≤ 1.0` với
  input `|x| ≤ 1`.
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
- Khởi điểm −6; `riseRatio ≥ 2.0` → −12; `riseRatio` 1.9 → −6.
- Đào: reinforce liên tục ⇒ Set −12 tại ≥ 300 ms, −18 tại ≥ 600 ms, dừng ở
  trần −18; trần −24 ⇒ thêm bậc −24; trần −6 ⇒ không Set nào.
- Hạ trần sống −18 → −12 khi notch ở −18 ⇒ Set −12 reason `Ceiling`, không
  đào lại khi trần nâng về −18 cho tới khi reinforce.
- Thang nhả: yên 29.9 s ⇒ không đổi; 30 s ⇒ −12; +9.9 s ⇒ không; +10 s ⇒
  −6; +10 s ⇒ Clear(AutoRelease).
- Kẹp lại: đang ở −12 (từ deepest −18), reinforce một block ⇒ Set −18 ngay,
  reason `Reclamp`; `quietMs` về 0.
- Đóng băng: snapshot `ringRiskValid=true, score ≥ 0.55×thr` ⇒ `quietMs`
  không tăng; `valid=false` ⇒ tăng bình thường.
- Phòng nhớ: Clear ở f, đặt lại f±0.5 bin trong 5 phút ⇒ khởi điểm =
  deepest cũ (kẹp trần); 5 phút + 1 ms ⇒ −6; `setWidth`/`clearAll` xóa nhớ;
  mục đã dùng không dùng lần hai.
- Preset: `adoptPreset` depth −12 ⇒ không đào; nhả thang; kẹp lại về −12.
- Soundcheck: không có Set/Clear nào ngoài hôm nay.
- LINKED: hai làn cùng index cùng bậc ở mọi bước; INDEP: làn kia không đổi.
- Event: mỗi đổi depth một `Retune` với reason đúng, `fromDepthDb` đúng,
  `ctx == nullptr`; SessionLogger ghi `kind: "retune"`.
- Không có `Set` nào với depth ngoài [−24, 0] trong toàn bộ fixture (fuzz
  nhỏ: 200 block ngẫu nhiên reinforce/yên/frozen).

### 5.4 Snapshot tool / ảnh
- `HandsFreeSnapshot` không cần stage mới; ảnh `console-live.png` gửi owner
  theo luật GUI, số depth trên panel là −6/−12 thay vì −18.

## 6. Việc phải làm ngoài code

- `docs/GIOI-THIEU.md`, `docs/KY-THUAT-CHONG-HU.md`: mục depth/nhả viết lại
  theo thang; nêu "nghe ở âm lượng thấp trước".
- `docs/spec-ring-risk.md`: thêm §"Người đọc thứ hai: thang nhả lane G".
- Docs data-loop: schema thêm `kind: "retune"`.
- `installer/TESTER-NOTES.md`: mục 1.2.0 — cái gì đổi, nghe gì, báo gì.
- `docs/release-notes/1.2.0-alpha.md`.
- Roadmap v2: lane G → "đã hạ cánh 1.2.0"; lane A ghi "fallback notch = thang G".
- `memory/` note nếu học được gì; sổ quyết định cập nhật nếu phản biện lật.

## 7. Quyết định đã chốt trong spec (để plan không hỏi lại)

- Trần đọc **sống**, không chụp lúc đặt (user hạ slider phải có tác dụng ngay).
- `quietMs` là **bộ đếm tích lũy**, không phải mốc — để đóng băng được.
- Kẹp lại **không** chờ 300 ms; đào **có** chờ 300 ms.
- Phòng nhớ **không** persist; TTL 5 phút; ±1 bin; mục dùng một lần.
- `riseRatio` là field mới trong `ScoreBreakdown`, không đổi `score`.
- `NotchCommand`/`AudioEngine` không đổi; retune là `Set` lên index đang chạy.
- Hằng số cố định cho 1.2.0, không đưa lên GUI.
- Hạ trần nhiều bậc một lần theo hướng nông là hợp lệ (invariant 3).

## 8. Phản biện đã xử lý

(chờ vòng read-only opus)
