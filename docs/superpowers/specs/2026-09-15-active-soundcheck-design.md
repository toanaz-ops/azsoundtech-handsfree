# Lane M — Soundcheck đo chủ động: quét sweep từng ngõ ra, đo loop gain, đề xuất notch phòng ngừa

**Ngày:** 2026-09-15. **Roadmap:** [`2026-09-04-anti-feedback-v2-roadmap.md`](2026-09-04-anti-feedback-v2-roadmap.md) (lane M, cần S — S đã hạ cánh; main `a6be099`, 1.2.0 alpha, suite 547/547).
**Sổ quyết định:** [`../decisions/2026-09-15-lane-m-active-soundcheck.md`](../decisions/2026-09-15-lane-m-active-soundcheck.md) (Q1–Q15).
**Trạng thái:** **bản nháp v1, CHƯA phản biện, CHƯA owner duyệt.** Mọi `Q` trong sổ quyết định là lựa chọn **tạm của điều phối** ngày 2026-09-15 — owner vắng mặt. Không dòng code nào được viết theo spec này cho tới khi owner duyệt ít nhất Q2 (mức phát), Q6 (tự đặt hay đề xuất) và Q15 (tắt đường mic khi quét).
**Đụng audio path:** **có, và nặng nhất từ trước tới nay** — lần đầu app **tự sinh tín hiệu và phát ra PA**. **Release:** 1.3.0 (`-Part minor`), không gộp với lane nào khác.

---

## 1. Vấn đề

Đọc code 2026-09-15 (main `a6be099`):

- App hôm nay **hoàn toàn thụ động**. Nó chỉ trừ gain khỏi tín hiệu người khác
  đưa vào. "Soundcheck" trong app là `AudioEngine::Mode::Soundcheck`
  (`src/app/AudioEngine.h:66`) → `NotchController::startSoundcheck()`
  (`src/app/MainComponent.cpp:801-802`), tức là **ngồi nghe 15 giây và chờ phòng
  tự hú** (`kSoundcheckDurationMs`, `src/app/NotchController.h:91`). Research §0
  gọi đúng tên: *"Soundcheck = chờ phòng tự hú 15 s. Không có phép đo chủ
  động."*
- Hệ quả: **notch đầu tiên luôn tới sau tiếng hú đầu tiên.** Với thang lane G
  (đặt −6, đào 6 dB mỗi 300 ms, `src/app/NotchController.h:109-118`), khán
  phòng nghe khoảng 0,3–0,9 s tiếng hú trước khi filter đủ sâu. Lane G làm cho
  phần đó *nông hơn*; lane M là lane duy nhất có thể làm cho nó *không xảy ra*.
- Lane G cũng **thiếu một phép đo** vì lý do này: công thức "τ ringing → loop
  gain vượt X dB" cần round-trip delay của vòng hú, không ai đo, nên lane G
  phải đi đường thực nghiệm (spec lane G §1). Lane M là chỗ con số đó sinh ra.
- Lane A (AFC) **không viết được** trước lane M: roadmap ghi thẳng "A cần M (đo
  loa→mic)".

Ba sự thật kỹ thuật định hình toàn bộ thiết kế dưới đây, cả ba đã kiểm chứng
trong code, không phải giả định:

1. **Tap mà detector đọc là OUTPUT chứ không phải mic.** Callback gán
   `tapSource[slot][lane] = out` (`src/app/AudioEngine.cpp:561`) rồi ghi ring từ
   con trỏ đó (`src/app/AudioEngine.cpp:657-666`). Bất kỳ tín hiệu nào lane M
   trộn vào `out` sẽ quay lại chính đường detector.
2. **Không có limiter trong app.** Đường ra chỉ có một **kẹp cứng** ±1.0f
   (`kMaxOutputLevel`, `src/app/AudioEngine.cpp:15`, áp ở `:631-647`) — đó là
   clipper toàn thang. Prompt gốc của owner viết "limiter vẫn nằm trong đường";
   sự thật là **không có**, nên lane M phải tự kẹp ở nguồn, không dựa vào cái gì
   phía sau.
3. **`setNotch` ghi đè im lặng.** `setNotchImpl`
   (`src/app/NotchController.cpp:195`) không kiểm `n.active` trước khi ghi đè ô
   `index`, và với origin khác `Detector` nó đặt luôn `ceilingDb = depth`.
   `firstFreeIndexLocked` (`src/app/NotchController.cpp:1064`) là **private**.
   Lane M phải tự tránh va chạm index, và §4.6 nói tránh bằng cách nào.

## 2. Mục tiêu và phi mục tiêu

**Mục tiêu:**

1. Một hành động "ĐO" phát **log sine sweep** qua **từng ngõ ra một**, thu mic
   tương ứng, và tính **loop gain theo tần số** cho từng làn (Q1, Q4, Q5).
2. Từ đường loop gain đó, **đề xuất** notch phòng ngừa tại các bin còn ít
   margin, với độ sâu lượng tử lên **đúng thang lane G**; người vận hành bấm
   `ÁP DỤNG` thì mới có gì tới PA (Q6, Q7).
3. Mọi notch phòng ngừa đi qua **đúng một API có sẵn**:
   `NotchController::setNotch(..., Origin::Soundcheck)`
   (`src/app/NotchController.h:221-223`). Không có đường thứ hai xuống chain.
4. Vẽ kết quả lên analyser: đường margin + marker ứng viên (Q10).
5. Ghi năm sự kiện `soundcheck_*` vào log session lane D (Q9).
6. **Không thể hú trong lúc đo**: vòng hú bị mở bằng cách tắt đóng góp của slot
   đang đo vào ngõ ra đang đo (Q15).

**Phi mục tiêu** (ghi để plan không lấn):

- **Không** tự đặt notch. v1 chỉ đề xuất (Q6). Tự đặt là bậc sau, mở khi rig có
  bằng chứng.
- **Không** đo chéo làn (out L → mic R). Vòng chéo có thật trong hệ stereo và
  đóng qua chain của làn kia; v1 chỉ đo vòng cùng làn. Ghi vào §7 rủi ro.
- **Không** giải chập ra đáp ứng xung, **không** ước lượng τ/Q của mode phòng.
  Lane A cần thứ đó; lane M v1 chỉ ra `|H|` theo bin (Q5 PA 3 bị loại).
- **Không** đổi định dạng preset (Q9).
- **Không** đổi mode `Soundcheck` thụ động đang có, không thêm mode thứ tư
  (Q12).
- **Không** đổi thuật toán dò, scorer, hay thang lane G. Lane M chỉ **đọc** các
  hằng số của lane G.
- **Không** tự tăng mức phát để cải thiện SNR (Q8 PA 2 bị loại rõ ràng).

## 3. Mức thay đổi level dự kiến (bắt buộc theo CLAUDE.md)

> **Lane M là thay đổi level lớn nhất của dự án tới nay: lần đầu tiên app tự
> sinh tín hiệu và phát ra PA, thay vì chỉ trừ gain khỏi tín hiệu người khác
> gửi vào. Từ `AudioEngine` trở ra, sweep đi qua đúng một kẹp cứng ±1.0f
> (`src/app/AudioEngine.cpp:631-647`) — trong app **không có limiter nào cả**.
> Mức âm thanh thật trong phòng do **fader master của người vận hành** quyết
> định, không do app; đó là lý do mọi lần chạy đều bị chặn bởi một hộp thoại
> "HẠ MASTER TRƯỚC" phải bấm xác nhận.**

Bằng số, so với 1.2.0:

| Thời điểm | Trên ngõ ra ĐANG đo | Trên các ngõ ra khác | Ghi chú |
|---|---|---|---|
| Trước khi bấm xác nhận | 0 dB (không đổi) | 0 dB | Không phát gì |
| Ramp vào | từ −∞ lên **−20 dBFS đỉnh** trong **30 ms**, raised-cosine | 0 dB | Q2 |
| Trong sweep (3,0 s) | sweep đỉnh **−20 dBFS**, RMS **≈ −23 dBFS**; **đồng thời đường mic của slot đó về im** (Q15) | 0 dB | Xem hai đoạn bên dưới |
| Đuôi (0,7 s) | im (−∞), đường mic vẫn im | 0 dB | Đo phần ngân |
| Giữa hai ngõ ra (300 ms) | im, đường mic **đã trả lại** | 0 dB | |
| Dừng khẩn | ramp xuống −∞ trong **30 ms** rồi đường mic trả lại ngay | 0 dB | Không cắt phựt |
| Sau khi `ÁP DỤNG` | mỗi bin ứng viên bị cắt **−6 / −12 / −18 / −24 dB** (bậc thang lane G), tối đa **6 bin mỗi làn** | 0 dB | Q7, Q14 |
| Sau khi `BỎ` | 0 dB | 0 dB | Không gì được đặt |

**Quy ra dB SPL trong phòng.** App không biết và không thể biết SPL tuyệt đối.
Điều nói được là **tỉ số**: một sweep đỉnh −20 dBFS có RMS ≈ −23 dBFS, tức thấp
hơn khoảng **5 dB** so với mức RMS chương trình điển hình (−18 dBFS). Nếu hệ
đang chỉnh sao cho chương trình đạt đỉnh ~100 dB SPL ở vị trí mic, sweep sẽ nằm
quanh **80 dB SPL** ở vị trí đó — nghe rõ, khó chịu nếu bất ngờ, **không** gây
hại, và thấp hơn ngưỡng hú thông thường. Nhưng con số đó chỉ đúng nếu master
đang ở mức chạy show; nếu master đang mở hết, cùng một −20 dBFS có thể là 110 dB
SPL. **Đó là toàn bộ lý do có hộp thoại cảnh báo, và lý do mức phát chỉ chỉnh
được XUỐNG.**

**Sự sụt level ít người để ý, nhưng phải nói ra.** Q15 làm đường mic của slot
đang đo **im hoàn toàn trong 3,7 s**. Nếu ai đó bấm ĐO giữa lúc MC đang nói, MC
biến mất khỏi loa đúng 3,7 giây. Đây là thay đổi level âm, không phải dương, và
nó là có chủ ý: nó **mở vòng hú**, nên phép đo không thể tự gây hú.

**Cơ chế an toàn — tất cả đều là code, không phải kỷ luật vận hành:**

| Cơ chế | Chi tiết |
|---|---|
| Kẹp biên độ tại nguồn | `SoundcheckSignal` nhân biên độ đã `jlimit` vào `kSoundcheckMaxPeak = 0.1f` (−20 dBFS). Không API nào nhận giá trị lớn hơn; giá trị lớn hơn bị kẹp, không bị từ chối im lặng |
| Kẹp cuối đường | Kẹp ±1.0f có sẵn (`src/app/AudioEngine.cpp:631-647`) vẫn nằm **sau** điểm tiêm — xem §4.1. Không được bỏ |
| Ramp | 30 ms raised-cosine vào và ra, kể cả khi dừng khẩn |
| Dừng khẩn | Nút `DỪNG` + **bất kỳ phím nào** + engine dừng + thiết bị lỗi. Cả bốn đi vào cùng một `abort()` |
| Tự hủy | Mic > −6 dBFS RMS trong > 20 ms; `ringRiskScore ≥ ringRiskThreshold`; ring thu của lane M bị drop |
| Từ chối chạy | Engine chưa `isRunning()`; `getNumInputChannels()`/`getNumOutputChannels()` còn 0; slot disable; RING RISK đang ≥ RISING |
| Không thể hú | Vòng hở trong suốt lần quét (Q15) |
| Không detector nào phản ứng với sweep | Detection tắt trên **mọi** slot trong suốt lần chạy (Q12), và tap bị **treo** nên `liveMs_` của mọi controller đứng yên (§4.1) |
| Chết an toàn | Trạng thái nghỉ của `SoundcheckController` phát 0.0f. Mọi lỗi, exception, hay huỷ đối tượng đều rơi về đó |

## 4. Thiết kế

### 4.1 Kiến trúc và bốn điểm chèn THẬT trong `AudioEngine`

Ba lớp mới, hai trong `src/dsp/` (thuần tính toán, test được không cần thiết
bị), một trong `src/app/`:

| Lớp | File | Thread | Việc |
|---|---|---|---|
| `SoundcheckSignal` | `src/dsp/SoundcheckSignal.h` | audio + controller | Hàm **thuần** của chỉ số mẫu → biên độ sweep. Không state ngoài chỉ số, không cấp phát |
| `LoopGainEstimator` | `src/dsp/LoopGainEstimator.h/.cpp` | controller | Cộng dồn `Σ|X|²` và `Σ|Y|²` theo bin, ra `H_dB[k]`, làm trơn 1/3 octave, nhặt ứng viên |
| `SoundcheckController` | `src/app/SoundcheckController.h/.cpp` | thread riêng, poll 5 ms như `NotchController::run()` | Máy trạng thái, lái `AudioEngine`, đọc ring thu, gọi `NotchController::setNotch` khi APPLY |

`AudioEngine` nhận **năm** thành viên mới, tất cả `std::atomic` hoặc ring
lock-free — callback vẫn không khoá, không cấp phát:

```cpp
// -1 = không quét. Chỉ số kênh ĐẦU RA đang mang sweep.
std::atomic<int>      scOutChannel_    { -1 };
// (slot, lane) bị tắt đóng góp trong lúc quét (Q15). -1 = không tắt.
std::atomic<int>      scMuteSlot_      { -1 };
std::atomic<int>      scMuteLane_      { -1 };
// Chỉ số mẫu sweep, audio thread SỞ HỮU, controller chỉ đọc.
std::atomic<int64_t>  scSampleIndex_   { 0 };
// Tham số bất biến trong một lần quét, đặt TRƯỚC khi scOutChannel_ != -1.
std::atomic<float>    scPeak_          { 0.0f };   // đã kẹp <= kSoundcheckMaxPeak
```

cộng **một** ring thu duy nhất (chỉ một ngõ ra được đo tại một thời điểm — Q4,
nên không cần 16 ring):

```cpp
static constexpr size_t kCaptureCapacity = 65536;   // 1,37 s @ 48 kHz, luỹ thừa 2
LockFreeRingBuffer<float>   micCapture_ { kCaptureCapacity };
std::atomic<std::uint64_t>  micCaptureDrops_ { 0 };
```

Bốn điểm chèn, theo đúng thứ tự chúng xuất hiện trong
`audioDeviceIOCallbackWithContext`:

1. **Bắt mic thô — trong vòng dựng bảng làn,
   `src/app/AudioEngine.cpp:541-562`.** Con trỏ `in` cho làn được đo đã có sẵn
   ở `:550-551`. Lưu nó vào một biến stack `capSource` khi `(slot, lane)` khớp
   với cặp đang đo. Đây là mic **thô, trước mọi DSP** (Q13) — đúng thứ phép đo
   cần, và không đụng gì tới `tapSource`.
2. **Tắt đóng góp của làn đang đo (Q15) — cùng vòng đó,
   `src/app/AudioEngine.cpp:558-561`.** Khi `(slot, lane)` khớp
   `scMuteSlot_/scMuteLane_`, **không** đẩy `LaneRef` vào bảng và **không** đặt
   `tapSource`. Ngõ ra vẫn được xoá trắng ở `:570-577` như mọi ngõ ra khác, nên
   không có mẫu rác nào.
3. **Tiêm sweep — giữa `:624` (hết khối DSP) và `:631` (kẹp cuối đường).** Đây
   là vị trí **bắt buộc**: đặt sau kẹp thì sweep tới driver mà không được kẹp
   (Q13 PA 3, bị loại vì an toàn). Khối chỉ ghi vào **đúng một** kênh
   `scOutChannel_`, cộng dồn (`out[n] += ...`) như mọi nguồn khác, và mỗi mẫu
   lấy từ `SoundcheckSignal::sampleAt(scSampleIndex_ + n, ...)`.
   `scSampleIndex_` được cộng thêm `numSamples` ở cuối khối.
4. **Treo ghi tap trong suốt lần chạy — vòng `src/app/AudioEngine.cpp:657-683`.**
   Khi `scOutChannel_ != -1`, bỏ qua **toàn bộ** vòng ghi tap. Ba lý do, theo
   thứ tự quan trọng: (a) tap được ghi từ `out` **sau** kẹp, tức là **sau** điểm
   tiêm, nên nếu không treo thì tap của mọi slot trỏ vào kênh đó sẽ chứa chính
   cái sweep; (b) bỏ qua khác với drop — `micCaptureDrops_`/`tapDropCounts_`
   không tăng, không có false positive nào cho người đọc log; (c) tap im khiến
   `kTapSilenceTimeoutMs` (250 ms, `src/app/NotchController.h:83`) **đóng băng
   `liveMs_` của mọi controller** theo đúng owner decision D-06 — nghĩa là thang
   nhả của lane G **không chạy** trong lúc đo. Đây là hệ quả tốt, và nó phải
   được test khẳng định chứ không phải được hy vọng.
   Ghi mic thô vào `micCapture_` từ `capSource` thay thế vòng đó trong lúc chạy.

Không điểm nào trong bốn điểm trên cấp phát, khoá, hay log.

### 4.2 Bộ sinh tín hiệu (`SoundcheckSignal`)

Log sweep, dạng đóng, hàm thuần của chỉ số mẫu `n` (Q1):

```
T   = kSweepSeconds * sampleRate          (số mẫu của phần quét)
K   = ln(f1 / f0)
f(t)   = f0 * exp(K * t / T_sec)
phi(n) = 2*pi * f0 * T_sec / K * (exp(K * n / T) - 1)
x(n)   = peak * w(n) * sin(phi(n))
```

`w(n)` là cửa raised-cosine 30 ms hai đầu, `w(0) = 0` và `w(T-1) → 0`, nên mẫu
đầu và mẫu cuối **bằng đúng 0** — không click. Ngoài `[0, T)` hàm trả 0.0f.
`peak` đã được kẹp `<= kSoundcheckMaxPeak` **ở chỗ đặt**, và kẹp lại lần nữa ở
chỗ dùng: một giá trị có hai đường trở nên sai thì một kẹp là nửa cái kẹp (bài
học lane G, mục A.11).

Vì là hàm thuần, `LoopGainEstimator` sinh lại **đúng cùng dãy mẫu** trên
thread của nó để tính `X` — không cần chia sẻ buffer với audio thread, và test
headless khẳng định được từng mẫu.

### 4.3 Máy trạng thái (`SoundcheckController`)

Chạy trên `ClockSource` tiêm được (`src/dsp/ClockSource.h:8-13`), nên test
headless lái được toàn bộ bằng đồng hồ giả.

```
Idle
 └─(yêu cầu chạy)→ Preflight        kiểm mọi điều kiện từ chối (Q3c). Fail → Refused
      └─→ Confirm                   hộp thoại "HẠ MASTER TRƯỚC" + tổng thời lượng
           └─(người bấm OK)→ Arm    tắt detection MỌI slot; ghi nhớ mode; log soundcheck_start
                └─→ NoiseFloor      500 ms im, đo nền nhiễu của mic làn hiện tại
                     └─→ Sweep      3,0 s phát + thu
                          └─→ Tail  0,7 s im + thu
                               └─→ Analyse    tính H, nhặt ứng viên; log soundcheck_output
                                    ├─(còn ngõ ra)→ Gap 300 ms → NoiseFloor
                                    └─(hết)→ Results   log soundcheck_result
Results  (detection VẪN TẮT — xem §4.6)
 ├─(ÁP DỤNG)→ Apply → log soundcheck_apply → Restore
 ├─(BỎ)     → Restore
 └─(hết 60 s)→ Restore
Restore  trả mode + detection bằng applyModeGating; scOutChannel_ = -1; → Idle

Abort  ← từ BẤT KỲ trạng thái phát nào: ramp-out 30 ms → log soundcheck_abort → Restore
```

`Abort` vào được từ: nút `DỪNG`, bất kỳ phím nào, `engine.isRunning()` hoá false,
`getLastDeviceError()` không rỗng, mic vượt ngưỡng, `ringRiskScore ≥
ringRiskThreshold`, `micCaptureDrops_` tăng. **Đính chính Q3:** sổ quyết định
viết "`getTapDropCount` tăng"; điều đó không dùng được, vì §4.1 điểm 4 treo hẳn
việc ghi tap nên bộ đếm đó **không thể** tăng trong lúc quét. Điều kiện đúng là
`micCaptureDrops_` — bộ đếm của chính ring thu lane M. Phản biện xác nhận giúp.

Trạng thái nghỉ (`Idle`, `Refused`, `Results`) đều phát 0.0f: `scOutChannel_`
chỉ khác −1 trong `Sweep`, `Tail`, và phần ramp-out của `Abort`.

### 4.4 Toán của phép đo loop gain (`LoopGainEstimator`)

Dùng **đúng** FFT của detector — 2048 điểm, hop 512, cửa Hann
(`src/dsp/Detector.h:66-68`) — để notch phòng ngừa rơi vào **đúng bin** mà
detector sau này sẽ gia cố (Q5).

Với mỗi ngõ ra:

1. **Nền nhiễu.** Trong `NoiseFloor`, cộng dồn `N[k] = Σ_f |mic_f[k]|²` trên
   500 ms, rồi chia cho số frame → năng lượng nền trung bình mỗi bin.
2. **Năng lượng phát.** Sinh lại sweep bằng `SoundcheckSignal` và chạy cùng
   chuỗi FFT → `EX[k] = Σ_f |X_f[k]|²` trên toàn phần quét.
3. **Năng lượng thu.** Trong `Sweep` + `Tail`, `EY[k] = Σ_f |Y_f[k]|²` trên
   **toàn bộ** hai pha, kể cả đuôi.
4. **Loop gain.**
   `H_dB[k] = 10 * log10( max(EY[k] − N[k]*frames_Y, eps) / max(EX[k], eps) )`
   Trừ nền trước khi chia là điều duy nhất giữ cho bin yên tĩnh không bị thổi
   lên thành "gần hú" trong phòng ồn.

**Vì sao tổng trên toàn lần quét thì không cần bù trễ.** Phép đo này là **tỉ số
năng lượng theo bin**, không phải tương quan theo thời gian. Năng lượng sweep
gửi vào bin `k` không phụ thuộc nó tới lúc nào; năng lượng mic nhận ở bin `k`
cũng vậy, miễn cửa sổ thu bao trọn cả phần quét lẫn phần ngân. Vì thế round-trip
delay (ra, qua phòng, về) **rơi ra khỏi phép tính**, và lane M không cần đo cái
mà lane G từng thiếu. Đổi lại: lane M **cũng không đo được** delay đó, nên lane
A vẫn phải tự lo phần của mình (§6).

**`H_dB` nghĩa là gì.** `X` là dBFS **ở đầu ra app**, `Y` là dBFS **ở đầu vào
app**. Nên `H` là hàm truyền của **toàn bộ phần vật lý** của vòng: DAC → amp →
loa → phòng → mic → preamp → ADC. Vòng đóng lại qua chính app, và app **là đơn
vị** ở mọi tần số không có notch (chain chỉ có notch; ngoài ra là đường thẳng —
`src/app/AudioEngine.cpp:592-624`). Suy ra một phát biểu sạch và là nền của toàn
lane:

> **Hú xảy ra ở bin nào có `H_dB[k] ≥ 0`. Margin của bin đó là `−H_dB[k]` dB.**

Ba giới hạn của phát biểu này, phải in ra cho người vận hành đọc:

- Nó đúng **tại mức gain lúc đo**. Ai vặn master lên 6 dB sau soundcheck thì mọi
  margin tụt đúng 6 dB.
- Nó đúng nếu ngõ ra đo được nối tới loa mà mic đo được nghe thấy. Một bàn
  routing sai làm phép đo vô nghĩa mà không có gì báo.
- Nó bỏ qua vòng chéo làn (§2 phi mục tiêu).

**Nhặt ứng viên** (Q5, Q8, Q13, Q14):

1. Loại mọi bin ngoài `[100 Hz, 10 kHz]`.
2. Loại bin **không tin cậy**: `EY[k]` không vượt `N[k]*frames_Y` quá 6 dB.
3. Loại bin **đã có notch sống trong ±1 bin** — đọc từ `copySnapshot()`
   (`src/app/NotchController.h:447`), trường `notches[].frequency`. Không lọc
   bước này thì notch chồng notch, vì `H` đo mic **thô** nên không thấy các notch
   đang chạy (Q13).
4. Làm trơn `H_dB` bằng trung bình trượt 1/3 octave → `Hs_dB`.
5. Ứng viên = cực đại cục bộ có `H_dB[k] ≥ −6 dB` **và**
   `H_dB[k] − Hs_dB[k] ≥ 6 dB`. Điều kiện thứ hai là thứ tách **mode phòng**
   khỏi **đáp tuyến loa**: bỏ nó đi thì lane M sẽ đề nghị notch cái horn.
6. Sắp theo `H_dB` giảm dần, lấy tối đa **6** mỗi làn (Q14) — chừa 10 ô trong 16
   cho detector lúc chạy show.

**Độ sâu đề xuất** (Q7): kéo margin về 6 dB, rồi lượng tử lên bậc thang lane G:

```
needed_dB   = -(H_dB[k] + 6)                       // dương khi thật sự cần cắt
depth_raw   = -needed_dB                            // âm
depth       = bậc NÔNG NHẤT của kDepthLadderDb mà <= depth_raw
depth       = max(depth, ceiling)                   // trần = slider preset
depth       = max(depth, kMaxDepthDb)               // -24, invariant 1 của lane G
```

`Q` = `getNotchQ()` (`src/app/NotchController.h:362`, mặc định 30).

### 4.5 Kết quả một lần chạy

```cpp
struct OutputResult
{
    int   slot = 0, lane = 0, outChannel = 0, inChannel = 0;
    bool  measured = false;            // false = "không đo được" (Q8)
    float snrDb = 0.0f;                // toàn băng, so với nền
    std::array<float, Detector::kNumBins> marginDb {};   // = -H_dB
    std::array<bool,  Detector::kNumBins> trusted {};
    int   candidateCount = 0;
    struct Candidate { float hz; float marginDb; float depthDb; float q; int bin; };
    std::array<Candidate, 6> candidates {};
};
```

`measured == false` là **kết quả hợp lệ**, không phải lỗi: hiển thị "không đo
được", log `soundcheck_output` với `ok: false`, và **không đặt gì** (Q8).

### 4.6 Giao diện với `NotchController` — chỉ API có sẵn

Khi người vận hành bấm `ÁP DỤNG`, với từng ứng viên:

```cpp
controller.setNotch (lane, index, cand.hz, cand.q, cand.depthDb,
                     NotchController::Origin::Soundcheck);
```

Không có hàm mới nào trên `NotchController`. Bốn điều phải làm đúng:

**a) Chọn `index` mà không va chạm.** `firstFreeIndexLocked`
(`src/app/NotchController.cpp:1064`) là private, nên lane M đọc
`copySnapshot()` (`src/app/NotchController.h:447`) và chọn các `index` không
xuất hiện trong `notches[0..notchCount)` cho làn đó. Cuộc đua "detector chiếm
mất ô giữa hai bước" **không tồn tại**, và đó là lý do trạng thái `Results` giữ
detection **tắt** cho tới khi người vận hành bấm: detector không chạy thì không
ai chiếm ô. Nếu bỏ tính chất này thì `setNotchImpl` sẽ **ghi đè im lặng** một
notch đang sống (`src/app/NotchController.cpp:195`).

**b) Dọn lần chạy trước.** Notch `Origin::Soundcheck` theo KD-7 **không bao giờ
tự nhả** (`src/app/NotchController.cpp:727-729`). Nên trước khi áp dụng, mọi
notch `Origin::Soundcheck` của slot đó phải bị `clearNotch(...)`, không thì hai
lần soundcheck chồng lên nhau và ô notch cạn dần. `ClearReason` hôm nay không có
giá trị nào hợp nghĩa; v1 dùng mặc định `Manual` và §6 ghi rõ rằng việc thêm
`ClearReason::SoundcheckReplace` là một thay đổi **cộng thêm** vào enum của lane
D, để `tools/logstats.py` phân biệt được. Không tự ý thêm trong lane M.

**c) Không đụng "phòng nhớ" của lane G.** Lane G đã có cổng không cho Soundcheck
tiêu ký ức phòng (memory note lane G, mục A.13). Lane M **không ghi** và **không
tiêu** ký ức; nó không được biết tới `takeRememberedDepthLocked`.

**d) Trần.** `setNotchImpl` đặt `ceilingDb = depth` cho mọi origin khác
`Detector`. Nghĩa là notch phòng ngừa **tự làm trần của chính nó** — đúng ý,
nhưng phải test khẳng định, vì nó là hành vi ngầm của một nhánh ternary chứ
không phải một dòng code lane M viết ra.

### 4.7 Log (lane D)

Năm sự kiện, khoá dispatch là **`ev`** — không phải `kind`. Lane G đã trả giá
cho bài học này: một ternary hai nhánh biến mọi `Retune` thành `notch_clear` và
làm hỏng toàn bộ `tools/logstats.py` suốt 5 task (memory note lane G, mục A.7).

| `ev` | Khi | Trường |
|---|---|---|
| `soundcheck_start` | vào `Arm` | `outputs` (số ngõ ra), `peak_dbfs`, `sweep_ms`, `mode_before` |
| `soundcheck_output` | hết `Analyse` mỗi ngõ ra | `slot`, `lane`, `out_ch`, `in_ch`, `ok`, `snr_db`, `candidates[]` (`hz`, `margin_db`, `depth_db`) |
| `soundcheck_result` | vào `Results` | `outputs_ok`, `outputs_failed`, `candidates_total` |
| `soundcheck_apply` | sau khi đặt xong | `placed`, `cleared_previous`, `refused` |
| `soundcheck_abort` | vào `Abort` | `reason` (`user_stop`/`key`/`engine_stopped`/`device_error`/`mic_hot`/`ring_risk`/`capture_drop`), `at_output`, `elapsed_ms` |

Số thực đi vào `juce::var` phải được làm tròn **3 chữ số có nghĩa** trước, không
thì `juce::JSON` in ra 18 chữ số (bài học lane D). `logstats.py` phải bỏ qua
năm `ev` này mà không sinh cảnh báo — §5 có test cho điều đó.

### 4.8 Preset (Q9)

Định dạng **không đổi**. Notch phòng ngừa là notch bình thường trong model, nên
`savePreset` đã ghi chúng như mọi notch khác (kèm `deepestDb`, lane S/G). Hệ quả
phải ghi vào `docs/GIOI-THIEU.md`:

> Preset lưu sau soundcheck **có** mang notch phòng ngừa, nhưng khi nạp lại
> chúng vào model với `Origin::Preset`, nên chúng **sẽ tự nhả sau 30 s yên
> tĩnh** như mọi notch preset khác. Muốn bảo vệ phòng ngừa quay lại đầy đủ thì
> chạy lại ĐO.

Đường cong đo được **không** vào preset. Nó sống trong phiên (analyser vẽ) và
trong log JSONL.

### 4.9 GUI (Q10)

Tối thiểu, và mọi thứ ăn theo cái đã có:

- **Nút `ĐO`** cạnh `SOUNDCHECK` trong `gui::ModeRail` (`src/gui/ModeRail.h:81`
  là nút đang có). Nút mới, lambda mới `onMeasure`, cùng kiểu với `onSoundcheck`
  (`src/gui/ModeRail.h:45-48`).
- **Trong lúc đo**: overlay mờ trên `SpectrumView` với `ĐANG ĐO · ngõ ra 2/4`,
  đồng hồ (tái dùng `countdownLabel` qua `getSoundcheckRemainingMs`,
  `src/app/MainComponent.cpp:247-251`), và một nút `DỪNG` to. Mọi phím cũng dừng.
- **Sau khi đo**: một đường `marginDb` vẽ chồng lên phổ của làn đang hiển thị
  (`setDisplayLane`, `src/gui/SpectrumView.h:202`), marker tại ứng viên, và một
  dải kết quả một dòng: **"tìm thấy N điểm dễ hú"** + `ÁP DỤNG` / `BỎ`. Số dB
  chỉ hiện khi rê chuột.
- Ngõ ra "không đo được" hiện bằng chữ, không bằng đường phẳng 0 dB — một đường
  phẳng trông như "phòng rất tốt".

Ràng buộc để lane U (Dumb mode) sau này không phải viết lại: **chữ chính là câu
người, không phải số.**

Theo CLAUDE.md, mọi task đổi hình phải kết thúc bằng ảnh render
`build/tools/Release/HandsFreeSnapshot.exe` và **đọc lại ảnh** trước khi gửi.

### 4.10 Hằng số (một chỗ, `SoundcheckController.h`)

| Hằng | Giá trị | Q |
|---|---|---|
| `kSweepLowHz` | 100.0 | Q1/Q14 |
| `kSweepHighHz` | 10000.0 | Q1/Q14 |
| `kSweepSeconds` | 3.0 | Q1 |
| `kTailSeconds` | 0.7 | Q14 |
| `kRampMs` | 30.0 | Q2 |
| `kGapMs` | 300.0 | Q14 |
| `kNoiseFloorMs` | 500.0 | Q8 |
| `kSoundcheckMaxPeak` | 0.1f (−20 dBFS) | Q2 — **kẹp cứng, không lên GUI** |
| `kSoundcheckMinPeak` | 0.01f (−40 dBFS) | Q2 |
| `kMicAbortDbfs` | −6.0 | Q3 |
| `kMicAbortHoldMs` | 20.0 | Q3 |
| `kMinBandSnrDb` | 12.0 | Q8 |
| `kMinBinSnrDb` | 6.0 | Q8 |
| `kCandidateMarginDb` | −6.0 | Q14 |
| `kMinProminenceDb` | 6.0 | Q14 |
| `kTargetMarginDb` | 6.0 | Q14 |
| `kMaxPreventivePerLane` | 6 | Q14 |
| `kResultsTimeoutMs` | 60000.0 | §4.3 |

Như lane G: cố định cho 1.3.0, **không lên GUI**. Một slider trên bất kỳ số nào
ở đây biến mọi báo cáo lỗi thành "lúc đó nó đang ở giá trị nào?".

### 4.11 Invariant an toàn (mỗi dòng là một phát biểu test được)

1. Mẫu sweep nào rời `SoundcheckSignal` cũng có `|x| <= kSoundcheckMaxPeak`, với
   **mọi** giá trị `peak` truyền vào, kể cả âm, NaN, hay lớn hơn 1.
2. `x(0) == 0.0f` và mẫu cuối của cửa `== 0.0f`: không click ở hai đầu.
3. Ngoài `[0, T)` bộ sinh trả đúng `0.0f`.
4. Điểm tiêm nằm **trước** kẹp `±kMaxOutputLevel`
   (`src/app/AudioEngine.cpp:631-647`); không mẫu sweep nào tới driver mà chưa
   qua kẹp đó.
5. Sweep chỉ bao giờ được ghi vào **đúng một** kênh đầu ra: kênh
   `scOutChannel_`. Mọi kênh khác nhận thêm đúng 0.
6. `scOutChannel_ == -1` ⇒ callback không ghi mẫu sweep nào và không ghi
   `micCapture_`. Đây là trạng thái mặc định lúc khởi tạo.
7. Trong suốt một lần chạy, detection **tắt** trên mọi slot; không `NotchCommand`
   nào phát sinh từ detector.
8. Trong suốt một lần chạy, **không tap nào được ghi**, nên `liveMs_` của mọi
   `NotchController` đứng yên (D-06, `kTapSilenceTimeoutMs`) — thang nhả lane G
   không tiến một bậc nào vì lý do soundcheck.
9. `Abort` từ bất kỳ đâu ⇒ biên độ về 0 qua ramp ≤ 30 ms, và trong ≤ 1 block sau
   đó `scOutChannel_ == -1`, `scMuteSlot_ == -1`.
10. Không notch nào được đặt trừ khi người vận hành bấm `ÁP DỤNG` (Q6).
11. Mọi độ sâu đề xuất nằm trong `[kMaxDepthDb, 0]` và là một bậc của
    `kDepthLadderDb` hoặc chính trần preset (quy tắc Q13 của lane G).
12. Không notch phòng ngừa nào được đặt trên bin đã có notch sống trong ±1 bin.
13. Không ghi và không tiêu "phòng nhớ" của lane G.
14. `Preflight` thất bại ⇒ **không** mẫu nào được phát, và trạng thái trở lại
    `Idle` (không phải một trạng thái nửa vời phát im lặng).
15. Không có đường nào đặt notch sâu hơn `kMaxDepthDb` — kể cả khi `H_dB` đo
    được là +30 dB (routing sai, mic chạm loa).

## 5. Kiểm thử

### 5.1 Headless (`ctest`, thêm vào suite 547)

**`test_soundchecksignal`** (mới)
- `PeakIsClampedWhateverIsAsked`: `peak` = 1.0, 10.0, −5.0, NaN → mọi mẫu
  `|x| <= 0.1f + 1e-6`. (Invariant 1)
- `FirstAndLastSampleAreExactlyZero`. (Invariant 2)
- `OutsideTheSweepReturnsZero`. (Invariant 3)
- `InstantaneousFrequencyIsMonotoneAndHitsBothEnds`: đếm zero-crossing trên các
  cửa 50 ms → tần số tăng đơn điệu, cửa đầu ≈ 100 Hz ±5 %, cửa cuối ≈ 10 kHz
  ±5 %.
- `RampIsMonotoneOverThirtyMilliseconds`: đường bao (đỉnh mỗi 1 ms) không giảm
  trên 30 ms đầu.

**`test_loopgainestimator`** (mới) — **phòng tổng hợp**, không thiết bị:
- `FlatRoomMeasuresFlatResponse`: "phòng" = trễ 15 ms + gain 0,5 → `H_dB` phẳng
  ở −6 dB ±1,0 dB trên `[200 Hz, 8 kHz]`.
- `ResonanceLandsInTheRightBin`: phòng = trễ + một peaking biquad +12 dB @
  1 kHz, Q 20 → đỉnh của `H_dB` nằm trong **±1 bin** của 1 kHz, biên độ trong
  **±1,5 dB** của giá trị đúng.
- `DelayDoesNotChangeTheAnswer`: cùng phòng, trễ 5 ms và 200 ms → hai `H_dB`
  lệch nhau ≤ 0,5 dB ở mọi bin. Đây là test **chứng minh** lập luận "tổng năng
  lượng thì không cần bù trễ" ở §4.4 — nếu nó đỏ thì lập luận sai, không phải
  test sai.
- `NoiseFloorIsSubtracted`: cùng phòng, thêm nhiễu trắng −50 dBFS → `H_dB` ở các
  bin ngoài dải quét không vượt `kCandidateMarginDb`.
- `BinBelowSnrIsMarkedUntrusted` và `UntrustedBinIsNeverACandidate`.

**`test_soundcheck_candidates`** (mới)
- `SpeakerRolloffIsNotACandidate`: `H_dB` dốc trơn (không có mode) → 0 ứng viên,
  dù `H_dB` vượt −6 dB ở cả một dải rộng. Đây là test quan trọng nhất của khối
  này: bỏ quy tắc prominence thì test này đỏ.
- `DepthQuantisesOntoTheLadder`: margin −2 dB → cần 8 dB → bậc −12 (không phải
  −6, không phải −8).
- `CeilingClampsTheProposal`: trần preset −10 → đề xuất không bao giờ sâu hơn
  −10, và bậc cuối **là chính trần** (quy tắc Q13 lane G).
- `NothingDeeperThanMinusTwentyFour`: `H_dB` = +30 dB → đề xuất −24, không phải
  −36. (Invariant 15)
- `BinWithALiveNotchIsSkipped` (±1 bin). (Invariant 12)
- `AtMostSixPerLane`. (Invariant 6 của §4.4 bước 6)

**`test_soundcheckcontroller`** (mới) — đồng hồ giả (`ClockSource`):
- `RefusesWhenEngineNotRunning`, `RefusesWithZeroChannels`,
  `RefusesWhenSlotDisabled`, `RefusesWhenRingRiskIsRising` — mỗi cái khẳng định
  **không mẫu nào được phát** và trạng thái về `Idle`. (Invariant 14)
- `AbortRampsDownAndClearsTheChannel`: khẳng định biên độ giảm đơn điệu về 0
  trong ≤ 30 ms **và** `scOutChannel_ == -1` sau đó. (Invariant 9)
- `NothingIsPlacedWithoutApply`: chạy trọn vẹn, không bấm → `copySnapshot`
  không có notch nào thêm. (Invariant 10)
- `ApplyPlacesThroughSetNotchWithSoundcheckOrigin` + `PreviousRunIsClearedFirst`.
- `DetectionIsOffForTheWholeRunAndRestoredAfter` — khẳng định **từng frame**,
  không chỉ ở cuối: bão hoà che độ trễ (bài học lane G, mục A.9).
- `SequencesOneOutputAtATime`: với 2 slot stereo, đúng 4 lần quét, không lúc nào
  có 2 kênh cùng mang sweep. (Invariant 5)

**`test_audioengine`** (bổ sung)
- `SweepGoesThroughTheOutputClamp`: `peak` bị ép (qua seam test) lên 5.0 → mẫu
  ra vẫn `|v| <= 1.0f`. (Invariant 4)
- `SweepTouchesOnlyTheMeasuredChannel`. (Invariant 5)
- `TapsAreSuspendedDuringARun` + `TapDropCountDoesNotMoveDuringARun` — bỏ qua
  khác với drop. (Invariant 8, §4.1 điểm 4)
- `MutedLaneContributesNothing` (Q15) và `MutedLaneIsNotTapped`.
- `IdleEngineEmitsNoSweep`. (Invariant 6)

**`test_sessionlogger`** (bổ sung) — hình dạng năm `ev` mới, và
`logstats.py` không cảnh báo khi gặp chúng.

### 5.2 Chỉ làm được trên rig (không claim trong ctest)

Danh sách này đi thẳng vào TESTER-NOTES của bản alpha:

1. Sweep ở −20 dBFS trong phòng thật có **nghe được** không, và **to cỡ nào** so
   với chương trình. Đo bằng máy đo SPL, không bằng cảm giác.
2. Đường margin có khớp một phép đo tham chiếu (REW / Smaart + mic đo) không?
   Sai **bao nhiêu dB** và **lệch mấy bin**?
3. Notch phòng ngừa có thật sự **nâng gain-before-feedback** không? Đo: vặn
   master lên từng dB cho tới khi hú, trước và sau khi `ÁP DỤNG`. Chênh lệch là
   con số duy nhất chứng minh lane M có giá trị.
4. Dừng khẩn: bấm `DỪNG` và bấm phím giữa sweep — bao lâu thì im, có click
   không?
5. Trong phòng ồn (quạt, khán giả) thì bao nhiêu ngõ ra ra "không đo được"?
6. Ứng viên lane M có trùng với các bin mà detector **thật sự** notch trong show
   sau đó không? Đây là dữ liệu đối chiếu cho lane C.

## 6. Cần gì từ lane G / R / S / D

| Lane | Cần gì | Đã có chưa |
|---|---|---|
| **S** | Per-lane = per-output: tap và notch theo làn, `width`, `linked` | **Đủ.** Đã hạ cánh, `src/app/SlotConfig.h:8-9`, `NotchController::setWidth` |
| **G** | Thang `kDepthLadderDb`, `kMaxDepthDb`, `getNotchQ()`, trần preset, quy tắc lượng tử Q13 | **Đủ.** `src/app/NotchController.h:100-114`, `:362` |
| **G** | Cổng không cho Soundcheck tiêu "phòng nhớ" | **Đủ** (memory note lane G, mục A.13). Lane M chỉ cần **không** đụng vào |
| **R** | `ringRiskScore/Valid/Threshold` để từ chối chạy khi phòng đã gần hú | **Đủ.** `src/app/NotchController.h:426-436` |
| **D** | `SessionLogger::makeEvent` + đường `ev` | **Đủ.** `src/app/SessionLogger.h:49-50` |
| **D** | `ClearReason::SoundcheckReplace` để logstats phân biệt lần dọn của soundcheck với một Clear do người bấm | **CHƯA.** Thay đổi **cộng thêm** vào enum `src/app/NotchController.h:67-70` + một nhánh trong `logstats.py`. v1 dùng `Manual` và chấp nhận mờ; **không tự thêm trong lane M** — cần owner gật |
| **A** | Round-trip delay của vòng hú | **Lane M KHÔNG cung cấp.** §4.4 giải thích: tỉ số năng lượng làm delay rơi ra khỏi phép tính. Lane A phải tự đo, hoặc lane M phải mở thêm nhánh giải chập (Q5 PA 3) |

**Không giẫm chân ai:** lane M không sửa `Detector`, `CandidateScorer`,
`PeakinessAnalyzer`, `Biquad`, `NotchChain`, hay bất kỳ nhánh nào của thang lane
G. Nó **thêm** vào `AudioEngine` (bốn điểm §4.1), **thêm** hai lớp `src/dsp/`,
**thêm** một lớp `src/app/`, **thêm** một nút và một overlay.

## 7. Rủi ro

1. **Phép đo đúng nhưng vô nghĩa vì routing sai.** Ngõ ra đo được không nối tới
   loa mà mic đo được nghe thấy → `H` là tiếng vọng từ loa khác, và lane M vẫn
   đưa ra một danh sách trông rất thuyết phục. Giảm nhẹ một phần bằng ngưỡng SNR
   (Q8), nhưng **không** phát hiện được trường hợp "loa khác cũng đủ to".
2. **Vòng chéo làn không được đo** (§2). Trong hệ stereo, hú có thể đóng qua
   out L → mic R; lane M v1 mù với đường đó và sẽ báo margin tốt hơn thực tế.
3. **Phép đo là một ảnh chụp.** Khán giả vào phòng đổi hấp thụ tới vài dB ở dải
   trên; mọi margin đo lúc phòng trống đều lạc quan. Notch `Origin::Soundcheck`
   lại **không tự nhả**, nên sai lầm lúc đo **ở lại cả buổi**.
4. **`H_dB` không phải hàm truyền đã hiệu chuẩn.** Tỉ số năng lượng qua cửa Hann
   có rò rỉ giữa bin; với mode Q rất cao nằm giữa hai bin, biên độ đo được sẽ
   thấp hơn thật. Đủ cho "bin nào nóng", không đủ cho "nóng đúng bao nhiêu dB".
5. **Ba giây rưỡi im lặng trên một ngõ ra** (Q15) là một thay đổi level không ai
   nghĩ tới lúc thiết kế; nếu ai bấm ĐO giữa show thì đó là ba giây rưỡi mất
   tiếng. Hộp thoại xác nhận có nói, nhưng người vận hành đang vội sẽ bấm qua.
6. **Ô notch cạn.** 6 notch phòng ngừa × không bao giờ nhả, cộng detector đang
   chạy, là 16 ô có thể đầy. Khi đầy, `firstFreeIndexLocked` trả −1 và detector
   **im lặng không đặt được gì**. Lane M làm khả năng đó gần hơn hẳn.
7. **Va chạm khái niệm "soundcheck".** Sau lane M, trong code có hai thứ tên
   soundcheck: mode thụ động 15 s và hành động đo chủ động. `Origin::Soundcheck`
   dùng chung cho cả hai, nên log và preset **không phân biệt được** một notch
   phòng ngừa với một notch bắt được trong cửa sổ 15 s. Sống được, nhưng phải
   biết.
8. **Hằng số chưa ai đo trên rig này.** Toàn bộ Q14 là phán đoán. Lần chạy thật
   đầu tiên gần như chắc chắn sẽ đổi ít nhất `kMinProminenceDb` và
   `kCandidateMarginDb`.

## 8. Phản biện

*(để trống — một reviewer read-only độc lập điền phần này. Theo quy trình của
lane S/D/G: agent read-only, model opus, không có `Edit`/`Write`, đọc file thật
chứ không đọc spec này như một bản báo cáo. Hấp thụ BLOCKER xong mới sang plan.
Ba chỗ đáng bắn trước: điểm chèn §4.1 điểm 3-4 và hệ quả của việc treo tap; lập
luận "không cần bù trễ" ở §4.4; và cách chọn `index` ở §4.6(a) — nó dựa vào
việc detection đang tắt ở trạng thái `Results`.)*
