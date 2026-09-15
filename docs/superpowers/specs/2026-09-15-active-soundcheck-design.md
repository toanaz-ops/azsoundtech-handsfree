# Lane M — Soundcheck đo chủ động: quét sweep từng ngõ ra, đo loop gain, đề xuất notch phòng ngừa

**Ngày:** 2026-09-15. **Roadmap:** [`2026-09-04-anti-feedback-v2-roadmap.md`](2026-09-04-anti-feedback-v2-roadmap.md) (lane M, cần S — S đã hạ cánh; main `a6be099`, 1.2.0 alpha, suite 547/547).
**Sổ quyết định:** [`../decisions/2026-09-15-lane-m-active-soundcheck.md`](../decisions/2026-09-15-lane-m-active-soundcheck.md) (Q1–Q17).
**Trạng thái:** **rev 2, 2026-09-15** — sau phản biện read-only độc lập vòng 1 (8 BLOCKER / 11 IMPORTANT / 8 MINOR). Tất cả BLOCKER và IMPORTANT đã hấp thụ; bảng đối chiếu từng finding ở §8. **CHƯA owner duyệt.** Rev 1 là bản trước phản biện; nó sai ở tám chỗ và không được dùng làm cơ sở cho plan.
**Đụng audio path:** **có, và nặng nhất từ trước tới nay** — lần đầu app **tự sinh tín hiệu và phát ra PA**. **Release:** 1.3.0 (`-Part minor`), không gộp với lane nào khác.

> **Owner phải xác nhận tám mục trước khi có một dòng code:** Q2 (mức phát, với
> cách diễn đạt trung thực ở §3) · Q15-lật-lại (im **cả kênh ngõ ra**, tổng
> ~72 s xấu nhất) · Q6 (đề xuất hay tự đặt — prompt gốc của owner viết "đặt") ·
> Q3-lật-lại (bộ điều kiện tự hủy còn lại + độ trễ dừng xấu nhất) ·
> Q16 (nút `ĐO` riêng hay tái dùng `SOUNDCHECK`) · Q7 + Q9 (bất đối xứng khi nạp
> lại preset) · `kResultsTimeoutMs` · `ClearReason::SoundcheckReplace`.

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
- Lane A (AFC) **không viết được** trước lane M: roadmap ghi thẳng "A cần M (đo
  loa→mic)".

**Sáu sự thật kỹ thuật định hình toàn bộ thiết kế dưới đây.** Cả sáu đã mở file
kiểm chứng, và bốn trong số đó là thứ phản biện vòng 1 dùng để bác rev 1:

1. **Tap mà detector đọc là OUTPUT chứ không phải mic.** Callback gán
   `tapSource[slot][lane] = out` (`src/app/AudioEngine.cpp:561`) rồi ghi ring từ
   con trỏ đó (`src/app/AudioEngine.cpp:657-666`). Bất kỳ tín hiệu nào lane M
   trộn vào `out` sẽ quay lại chính đường detector.
2. **Không có limiter trong app.** Đường ra chỉ có một **kẹp cứng** ±1.0f
   (`kMaxOutputLevel`, `src/app/AudioEngine.cpp:15`, áp ở `:631-647`) — đó là
   clipper toàn thang. Prompt gốc của owner viết "limiter vẫn nằm trong đường";
   sự thật là **không có**.
3. **Nhiều slot cộng dồn lên CÙNG một kênh ngõ ra.** Callback xoá trắng mọi kênh
   ra trước (`src/app/AudioEngine.cpp:565-577`) đúng để DSP **cộng dồn**
   `out[n] += v` (`src/app/AudioEngine.cpp:621`). Vì thế "tắt một (slot, lane)"
   **không** mở được vòng hú của kênh đó — rev 1 sai ở đúng chỗ này (§3, §4.1).
4. **`setNotch` ghi đè im lặng.** `setNotchImpl`
   (`src/app/NotchController.cpp:195`) không kiểm `n.active` trước khi ghi model
   (`src/app/NotchController.cpp:226-245`), và với origin khác `Detector` nó đặt
   luôn `ceilingDb = depth` (`:243-245`). `firstFreeIndexLocked`
   (`src/app/NotchController.cpp:1064`) là **private**.
5. **`SnapshotNotch` không mang `origin`** (`src/app/NotchController.h:394-406`:
   `frequency, Q, depthDB, deepestDb, channel, index` — hết), và `model_` là
   private. Nên **không có đường nào** để người ngoài biết notch nào là
   `Origin::Soundcheck`. Đây là lý do §4.6 phải thêm **đúng một trường**.
6. **Publish snapshot nằm TRONG vòng drain** (`src/app/NotchController.cpp:568-582`
   dựng `notchList`, `:617-650` publish). Không có block nào được drain thì
   `copySnapshot()` **không refresh**. Mọi thứ đọc ring risk hay danh sách notch
   trong lúc tap bị treo đều đang đọc một khung đông cứng.

Thêm một sự thật về GUI, vì prompt lane M mô tả sai: **không có placeholder
"sweep the room" nào trong repo.** Hai nút thật là
`gui::ModeRail::soundcheckButton {"SOUNDCHECK"}` (`src/gui/ModeRail.h:81`, đây là
nút đang hiển thị) và `gui::ModeBar::soundcheckButton {"Run Soundcheck (15s)"}`
(`src/gui/ModeBar.h:43`) — `modeBar_` còn sống nhưng **bị ẩn**
(`src/app/MainComponent.cpp:211`).

## 2. Mục tiêu và phi mục tiêu

**Mục tiêu:**

1. Một hành động "ĐO" phát **log sine sweep** qua **từng kênh ngõ ra một**, thu
   mic tương ứng, và tính **loop gain theo tần số** cho từng làn (Q1, Q4, Q5).
2. Từ đường loop gain đó, **đề xuất** notch phòng ngừa tại các bin còn ít
   margin, với độ sâu lượng tử lên **đúng thang lane G**; người vận hành bấm
   `ÁP DỤNG` thì mới có gì tới PA (Q6, Q7).
3. Mọi notch phòng ngừa đi qua **đúng một API ghi có sẵn**:
   `NotchController::setNotch(..., Origin::Soundcheck)`
   (`src/app/NotchController.h:221-223`), gọi từ **message thread**.
4. Vẽ kết quả lên analyser: đường margin + marker ứng viên (Q10).
5. Ghi năm sự kiện `soundcheck_*` vào log session lane D (Q9).
6. **Mở vòng hú của kênh ngõ ra đang đo** trong suốt lần quét (Q15 lật lại).

**Phi mục tiêu** (ghi để plan không lấn):

- **Không** tự đặt notch. v1 chỉ đề xuất (Q6).
- **Không** đo chéo làn (out L → mic R). Vòng chéo có thật và v1 mù với nó (§7).
- **Không** giải chập ra đáp ứng xung, **không** ước lượng τ/Q của mode phòng.
- **Không** pre-emphasis sweep trong v1 (F19) — hệ quả là băng tin cậy hẹp hơn
  băng quét; xem §4.4.
- **Không** đổi định dạng preset (Q9).
- **Không** đổi mode `Soundcheck` thụ động, không thêm mode thứ tư (Q12).
- **Không** đổi thuật toán dò, scorer, hay thang lane G. Lane M chỉ **đọc** các
  hằng số của lane G.
- **Không** tự tăng mức phát để cải thiện SNR (Q8).
- **Không** sửa bảng roadmap trong lane này — điều phối viên làm sau khi merge
  (F23).

## 3. Mức thay đổi level dự kiến (bắt buộc theo CLAUDE.md)

> **Lane M là thay đổi level lớn nhất của dự án tới nay: lần đầu tiên app tự
> sinh tín hiệu và phát ra PA, thay vì chỉ trừ gain khỏi tín hiệu người khác
> gửi vào. Từ `AudioEngine` trở ra, sweep đi qua đúng một kẹp cứng ±1.0f
> (`src/app/AudioEngine.cpp:631-647`) — trong app **không có limiter nào cả**.
> Mức âm thanh thật trong phòng do **fader master của người vận hành** quyết
> định, không do app.**

Bằng số, so với 1.2.0. "Kênh đang đo" = kênh ngõ ra `scOutChannel_`:

| Thời điểm | Trên KÊNH đang đo | Trên các kênh ngõ ra khác | Ghi chú |
|---|---|---|---|
| Trước khi bấm xác nhận | 0 dB | 0 dB | Không phát gì |
| Nền nhiễu (0,5 s) | **im hoàn toàn** — mọi làn định tuyến vào kênh này bị tắt | 0 dB | Đo nền trong đúng điều kiện của sweep |
| Ramp vào | −∞ → **−20 dBFS đỉnh** trong 30 ms, raised-cosine | 0 dB | Q2 |
| Sweep (3,0 s) | **chỉ có sweep**, đỉnh −20 dBFS, RMS ≈ −23 dBFS; không có tiếng mic nào | 0 dB | Xem "im cả kênh" bên dưới |
| Đuôi (0,7 s) | im | 0 dB | Đo phần ngân |
| Gap (0,3 s) | im, **các làn đã được trả lại** | 0 dB | |
| Dừng khẩn | ramp xuống −∞ trong 30 ms; các làn trả lại ở callback kế | 0 dB | Độ trễ: bảng dưới |
| Trạng thái `Results` (≤ 20 s) | 0 dB — **detection đã bật lại, thang nhả lane G chạy lại bình thường** | 0 dB | F9 |
| Sau `ÁP DỤNG` | mỗi bin ứng viên bị cắt **−6 / −12 / −18 / −24 dB**, tối đa **6 bin mỗi làn** | 0 dB | Q7 |
| Sau `BỎ` / hết giờ | 0 dB | 0 dB | Không gì được đặt |

**"Im cả kênh" — và tổng thời gian.** Q15 (lật lại sau phản biện) tắt **mọi làn
của mọi slot** có `outputChannels[lane] == scOutChannel_`, không phải một
`(slot, lane)`. Chỉ có thế mới mở được vòng, vì nhiều slot cộng dồn lên một kênh
(`src/app/AudioEngine.cpp:565-577`, `:621`). Giá phải trả:

- Kênh ngõ ra đó **im hoàn toàn 4,5 giây** (0,5 nền + 3,0 sweep + 0,7 đuôi +
  0,3 gap). Ai bấm ĐO giữa lúc MC đang nói thì kênh đó mất tiếng 4,5 giây.
- **Xấu nhất 8 slot stereo = 16 kênh ngõ ra ⇒ 16 × 4,5 s ≈ 72 giây.** Hộp thoại
  xác nhận phải in đúng con số này trước khi người vận hành bấm.

**Quy ra SPL — phát biểu trung thực duy nhất.** App không biết SPL tuyệt đối, và
không có phép quy đổi nào đúng nếu không biết gain của bàn và amp:

> **Sweep phát ở 20 dB dưới toàn thang của hệ, tại vị trí master hiện tại của
> bạn.** Nếu hệ đang chạy show ở mức bình thường, sweep nghe nhỏ hơn chương
> trình một chút. Nếu master đang mở hết, 20 dB dưới toàn thang vẫn là rất to.
> Vì thế: **hạ master trước**, và mức phát chỉ chỉnh được **xuống**.

Rev 1 viết "≈ 80 dB SPL"; con số đó dựa trên một giả định về bàn mà app không có
quyền đưa ra, và đã bị gỡ (F13).

**Không còn nguy cơ clipping trên kênh đang đo.** Vì mọi làn định tuyến vào kênh
đó đã bị tắt, kênh đó chỉ mang sweep (đỉnh 0,1). Kẹp ±1.0f không bao giờ phải
làm việc ở đây — nhưng nó vẫn nằm **sau** điểm tiêm và không được bỏ
(invariant 4).

**"Không thể hú trong lúc đo" — phát biểu đúng.** Rev 1 nói câu này và nó sai:

> Vòng hú **đi qua kênh ngõ ra đang đo** bị **mở** trong suốt lần quét: kênh đó
> không mang tiếng mic nào. Các vòng hú đi qua **kênh ngõ ra khác** vẫn **đóng**
> — loa của chúng vẫn phát tiếng mic, và mic vẫn nghe cả sweep lẫn chúng. Nếu
> phòng đang sát ngưỡng ở một kênh khác, lần quét **có thể** kích nó. Đó là lý
> do vẫn phải có bộ tự hủy (§4.3) và điều kiện từ chối chạy khi RING RISK đã ≥
> RISING.

**Độ trễ dừng xấu nhất** (từ lúc bấm `DỪNG` đến lúc biên độ sweep bằng 0, phía
app):

| Thành phần | @ buffer 64 / 48 kHz | @ buffer 1024 / 48 kHz |
|---|---|---|
| Chờ callback kế đọc cờ | ≤ 1,3 ms | ≤ 21,3 ms |
| Ramp-out raised-cosine | 30 ms | 30 ms |
| **Tổng phía app** | **≤ 31,3 ms** | **≤ 51,3 ms** |

Cộng độ trễ ra của driver và amp (`getCurrentLatency()`), thứ app không điều
khiển được. Không có đường nào cắt phựt: ramp do **chính callback** sinh (§4.2),
không phụ thuộc thread nào khác còn sống.

**Cơ chế an toàn — tất cả đều là code, không phải kỷ luật vận hành:**

| Cơ chế | Chi tiết |
|---|---|
| Kẹp biên độ tại nguồn | `SoundcheckSignal` kẹp `jlimit` vào `kSoundcheckMaxPeak = 0.1f` (−20 dBFS) ở **cả** chỗ đặt **và** chỗ dùng — một giá trị có hai đường trở nên sai thì một kẹp là nửa cái kẹp (bài học lane G A.11) |
| Kẹp cuối đường | Kẹp ±1.0f có sẵn vẫn nằm **sau** điểm tiêm (invariant 4). Không được bỏ |
| Ramp | 30 ms raised-cosine vào và ra; ramp-out do callback tự sinh (invariant 9) |
| Dừng khẩn | Nút `DỪNG` là **đường chính**; `Esc` là đường phụ, best-effort (F14) |
| Tự hủy | Mic RMS > −6 dBFS giữ > 20 ms, **tính trên thread lane M từ `micCapture_`**; peakiness của cửa sổ nền vượt `CandidateScorer::kConfirmScore`; `micCaptureDrops_` tăng; sample rate hoặc số kênh đổi; `isRunning()` hoá false; `getLastDeviceError()` khác rỗng |
| Từ chối chạy | Engine chưa `isRunning()`; cặp (in, out) của slot không hợp lệ với số kênh **thật** của callback; slot disable; RING RISK ≥ RISING (đọc lúc snapshot còn sống) |
| Không detector nào phản ứng với sweep | Detection tắt trên **mọi** slot từ `Arm` đến hết đuôi của kênh cuối (Q12) |
| Bounds check | `scOutChannel_` và kênh thu kiểm lại **mỗi callback** với số kênh của chính callback đó (invariant 3) |
| Chết an toàn | Trạng thái nghỉ phát 0.0f; `scOutChannel_ == -1` là giá trị khởi tạo |

## 4. Thiết kế

### 4.1 Kiến trúc và bốn điểm chèn THẬT trong `AudioEngine`

| Lớp | File | Thread | Việc |
|---|---|---|---|
| `SoundcheckSignal` | `src/dsp/SoundcheckSignal.h` | audio + controller | Hàm **thuần** của chỉ số mẫu → biên độ. Không state, không cấp phát |
| `LoopGainEstimator` | `src/dsp/LoopGainEstimator.h/.cpp` | thread lane M | Cộng dồn `Σ|X|²`, `Σ|Y|²`, nền nhiễu; ra `H_dB[k]`; làm trơn; nhặt ứng viên |
| `SoundcheckController` | `src/app/SoundcheckController.h/.cpp` | thread riêng, poll 5 ms | Máy trạng thái, lái `AudioEngine`, đọc `micCapture_`, tính mức/peakiness để tự hủy. **Không bao giờ gọi `NotchController`** (§4.6e) |

`AudioEngine` nhận **sáu atomic, một bộ đếm, một ring**:

```cpp
// Kênh ĐẦU RA đang đo. -1 = không chạy. Đây LÀ khoá tắt tiếng (Q15 lật lại):
// mọi làn có outputChannels[lane] == scOutChannel_ đều bị tắt.
std::atomic<int>      scOutChannel_       { -1 };
std::atomic<int>      scCaptureInChannel_ { -1 };
// Cổng thu, bật suốt NoiseFloor + Sweep + Tail (F5). Tách khỏi scOutChannel_
// để một tính chất AN TOÀN không phải suy ra từ dấu của một chỉ số.
std::atomic<bool>     scCaptureActive_    { false };
// Chỉ số mẫu sweep. ÂM trong pha NoiseFloor. Audio thread SỞ HỮU, controller đọc.
std::atomic<int64_t>  scSampleIndex_      { 0 };
// Biên độ đỉnh, ĐÃ kẹp <= kSoundcheckMaxPeak trước khi lưu.
std::atomic<float>    scPeak_             { 0.0f };
// Mốc ramp-out. -1 = không có. Đặt bởi message thread, TIÊU THỤ bởi callback (F8).
std::atomic<int64_t>  scRampOutAtSample_  { -1 };

static constexpr size_t kCaptureCapacity = 65536;
LockFreeRingBuffer<float>   micCapture_      { kCaptureCapacity };
std::atomic<std::uint64_t>  micCaptureDrops_ { 0 };
```

`kCaptureCapacity` **không** là "thời lượng cần thu" — `micCapture_` là một dòng
được thread lane M rút mỗi 5 ms, y như tap của detector. Nó là **độ trễ rút tối
đa chịu được**: 65536 mẫu = 1,37 s @ 48 kHz, **0,68 s @ 96 kHz, 0,34 s @
192 kHz** — vẫn hơn 68 lần chu kỳ rút ở rate cao nhất (F25).

**Snapshot một lần, đúng luật của nhà** (F4). Sáu atomic trên được đọc **một
lần duy nhất**, ngay cạnh `bypass` ở `src/app/AudioEngine.cpp:509-514`, vào biến
stack, và chỉ các biến stack đó được dùng cho cả callback. Lý do đã ghi sẵn ở
chính chỗ đó: *"The mode AND the mapping are snapshotted ONCE at the top of the
block"*. Rev 1 đọc chúng ở ba chỗ khác nhau; một cú lật giữa điểm 3 và điểm 4 sẽ
cho một callback **vừa tiêm sweep vừa ghi tap** — đúng kịch bản đầu độc detector
mà cả lane này dựng ra để tránh.

**Bounds check, mỗi callback** (F3). `scOutChannel_` và `scCaptureInChannel_`
kiểm lại với `numOutputChannels` / `numInputChannels` **của chính callback đó**,
giống hệt cách vòng làn đã kiểm mọi chỉ số kênh ở
`src/app/AudioEngine.cpp:546-548` và `:555-556`. Một lần restart thiết bị xuống
ít kênh hơn mà không có bước này là **ghi ngoài mảng trên thread realtime**. Chỉ
số không hợp lệ ⇒ callback coi như không chạy soundcheck; thread lane M phát
hiện qua thiếu dữ liệu / đổi số kênh và abort (§4.3).

Bốn điểm chèn, theo thứ tự trong `audioDeviceIOCallbackWithContext`:

1. **Bắt mic thô — trong vòng dựng bảng làn,
   `src/app/AudioEngine.cpp:541-562`.** Con trỏ `in` đã có ở `:550-551`. Khi
   `inIdx == scCaptureInChannel_` (đã bounds-check), lưu vào biến stack
   `capSource`. Mic **thô, trước mọi DSP** (Q13).
2. **Tắt mọi làn định tuyến vào kênh đang đo — cùng vòng,
   `src/app/AudioEngine.cpp:558-561`.** Điều kiện là **`outIdx == scOutChannel_`**,
   không phải `(slot, lane)` khớp (F2). Làn bị tắt thì **không** vào bảng
   `LaneRef` và **không** đặt `tapSource`. Kênh ra vẫn được xoá trắng ở `:570-577`
   như mọi kênh khác.
3. **Tiêm sweep + sinh ramp-out — giữa `:624` (hết khối DSP) và `:631` (kẹp cuối
   đường).** Vị trí **bắt buộc**: sau kẹp thì sweep tới driver mà không được kẹp.
   Khối ghi vào **đúng một** kênh `scOutChannel_`, cộng dồn `out[n] += ...`. Với
   mỗi mẫu `n`: nếu `scRampOutAtSample_ >= 0` thì biên độ nhân thêm nửa cửa
   raised-cosine tính từ mốc đó; khi ramp chạy hết, **chính callback** ghi
   `scOutChannel_ = -1` và `scRampOutAtSample_ = -1` (F8). Không thread nào khác
   phải còn sống để tiếng tắt. Cuối khối: `scSampleIndex_ += numSamples`.
4. **Treo ghi tap trong suốt lần chạy — vòng `src/app/AudioEngine.cpp:657-683`.**
   Khi `scOutChannel_ != -1` (giá trị đã snapshot), bỏ qua **toàn bộ** vòng ghi
   tap và thay bằng một `micCapture_.write(capSource, numSamples)` khi
   `scCaptureActive_`. Ba lý do: (a) tap đọc từ `out` **sau** kẹp, tức sau điểm
   tiêm; (b) bỏ qua khác với drop, nên `tapDropCounts_` không tăng và không sinh
   false positive cho người đọc log; (c) tap im làm `liveMs_` của các controller
   dừng lại — nhưng **không tức thì**, xem ngay dưới.

**Đóng băng đồng hồ nhả: con số thật, không phải lời hứa** (F11). `tapAlive =
(nowPolled − lastDataMs_) < kTapSilenceTimeoutMs`
(`src/app/NotchController.cpp:662`, 250 ms), và `lastDataMs_` còn được làm mới
chừng nào detector vẫn rút được phần dư trong ring (`kTapCapacity = 8192` ≈
170 ms @ 48 kHz). Nên từ lúc treo tap tới lúc `tapAlive` thành false là **tới
~420 ms**, và trong khoảng đó `quietMs` vẫn cộng
(`src/app/NotchController.cpp:806-812`). Phát biểu đúng: **mỗi lần chạy làm thang
nhả lane G tiến thêm tối đa ~0,42 s, một lần, không phải suốt 72 s.** Không notch
nào nhả thêm một bậc vì lý do đó (bậc rẻ nhất là 10 s).

**Snapshot đông cứng trong lúc chạy** (§1 mục 6, F11). Vì publish nằm trong vòng
drain (`src/app/NotchController.cpp:617-650`), `copySnapshot()` **không đổi** suốt
lần chạy. Hai hệ quả bắt buộc: overlay §4.9 **không** lấy dữ liệu từ snapshot mà
vẽ dữ liệu của chính lane M; và việc đọc danh sách notch để chọn `index` (§4.6)
chỉ xảy ra **sau** khi tap đã chạy lại.

Không điểm nào trong bốn điểm trên cấp phát, khoá, hay log (invariant 16).

### 4.2 Bộ sinh tín hiệu (`SoundcheckSignal`)

Log sweep, dạng đóng, hàm thuần của chỉ số mẫu `n` (Q1):

```
T_s     = kSweepSeconds                      (giây)
T       = T_s * sampleRate                   (mẫu)
K       = ln(f1 / f0)
phi(n)  = 2*pi * f0 * T_s / K * (exp(K * n / T) - 1)
x(n)    = clampedPeak * w(n) * sin(phi(n))          với 0 <= n < T
x(n)    = 0                                          với n < 0 hoặc n >= T
```

`n < 0` là pha nền nhiễu: cùng một hàm, trả 0, nên **không cần cờ phát riêng**.
`w(n)` là cửa raised-cosine 30 ms hai đầu, `w(0) = 0` và mẫu cuối `= 0`.

**Ramp-out là một hàm riêng, không phải cùng một cửa** (F8):

```
rampOut(n, n0) = 0.5 * (1 + cos(pi * (n - n0) / R))      với n0 <= n < n0 + R
rampOut        = 0                                        với n >= n0 + R
```

`R = kRampOutMs * sampleRate / 1000`. Callback nhân `x(n) * rampOut(n, n0)` với
`n0 = scRampOutAtSample_`. Cả hai là hàm thuần của chỉ số mẫu, nên callback tự
làm được toàn bộ và test headless khẳng định được từng mẫu.

### 4.3 Máy trạng thái (`SoundcheckController`)

Chạy trên `ClockSource` tiêm được (`src/dsp/ClockSource.h:8-13`).

```
Idle
 └─(yêu cầu)→ Preflight   kiểm điều kiện từ chối; GHI LẠI sampleRate + số kênh in/out
      └─→ Confirm         hộp thoại "HẠ MASTER TRƯỚC" + tổng thời lượng (~72 s xấu nhất)
           └─(OK)→ Arm    KIỂM RING RISK LẦN CUỐI (snapshot còn sống ở đây);
                          tắt detection MỌI slot; khoá control (§4.9);
                          log soundcheck_start
                └─→ [với mỗi kênh ngõ ra]
                     NoiseFloor 0,5 s  scOutChannel_ đặt (đã tắt tiếng),
                                       scCaptureActive_ = true,
                                       scSampleIndex_ = -noiseFloorSamples
                       └─ CỔNG: peakiness cửa sổ nền >= kConfirmScore ⇒ Abort(room_ringing)
                     Sweep 3,0 s       scSampleIndex_ đi qua 0
                     Tail 0,7 s
                     Analyse           tính H, nhặt ứng viên; log soundcheck_output
                     Gap 0,3 s         scOutChannel_ = -1, scCaptureActive_ = false
                └─(hết kênh)→ Restore-detection   BẬT LẠI detection ngay (applyModeGating)
                     └─→ Results       log soundcheck_result; mở khoá trừ PRESET LOAD
Results  (detection ĐÃ BẬT LẠI, tap chạy lại, snapshot sống lại)
 ├─(ÁP DỤNG)→ Apply  (message thread) → log soundcheck_apply → Idle
 ├─(BỎ)      → Idle
 └─(hết kResultsTimeoutMs = 20 s)→ Idle

Abort  ← từ bất kỳ pha phát nào: đặt scRampOutAtSample_ (callback lo phần còn
       lại), chờ scOutChannel_ về -1, bật lại detection, mở khoá,
       log soundcheck_abort → Idle
```

**Bốn thay đổi so với rev 1, mỗi cái sửa một finding:**

1. **`Results` chạy VỚI detection đã bật lại** (F9). Rev 1 giữ detection tắt tới
   60 s để khỏi tranh `index`. Nhưng tap chạy lại ngay khi treo được gỡ, nên
   `liveMs_` chạy (`src/app/NotchController.cpp:662-664`), `riskValid` false nên
   `frozen` false (`:706-708`), và thang nhả tiến 60 s
   (`src/app/NotchController.cpp:812-844`): một notch −24 mất bậc đầu ở 30 s và
   bậc thứ hai ở 40 s — **trong khi §3 của rev 1 khai là 0 dB**. Không chấp nhận
   được. Detection trả lại **ngay khi đuôi của kênh cuối kết thúc**;
   `kResultsTimeoutMs` hạ xuống **20 s** dù sao đi nữa.
2. **Cuộc đua `index` xử lý bằng đọc-lại, không bằng tắt detector** (F10) —
   §4.6(a).
3. **Ring risk chỉ đọc ở nơi nó còn sống.** Rev 1 đặt `ringRiskScore ≥
   ringRiskThreshold` làm điều kiện tự hủy **trong lúc chạy**; điều đó **không
   bao giờ đúng được**: detection tắt ⇒ `processSpectrumForDetection` trả về ngay
   (`src/app/NotchController.cpp:1281-1282`) ⇒ `frameScoreValid_` không bao giờ
   true; và tap treo ⇒ không block nào được drain ⇒ snapshot không refresh
   (`:617-650`). Nên: **ring risk đọc ở `Preflight` và lần nữa ở `Arm`**, khi tap
   còn chạy. Phép "phòng có đang ngân không" **giữa lúc chạy** được thay bằng một
   phép đo của chính lane M: cửa sổ nền 0,5 s trước mỗi sweep đưa qua
   `PeakinessAnalyzer`, một bin vượt `CandidateScorer::kConfirmScore` trong một
   cửa sổ lẽ ra phải im nghĩa là phòng đang ngân ⇒ Abort. Cùng một câu hỏi, hỏi
   bằng dữ liệu còn sống.
4. **Mức mic để tự hủy cũng tính trên thread lane M** từ `micCapture_` (RMS
   trượt 20 ms), không lấy từ bất cứ thứ gì của detector.

**Restart thiết bị giữa chừng** (F15). `devicePanel_.onBeforeRestart`
(`src/app/MainComponent.cpp:351-358`) hôm nay chỉ `stop()` các
`NotchController`; nó **không biết** `SoundcheckController` tồn tại. Và
`audioDeviceAboutToStart` xoá mọi tap ring (`src/app/AudioEngine.cpp:713-733`)
dưới tiền điều kiện "không producer, không consumer đang chạy"
(`src/dsp/LockFreeRingBuffer.h:131-141`). Bắt buộc:

- `onBeforeRestart` **abort + join** `SoundcheckController` **trước** khi stop
  các `NotchController`.
- `micCapture_.clear()` thêm vào cùng khối drain trong `audioDeviceAboutToStart`,
  dưới cùng tiền điều kiện.
- `Preflight` ghi lại `getCurrentSampleRateHz()`, `getNumInputChannels()`,
  `getNumOutputChannels()`; mọi pha sau kiểm lại mỗi vòng poll và **abort** khi
  khác. Đổi sample rate làm `T`, `X`, và ánh xạ bin → Hz sai hết, mà
  `getLastDeviceError()` **không** báo gì trong trường hợp đó.

**Cặp kênh phải hợp lệ ở `Preflight`** (F26). Vòng làn chỉ `continue` khi gặp cặp
kênh không hợp lệ (`src/app/AudioEngine.cpp:546-548`, `:555-556`), nên một routing
sai sẽ ra "không đo được" thay vì "sai định tuyến". Thêm nữa,
`numInputChannels_`/`numOutputChannels_` chỉ được ghi từ callback
(`src/app/AudioEngine.cpp:506-507`) và **không bao giờ được reset khi stop**, nên
một giá trị khác 0 có thể là di sản của phiên thiết bị trước. Vì thế `Preflight`
**bắt buộc** kiểm `isRunning()` **và** kiểm cặp `(inputChannels[lane],
outputChannels[lane])` cụ thể của từng slot sẽ đo, và từ chối với thông báo
"định tuyến không hợp lệ" — khác hẳn "không đo được".

### 4.4 Toán của phép đo loop gain (`LoopGainEstimator`)

FFT của detector — 2048 điểm, hop 512, Hann (`src/dsp/Detector.h:66-68`).

1. **Nền nhiễu.** Trong `NoiseFloor` (kênh đã im),
   `Nbar[k] = (Σ_f |mic_f[k]|²) / frames_N`.
2. **Năng lượng phát.** Sinh lại sweep bằng `SoundcheckSignal`, cùng chuỗi FFT →
   `EX[k] = Σ_f |X_f[k]|²`.
3. **Năng lượng thu.** `EY[k] = Σ_f |Y_f[k]|²` trên cả `Sweep` và `Tail`,
   `frames_Y` frame.
4. **Loop gain.**
   `H_dB[k] = 10·log10( max(EY[k] − Nbar[k]·frames_Y, eps) / max(EX[k], eps) )`

**Vì sao tổng trên toàn lần quét thì không cần bù trễ.** Đây là **tỉ số năng
lượng theo bin**, không phải tương quan theo thời gian: năng lượng sweep gửi vào
bin `k` không phụ thuộc nó tới lúc nào, và năng lượng mic nhận ở bin `k` cũng
vậy — **miễn cửa sổ thu bao trọn cả phần quét lẫn phần ngân**. Mệnh đề điều kiện
đó là chỗ phải cẩn thận, và test `DecayLongerThanTheTailIsUnderRead` (§5) **đo**
đúng phần sai khi nó không đúng.

**`H_dB` nghĩa là gì.** `X` là dBFS **ở đầu ra app**, `Y` là dBFS **ở đầu vào
app**, nên `H` là hàm truyền của **toàn bộ phần vật lý** của vòng. Vòng đóng lại
qua app, và app là **đơn vị** ở mọi tần số không có notch
(`src/app/AudioEngine.cpp:592-624`). Suy ra:

> **Hú xảy ra ở bin nào có `H_dB[k] ≥ 0`. Margin của bin đó là `−H_dB[k]` dB.**

**Băng tin cậy hẹp hơn băng quét** (F19). Một log sweep biên độ hằng dành thời
gian bằng nhau cho mỗi octave, nên năng lượng **trên mỗi Hz** tỉ lệ `1/f`. Với
bin rộng cố định 23,4 Hz, năng lượng mỗi bin ở 10 kHz thấp hơn ở 100 Hz đúng
**20 dB**; và ở 10 kHz sweep băng qua một bin trong ~1,5 ms, ngắn hơn hẳn hop
10,67 ms. Với `kMinBinSnrDb = 6`, dải trên rụng vào "không tin cậy" trước tiên.
Quyết định cho v1:

- **Vẫn quét 100 Hz – 10 kHz**, nhưng **chỉ đề xuất notch trong
  `[100 Hz, kTrustedHighHz = 6 kHz]`**.
- Dải 6–10 kHz vẽ kèm nhãn **"độ tin cậy thấp"**, không bao giờ sinh ứng viên.
- **Pre-emphasis (nghiêng +3 dB/octave) hoãn sang bậc sau** cùng với việc nới
  băng tin cậy. TESTER-NOTES ghi: *"trên 6 kHz bản này chỉ vẽ, không đề xuất."*

**Nhặt ứng viên:**

1. Bỏ mọi bin ngoài `[kSweepLowHz, kTrustedHighHz]`.
2. Bỏ bin **không tin cậy**: `EY[k]` không vượt `Nbar[k]·frames_Y` quá
   `kMinBinSnrDb`.
3. Bỏ bin **đã có notch sống trong ±1 bin** — đọc `copySnapshot()` **sau khi tap
   chạy lại** (§4.1).
4. Làm trơn `H_dB` bằng trung bình trượt 1/3 octave → `Hs_dB`.
5. **Đánh dấu** (vẽ marker) khi `H_dB[k] ≥ kCandidateMarginDb` **và**
   `H_dB[k] − Hs_dB[k] ≥ kMinProminenceDb`.
6. **Đề xuất notch** chỉ cho bin đã đánh dấu mà còn thoả
   `needed_dB ≥ kMinUsefulCutDb`.
7. Sắp theo `H_dB` giảm dần, lấy tối đa `kMaxPreventivePerLane`.

**Độ sâu đề xuất — dấu đã sửa** (F6, F22). Rev 1 viết
`needed_dB = -(H_dB + 6)`, cho ra một số **dương** làm `depth_raw` dương, và
`setNotchImpl` từ chối thẳng tại `src/app/NotchController.cpp:214`
(`if (! (depth <= 0.0)) return false;`). Đúng là:

```
needed_dB  = H_dB[k] + kTargetMarginDb        // dB CẦN CẮT; > 0 khi thật sự cần
depth_raw  = -needed_dB                        // luôn <= 0
depth      = bậc SÂU NHẤT của kDepthLadderDb không sâu hơn depth_raw,
             cộng chính trần preset làm bậc cuối (quy tắc Q13 của lane G)
depth      = max(depth, ceiling)               // trần = slider preset
depth      = max(depth, kMaxDepthDb)           // -24, invariant 1 của lane G
```

Kiểm: `H_dB = +2` ⇒ `needed = 8` ⇒ `depth_raw = −8` ⇒ bậc −12. `H_dB = −1` ⇒
`needed = 5` ⇒ `depth_raw = −5` ⇒ bậc −6.

**Biên dưới (bão hoà).** `needed_dB > 24` ⇒ `depth` kẹp ở `kMaxDepthDb = −24`, và
kết quả **phải nói ra**: `OutputResult` mang `saturatedBins`, GUI hiện *"còn vượt
X dB sau khi cắt sâu nhất — chỉnh gain hoặc vị trí mic"*, và `soundcheck_output`
log `saturated: true` kèm `residual_db`. Một notch −24 im lặng ở một bin cần −31
là một lời hứa sai.

**Biên trên (không cắt thừa).** `kCandidateMarginDb = −6` là ngưỡng **đánh dấu**;
`kMinUsefulCutDb = 3,0 dB` là ngưỡng **đề xuất**. Một bin ở `H_dB = −5,9` cần
0,1 dB: nó được **vẽ marker** (người vận hành biết nó sát), nhưng **không** được
đề xuất một notch −6 — rev 1 sẽ cắt thừa 5,9 dB và tiêu một ô notch cho nó
(MINOR 22). Vì `needed ≥ 3` mới đề xuất, bậc −6 không bao giờ cắt thừa quá 3 dB,
và trường hợp `needed == 0` mà rev 1 để ngỏ nay **không thể xảy ra**.

### 4.5 Kết quả một lần chạy

```cpp
struct OutputResult
{
    int   slot = 0, lane = 0, outChannel = 0, inChannel = 0;
    bool  measured = false;            // false = "không đo được" (Q8)
    bool  routingInvalid = false;      // khác hẳn measured == false (F26)
    float snrDb = 0.0f;
    std::array<float, Detector::kNumBins> marginDb {};   // = -H_dB
    std::array<bool,  Detector::kNumBins> trusted {};
    int   markedCount = 0, candidateCount = 0, saturatedBins = 0;
    struct Candidate { float hz; float marginDb; float depthDb; float q;
                       int bin; float residualDb; };
    std::array<Candidate, 6> candidates {};
};
```

`measured == false` là **kết quả hợp lệ**: hiện "không đo được", log `ok: false`,
không đặt gì.

### 4.6 Giao diện với `NotchController`

**Đúng một thay đổi API, và nó là cộng thêm** (F7). Rev 1 khai "không thêm API
nào" và điều đó **sai**: `SnapshotNotch` (`src/app/NotchController.h:394-406`)
không mang `origin`, `model_` là private, nên §4.6(b) của rev 1 — dọn notch
`Origin::Soundcheck` của lần trước — **không thực hiện được**. Mà bỏ qua nó thì
những notch ấy không bao giờ tự nhả (`src/app/NotchController.cpp:727-729`) và
chuỗi 16 ô cạn dần sau vài lần soundcheck.

Thay đổi: **thêm một trường `Origin origin` vào `SnapshotNotch`**, điền ở đúng
chỗ đang dựng danh sách (`src/app/NotchController.cpp:568-582`, một dòng
aggregate-init dài thêm một trường). Cộng thêm, **không** đổi hành vi. Đây là
**thay đổi duy nhất** lane M làm trên `NotchController` ngoài giá trị enum ở (b);
mọi thao tác ghi vẫn chỉ đi qua `setNotch`/`clearNotch` sẵn có.

Khi người vận hành bấm `ÁP DỤNG`:

```cpp
// MESSAGE THREAD. Xem (e).
controller.setNotch (lane, index, cand.hz, cand.q, cand.depthDb,
                     NotchController::Origin::Soundcheck);
```

**a) Chọn `index` — đọc lại ngay trước mỗi lần ghi** (F10). `firstFreeIndexLocked`
(`src/app/NotchController.cpp:1064`) là private, nên lane M dùng `copySnapshot()`
và chọn `index` không xuất hiện cho làn đó. Rev 1 chống đua bằng cách giữ
detection tắt ở `Results`; F9 vừa bác cách đó. Cách của rev 2:

- **Đọc lại `copySnapshot()` ngay trước TỪNG `setNotch`**, không phải một lần cho
  cả danh sách.
- **Cấp phát từ TRÊN xuống**: lấy `index` 15, 14, 13… trong khi detector cấp từ
  dưới lên (`firstFreeIndexLocked` quét `i = 0..kSlots`,
  `src/app/NotchController.cpp:1066-1068`). Hai bên chỉ gặp nhau khi chuỗi gần
  đầy.
- **Cuộc đua còn lại được thừa nhận, không bị giấu**: giữa lần đọc snapshot và
  lệnh `setNotch` vẫn có một khe cỡ micro giây trong đó detector có thể chiếm ô
  và bị ghi đè im lặng (`src/app/NotchController.cpp:226-245`). Nhịp đặt notch
  của detector là ≥ 300 ms (`kDeepenAfterMs`), nên xác suất rất nhỏ nhưng
  **không bằng 0**. Không đóng hẳn được nếu không thêm một API ghi thứ hai —
  thứ Q7 cấm.
- **Kết quả áp dụng một phần được định nghĩa**: `setNotch` trả `false` (validate
  hỏng, hoặc `channel >= width_`) ⇒ lane M **dừng ở đó, không rollback**, và
  `soundcheck_apply` ghi `placed`, `refused`, `cleared_previous`. Notch đã đặt
  được là bảo vệ thật; gỡ chúng ra vì lỗi của cái kế tiếp là tệ hơn.

**b) Dọn lần chạy trước.** Đọc `copySnapshot()`, với mỗi notch có
`origin == Origin::Soundcheck` trên slot đó gọi
`clearNotch(lane, index, ClearReason::SoundcheckReplace)`. Giá trị enum đó **chưa
tồn tại** — thêm vào `src/app/NotchController.h:67-70` là **thay đổi cộng thêm
thứ hai và cuối cùng**, cộng một nhánh trong `tools/logstats.py` (F21). Không có
nó, lần dọn của soundcheck ghi `reason: manual`
(`src/app/MainComponent.cpp:608`) và **không phân biệt được** với việc người vận
hành tự gỡ một notch — đúng loại nhãn sai mà lane D tồn tại để tránh. **Owner
phải gật cho giá trị enum này.**

**c) Làn LINKED** (F16). `setNotch` chỉ đặt **một** làn
(`src/app/NotchController.cpp:195-256`), trong khi đường LINKED của detector
fan-out qua `firstFreeIndexAllLanesLocked` (`:1072-1083`, dùng ở `:1105`). Nếu
lane M đặt lệch làn trên một slot đang LINK, các placement LINKED sau sẽ không
tìm được index trống ở **cả hai** làn và lặng lẽ trượt. Cho v1:

- Slot đang `linked` (đọc `SnapshotBuffer::linked`): **đo một lần trên làn 0**,
  và `ÁP DỤNG` đặt **hai lệnh `setNotch` cùng `index`** cho làn 0 và làn 1 —
  đúng hình dạng `placeConfirmed` tạo ra, nhưng dựng từ phía message thread.
- `index` chọn là ô **trống trên cả hai làn**, suy từ `copySnapshot` (chỉ notch
  `active` vào danh sách, `src/app/NotchController.cpp:576-580`).
- **Không cần API mới nào cho việc này.** Đã cân nhắc một accessor public bọc
  `firstFreeIndexAllLanesLocked`; nó **không cần thiết**. Ghi ra đây để plan
  không thêm thừa.
- Slot mono hoặc INDEP: đo và đặt theo từng làn như §4.4.

**d) Trần.** `setNotchImpl` đặt `ceilingDb = depth` cho mọi origin khác
`Detector` (`src/app/NotchController.cpp:243-245`). Notch phòng ngừa tự làm trần
của chính nó — đúng ý, nhưng là hành vi ngầm của một ternary, nên §5 có test.

**e) Thread** (F27). `setNotch`/`clearNotch` là **policy entry point, message
thread** (`src/app/NotchController.h:13-14`, `:219-220`); `setNotchImpl` còn đọc
`width_` (`:199`) và sample rate của detector (`:209`) **ngoài** `modelMutex_`.
Vì vậy: **`SoundcheckController` (thread riêng) không bao giờ gọi
`NotchController`.** Nó chỉ đặt cờ và công bố `OutputResult`; toàn bộ `ÁP DỤNG` /
`BỎ` chạy trên **message thread**, từ lambda của nút.

**f) Không đụng "phòng nhớ".** Lane G đã có cổng không cho Soundcheck tiêu ký ức
(`src/app/NotchController.cpp:1105-1113`). Lane M không ghi và không tiêu ký ức
phòng; §5 có test.

### 4.7 Log (lane D)

Năm sự kiện, khoá dispatch là **`ev`** — không phải `kind` (bài học lane G A.7).

| `ev` | Khi | Trường |
|---|---|---|
| `soundcheck_start` | vào `Arm` | `outputs`, `peak_dbfs`, `sweep_ms`, `total_ms`, `mode_before` |
| `soundcheck_output` | hết `Analyse` | `slot`, `lane`, `out_ch`, `in_ch`, `ok`, `routing_invalid`, `snr_db`, `saturated`, `candidates[]` (`hz`, `margin_db`, `depth_db`, `residual_db`) |
| `soundcheck_result` | vào `Results` | `outputs_ok`, `outputs_failed`, `marked_total`, `candidates_total` |
| `soundcheck_apply` | sau khi đặt | `placed`, `refused`, `cleared_previous` |
| `soundcheck_abort` | vào `Abort` | `reason` (`user_stop`/`esc`/`engine_stopped`/`device_error`/`device_changed`/`mic_hot`/`room_ringing`/`capture_drop`), `at_output`, `elapsed_ms` |

Số thực làm tròn **3 chữ số có nghĩa** trước khi vào `juce::var` (bài học lane
D). **`tools/logstats.py` KHÔNG cần nhánh nào cho năm `ev` này** — chuỗi
`if/elif` ở `tools/logstats.py:58-70` để một tên `ev` lạ rơi xuyên qua mọi nhánh,
và comment tại đó nói đúng điều ấy. Việc thật duy nhất ở phía logstats là nhánh
cho `ClearReason::SoundcheckReplace` (F21). Rev 1 mô tả ngược.

### 4.8 Preset (Q9)

Định dạng **không đổi**. Notch phòng ngừa là notch bình thường nên `savePreset`
đã ghi chúng. Hệ quả phải ghi vào **cả** `docs/GIOI-THIEU.md` **và**
`docs/KY-THUAT-CHONG-HU.md` trong cùng commit (Definition of done mục 5, F23):

> Preset lưu sau soundcheck **có** mang notch phòng ngừa, nhưng khi nạp lại
> chúng vào model với `Origin::Preset`, nên chúng **sẽ tự nhả sau 30 s yên
> tĩnh** như mọi notch preset khác. Muốn bảo vệ phòng ngừa quay lại đầy đủ thì
> chạy lại ĐO.

Bất đối xứng này (đặt thì vĩnh viễn, nạp lại thì tự nhả) là một trong tám mục
owner phải xác nhận.

### 4.9 GUI (Q10, Q16)

- **Nút.** Q16 (mới) hỏi: nút `ĐO` riêng, hay tái dùng `SOUNDCHECK`? Khuyến nghị
  **nút riêng**; prompt gốc của owner nói tái dùng, nên owner phải chốt. Nhãn
  thật hôm nay: `"SOUNDCHECK"` (`src/gui/ModeRail.h:81`, đang hiển thị) và
  `"Run Soundcheck (15s)"` (`src/gui/ModeBar.h:43`, `modeBar_` bị ẩn —
  `src/app/MainComponent.cpp:211`). **Không có placeholder "sweep the room".**
- **Đồng hồ đếm ngược phải là của lane M** (F12). `getSoundcheckRemainingMs`
  (`src/app/MainComponent.cpp:247-251` → `src/app/NotchController.cpp:1016-1023`)
  báo cửa sổ **thụ động 15 s**, đo trong `liveMs_` — thứ đang đóng băng vì tap bị
  treo. Dùng nó sẽ hiện một con số đứng im hoặc bằng 0. Overlay của lane M vẽ
  đồng hồ của **chính nó**, từ `ClockSource` của `SoundcheckController`.
  `ModeRail::countdownLabel` không bị đụng tới.
- **Trong lúc đo:** overlay trên `SpectrumView` — `ĐANG ĐO · kênh 2/4`, đồng hồ
  lane M, và nút `DỪNG` to. Overlay **giành keyboard focus**; `Esc` là đường phụ
  (F14): "bất kỳ phím nào" của rev 1 không dùng được vì phím còn để gõ tên preset
  (`src/app/MainComponent.cpp:255-272`) và một phím chỉ tới được khi đúng
  component đang có focus. **`DỪNG` là đường dừng chính thức**; `Esc` là tiện
  lợi, không phải bảo đảm.
- Overlay vẽ dữ liệu của **chính lane M**, không đọc `copySnapshot()` (snapshot
  đông cứng trong lúc chạy — §4.1).
- **Control bị khoá khi state ≠ `Idle`** (F10): `SOUNDCHECK`, `AUTO`, `BYPASS`,
  `CLEAR ALL` (`src/app/MainComponent.cpp:241-245`), `PRESET LOAD` và
  `PRESET SAVE` (`src/app/MainComponent.cpp:255-272`), và mọi nút đổi
  enable/width/routing của slot. `PRESET LOAD` bị khoá thêm cả trong `Results`,
  vì `adoptPreset` dùng **index của file** và ghi đè không kiểm `n.active`
  (`src/app/NotchController.cpp:487-506` → `setNotchImpl` `:226-245`) — nó sẽ xoá
  sạch đề xuất đang chờ. Restart thiết bị **không** khoá được (nó có thể tự xảy
  ra), nên nó đi đường abort ở §4.3.
- **Sau khi đo:** đường `marginDb` chồng lên phổ của làn đang hiển thị
  (`setDisplayLane`, `src/gui/SpectrumView.h:202`), marker tại bin đã đánh dấu,
  dải 6–10 kHz vẽ mờ kèm nhãn "độ tin cậy thấp", và một dải kết quả một dòng:
  **"tìm thấy N điểm dễ hú"** + `ÁP DỤNG` / `BỎ`. Số dB chỉ hiện khi rê chuột.
- Kênh "không đo được" và kênh "định tuyến sai" hiện bằng **hai câu khác nhau**,
  không bằng một đường phẳng 0 dB (một đường phẳng trông như "phòng rất tốt").

Theo CLAUDE.md, mọi task đổi hình kết thúc bằng ảnh render
`build/tools/Release/HandsFreeSnapshot.exe` và **đọc lại ảnh** trước khi gửi.

### 4.10 Hằng số (một chỗ, `SoundcheckController.h`)

| Hằng | Giá trị | Nguồn |
|---|---|---|
| `kSweepLowHz` | 100.0 | Q1 |
| `kSweepHighHz` | 10000.0 | Q1 |
| `kTrustedHighHz` | **6000.0** | Q17 (F19) |
| `kSweepSeconds` | 3.0 | Q1 |
| `kTailSeconds` | 0.7 | Q14 |
| `kRampMs` | 30.0 | Q2 |
| `kRampOutMs` | **30.0** | Q17 (F8) |
| `kGapMs` | 300.0 | Q14 |
| `kNoiseFloorMs` | 500.0 | Q8 |
| `kSoundcheckMaxPeak` | 0.1f (−20 dBFS) | Q2 — kẹp cứng, không lên GUI |
| `kSoundcheckMinPeak` | 0.01f (−40 dBFS) | Q2 |
| `kMicAbortDbfs` | −6.0 | Q3 |
| `kMicAbortHoldMs` | 20.0 | Q3 |
| `kMinBandSnrDb` | 12.0 | Q8 |
| `kMinBinSnrDb` | 6.0 | Q8 |
| `kCandidateMarginDb` | −6.0 (ngưỡng **đánh dấu**) | Q14 |
| `kMinUsefulCutDb` | **3.0** (ngưỡng **đề xuất**) | Q17 (F6/F22) |
| `kMinProminenceDb` | 6.0 | Q14 |
| `kTargetMarginDb` | 6.0 | Q14 |
| `kMaxPreventivePerLane` | 6 | Q14 |
| `kResultsTimeoutMs` | **20000.0** | Q17 (F9) |

Cố định cho 1.3.0, **không lên GUI** — một slider trên bất kỳ số nào ở đây biến
mọi báo cáo lỗi thành "lúc đó nó đang ở giá trị nào?".

### 4.11 Invariant an toàn

Mỗi dòng là một phát biểu test được, và §5 có ít nhất một test cho **mỗi** dòng
(F20 — rev 1 có bốn invariant không test nào chạm tới).

1. Mẫu nào rời `SoundcheckSignal` cũng có `|x| <= kSoundcheckMaxPeak`, với **mọi**
   `peak` truyền vào, kể cả âm, NaN, hay > 1.
2. `x(0) == 0.0f`, mẫu cuối của cửa `== 0.0f`, và `x(n) == 0` với `n < 0`.
3. **Mỗi callback**, `scOutChannel_` và `scCaptureInChannel_` kiểm lại với số
   kênh của chính callback đó; chỉ số ngoài phạm vi ⇒ không tiêm, không thu,
   không ghi ngoài mảng.
4. Điểm tiêm nằm **trước** kẹp `±kMaxOutputLevel`
   (`src/app/AudioEngine.cpp:631-647`).
5. Sweep chỉ ghi vào **đúng một** kênh: `scOutChannel_`.
6. `scCaptureActive_ == false` ⇒ **không** mẫu nào vào `micCapture_`.
   `scOutChannel_ == -1` ⇒ **không** mẫu sweep nào và **không** làn nào bị tắt.
   Hai cổng **tách rời**; pha `NoiseFloor` có `scCaptureActive_ == true` và
   `scOutChannel_ != -1` nhưng `scSampleIndex_ < 0` nên biên độ bằng 0.
7. Sáu atomic soundcheck được đọc **một lần** mỗi callback, cạnh `bypass`
   (`src/app/AudioEngine.cpp:509-514`); phần còn lại của callback chỉ dùng bản
   sao stack.
8. Trong lúc chạy, **mọi** làn có `outIdx == scOutChannel_` bị tắt: kênh đó chỉ
   mang sweep, không mang mẫu nào từ chain nào.
9. `Abort` từ bất kỳ đâu ⇒ **callback** sinh ramp-out raised-cosine ≤
   `kRampOutMs` rồi **tự** đặt `scOutChannel_ = -1`. Không cần thread nào khác
   còn sống. Không có đường cắt phựt nào.
10. Trong lúc chạy, không tap nào được ghi, và `tapDropCounts_` **không tăng**.
11. Trong lúc chạy, detection tắt trên mọi slot và **không `NotchCommand` nào**
    được đẩy vào bất kỳ command ring nào.
12. Detection được bật lại **trước** khi vào `Results`, và
    `kResultsTimeoutMs <= 20 s`.
13. Không notch nào được đặt trừ khi người vận hành bấm `ÁP DỤNG`.
14. Mọi độ sâu đề xuất `<= 0`, `>= kMaxDepthDb`, và là một bậc của
    `kDepthLadderDb` **hoặc chính trần preset** — kể cả khi trần không phải bội
    của 6 (ví dụ `presets/Music.json` mang −10).
15. Không notch phòng ngừa nào đặt lên bin đã có notch sống trong ±1 bin.
16. Callback chỉ đụng **sáu atomic + một bộ đếm + một ring**; không lock, không
    cấp phát, không log.
17. `SoundcheckController` (thread riêng) **không bao giờ** gọi `NotchController`;
    mọi `setNotch`/`clearNotch` chạy trên message thread.
18. Lane M không ghi và không tiêu "phòng nhớ" của lane G.
19. `Preflight` thất bại ⇒ **không** mẫu nào được phát, trạng thái về `Idle`.
20. Sample rate hoặc số kênh đổi giữa chừng ⇒ abort trong ≤ một vòng poll (5 ms)
    cộng độ trễ ramp-out.

## 5. Kiểm thử

### 5.1 Headless (`ctest`, thêm vào suite 547)

**`test_soundchecksignal`**
- `PeakIsClampedWhateverIsAsked` (1.0 / 10.0 / −5.0 / NaN). *(inv 1)*
- `FirstAndLastSampleAreExactlyZero`, `NegativeIndexReturnsZero`. *(inv 2)*
- `InstantaneousFrequencyIsMonotoneAndHitsBothEnds` (±5 % hai đầu).
- `RampIsMonotoneOverThirtyMilliseconds`.
- `RampOutIsMonotoneAndReachesExactlyZero` — đường bao giảm đơn điệu từ mốc, mẫu
  cuối bằng đúng 0. *(inv 9)*

**`test_loopgainestimator`** — "phòng tổng hợp", không thiết bị:
- `FlatRoomMeasuresFlatResponse` (trễ 15 ms + gain 0,5 → −6 dB ±1,0 dB).
- `ResonanceLandsInTheRightBin` (±1 bin, ±1,5 dB).
- `DelayDoesNotChangeTheAnswer` — 5 ms **và 900 ms** (900 ms > `kTailSeconds`, nên
  nó thật sự thử biên; rev 1 chọn 5/200 ms, cả hai nằm gọn trong đuôi 0,7 s nên
  test **không thể đỏ** — F18).
- `DecayLongerThanTheTailIsUnderRead` — cộng hưởng T60 = 2,0 s > `kTailSeconds`:
  khẳng định `H_dB` **thấp hơn** giá trị đúng, và **đo sai số** (kỳ vọng ≈
  −10·log10(phần năng lượng bị cắt), lệch ≤ 1,5 dB). Truncation được **đo**,
  không bị giấu.
- `NoiseFloorIsSubtracted`, `BinBelowSnrIsMarkedUntrusted`,
  `UntrustedBinIsNeverACandidate`.
- `AboveTrustedHighHzNeverProducesACandidate` — bin 8 kHz rất nóng vẫn không ra
  ứng viên. *(F19)*

**`test_soundcheck_candidates`**
- `SpeakerRolloffIsNotACandidate` — `H_dB` dốc trơn, không mode ⇒ 0 ứng viên.
- `DepthSignIsNegative` — `H_dB = +2` ⇒ `depth == −12`, và `depth <= 0` cho toàn
  dải `H_dB ∈ [−6, +30]`. **Test này đỏ trên công thức của rev 1.** *(F6)*
- `DepthQuantisesOntoTheLadder`, `CeilingClampsTheProposal`.
- `CeilingNotMultipleOfSixEndsOnTheCeiling` — trần −10 (giá trị thật của
  `presets/Music.json`) ⇒ bậc cuối là −10, không phải −6 và không phải −12.
  *(inv 14, nửa sau mà rev 1 không test)*
- `SaturatesAtMinusTwentyFourAndReportsResidual` — `H_dB = +30` ⇒ `depth == −24`
  **và** `residualDb ≈ 12` **và** `saturatedBins == 1`.
- `MarkedButNotProposedBelowMinUsefulCut` — `H_dB = −5,9` ⇒ `markedCount == 1`,
  `candidateCount == 0`. *(F22)*
- `BinWithALiveNotchIsSkipped` (±1 bin) *(inv 15)*, `AtMostSixPerLane`.

**`test_soundcheckcontroller`** — đồng hồ giả:
- `RefusesWhenEngineNotRunning`, `RefusesWithZeroChannels`,
  `RefusesWhenSlotDisabled`, `RefusesOnInvalidChannelPair` *(F26)*,
  `RefusesWhenRingRiskIsRising` — mỗi cái khẳng định **không mẫu nào được phát**
  và về `Idle`. *(inv 19)*
- `AbortRampsDownInTheCallbackAlone` — thread lane M bị treo (không poll) sau khi
  đặt cờ; chỉ chạy callback ⇒ biên độ vẫn về 0 và `scOutChannel_ == -1`.
  *(inv 9, F8)*
- `DetectionIsRestoredBeforeResults` + `ResultsTimeoutIsTwentySeconds`.
  *(inv 12, F9)*
- `NoNotchCommandIsEmittedDuringARun` — đọc **command ring** của mọi slot trước và
  sau, khẳng định số phần tử không đổi. Rev 1 chỉ khẳng định cái cờ. *(inv 11,
  F20)*
- `NothingIsPlacedWithoutApply`. *(inv 13)*
- `ApplyRunsOnTheMessageThread` — seam ghi lại thread id của mỗi `setNotch`.
  *(inv 17, F27)*
- `ApplyReReadsTheSnapshotBeforeEachSetNotch` — seam chèn một notch detector vào
  ô đã chọn giữa hai lần đặt; lane M phải chọn ô khác. *(F10)*
- `PartialApplyStopsAndReportsRefused`.
- `LinkedSlotPlacesBothLanesAtOneIndex`. *(F16)*
- `RoomMemoryIsUntouched` — đặt notch phòng ngừa, clear, rồi một howl detector ở
  bin đó phải khởi từ thang (−6/−12), **không** từ độ sâu đã nhớ. *(inv 18, F20)*
- `CaptureDropAborts` — ép ring thu đầy. *(F20)*
- `SampleRateChangeAborts`, `ChannelCountChangeAborts`. *(inv 20, F15)*
- `SequencesOneOutputChannelAtATime`. *(inv 5)*

**`test_audioengine`**
- `SweptChannelCarriesOnlyTheSweep` — hai slot enabled cùng trỏ vào kênh đó, cả
  hai có tín hiệu vào; mẫu ra **bằng đúng** mẫu sweep. *(inv 8, F2)*
- `OutputClampStillCoversTheSweepPath` — qua seam **chỉ dành cho test**
  `setSoundcheckPeakUnclampedForTest(5.0f)` (bỏ qua kẹp ở setter, theo đúng kiểu
  `retuneForTest` của lane G): mẫu ra `|v| <= 1.0f` **và** khác 0. Seam tồn tại
  đúng vì lý do này: không có nó, invariant 4 **không thể đỏ** — biên độ đã bị kẹp
  hai lần trước khi tới kẹp cuối (F17).
- `OutOfRangeChannelIsIgnored` — đặt `scOutChannel_` = `numOutputChannels`, chạy
  callback: không crash, không ghi. *(inv 3, F3)*
- `AtomicsAreSnapshottedOnce` — lật `scOutChannel_` từ thread khác giữa callback
  (seam), khẳng định callback đó **hoặc** tiêm **hoặc** ghi tap, không bao giờ cả
  hai. *(inv 7, F4)*
- `NoiseFloorCapturesWithoutEmitting` — `scCaptureActive_` true,
  `scSampleIndex_ < 0` ⇒ ring có dữ liệu, kênh ra im. *(inv 6, F5)*
- `TapsAreSuspendedDuringARun` + `TapDropCountDoesNotMoveDuringARun`. *(inv 10)*
- `IdleEngineEmitsNoSweep`.

**`test_notchcontroller`**
- `SnapshotCarriesOrigin` — `Origin::Soundcheck` đặt qua `setNotch` đọc lại được
  từ `copySnapshot()`. *(F7)*
- `SoundcheckReplaceIsItsOwnClearReason`. *(F21)*

**`test_sessionlogger`** — hình dạng năm `ev` mới; một test khẳng định
`logstats.py` **không cần** nhánh nào cho chúng (chuỗi if/elif ở
`tools/logstats.py:58-70` để chúng rơi xuyên qua), nhưng **có** nhánh cho
`soundcheck_replace`.

### 5.2 Chỉ làm được trên rig

Vào thẳng TESTER-NOTES của bản alpha:

1. Sweep ở −20 dBFS trong phòng thật nghe to cỡ nào so với chương trình — đo
   bằng máy đo SPL.
2. Đường margin có khớp một phép đo tham chiếu (REW/Smaart + mic đo) không? Sai
   **bao nhiêu dB**, lệch **mấy bin**? Riêng dải 6–10 kHz: bản này chỉ vẽ, không
   đề xuất — người đo cho biết có đáng nới không, và có nên pre-emphasis không.
3. Notch phòng ngừa có nâng gain-before-feedback không? Vặn master lên từng dB
   tới khi hú, trước và sau `ÁP DỤNG`. Chênh lệch là con số duy nhất chứng minh
   lane M có giá trị.
4. Dừng khẩn: bấm `DỪNG` giữa sweep — bao lâu thì im, có click không? Đối chiếu
   bảng độ trễ §3.
5. 4,5 giây im trên một kênh ngõ ra có chấp nhận được trong quy trình soundcheck
   thật không, và ~72 s cho hệ 8 slot stereo có quá dài không?
6. Trong phòng ồn thì bao nhiêu kênh ra "không đo được"?
7. Ứng viên lane M có trùng với bin detector **thật sự** notch trong show sau đó
   không? Dữ liệu đối chiếu cho lane C.

## 6. Cần gì từ lane G / R / S / D

| Lane | Cần gì | Đã có chưa |
|---|---|---|
| **S** | Per-lane = per-output; `width`, `linked` trong snapshot | **Đủ** |
| **G** | `kDepthLadderDb`, `kMaxDepthDb`, `getNotchQ()`, trần preset, quy tắc lượng tử Q13 | **Đủ** (`src/app/NotchController.h:100-114`, `:362`) |
| **G** | Cổng không cho Soundcheck tiêu "phòng nhớ" | **Đủ** (`src/app/NotchController.cpp:1105-1113`) |
| **R** | `ringRiskScore/Valid/Threshold` — **chỉ dùng được ở `Preflight`/`Arm`** | **Đủ**, với giới hạn đã nêu ở §4.3 |
| **D** | `SessionLogger::makeEvent`, đường `ev` | **Đủ** (`src/app/SessionLogger.h:49-50`) |
| **D** | `ClearReason::SoundcheckReplace` + một nhánh `logstats.py` | **CHƯA** — cộng thêm, cần owner gật (§4.6b) |
| **G/S** | `Origin origin` trong `SnapshotNotch` | **CHƯA** — cộng thêm, một trường + một dòng init (§4.6) |
| **A** | Round-trip delay của vòng hú | **Lane M KHÔNG cung cấp** (§4.4). Lane A tự lo, hoặc mở nhánh giải chập (Q5 PA 3) |

Hai thay đổi "CHƯA" là **toàn bộ** phần lane M chạm vào code của lane khác. Ngoài
chúng, lane M chỉ **thêm**: bốn điểm chèn ở `AudioEngine`, hai lớp `src/dsp/`,
một lớp `src/app/`, một nút, một overlay, một `micCapture_.clear()` trong khối
drain có sẵn của `audioDeviceAboutToStart`, và hai dòng trong
`devicePanel_.onBeforeRestart`.

## 7. Rủi ro

1. **Phép đo đúng nhưng vô nghĩa vì routing sai.** `Preflight` bắt được cặp kênh
   **không hợp lệ** (F26), nhưng không bắt được cặp **hợp lệ mà sai vật lý** —
   ngõ ra nối tới loa khác. Ngưỡng SNR che một phần, không che "loa khác cũng đủ
   to".
2. **Vòng chéo làn không được đo** (§2).
3. **Vòng qua các kênh ngõ ra KHÁC vẫn đóng trong lúc quét** (§3). Sweep đi vào
   mic rồi ra loa của những kênh đó; nếu phòng sát ngưỡng ở một trong số chúng,
   lần quét có thể kích nó, và `EY` nhiễm phần sweep tái phát từ loa khác. Q15
   PA 3 (tắt **mọi** kênh ra) loại được hẳn cả hai; owner chưa chọn.
4. **Phép đo là một ảnh chụp.** Khán giả vào phòng đổi hấp thụ vài dB ở dải trên;
   margin đo lúc phòng trống đều lạc quan. Notch `Origin::Soundcheck` lại **không
   tự nhả**, nên sai lầm lúc đo **ở lại cả buổi**.
5. **`H_dB` không phải hàm truyền đã hiệu chuẩn**: rò rỉ giữa bin qua cửa Hann,
   và trên 6 kHz SNR mỗi bin thấp hẳn (F19). Đủ cho "bin nào nóng", không đủ cho
   "nóng đúng bao nhiêu dB".
6. **~72 giây và 4,5 s im mỗi kênh** là một thay đổi quy trình vận hành, không
   chỉ một tính năng.
7. **Ô notch cạn.** 6 phòng ngừa × không tự nhả, cộng detector đang chạy, có thể
   làm đầy 16 ô; khi đầy, `firstFreeIndexLocked` trả −1 và detector **im lặng
   không đặt được gì**.
8. **Cuộc đua `index` còn lại ở cỡ micro giây** (§4.6a) — thu hẹp, không loại bỏ.
9. **Va chạm khái niệm "soundcheck".** Sau lane M có hai thứ cùng tên, và
   `Origin::Soundcheck` dùng chung cho cả hai, nên log và preset không phân biệt
   được notch phòng ngừa với notch bắt trong cửa sổ 15 s.
10. **~0,42 s trôi của đồng hồ nhả mỗi lần chạy** (§4.1) — nhỏ so với bậc rẻ nhất
    10 s, nhưng có thật.
11. **Hằng số chưa ai đo trên rig này.** Q14 và Q17 đều là phán đoán.

## 8. Phản biện

Vòng 1: reviewer read-only độc lập, 2026-09-15 — **8 BLOCKER, 11 IMPORTANT,
8 MINOR**; kết luận *"chưa sẵn sàng cho plan, sẵn sàng sau khi hấp thụ 8
BLOCKER"*. Rev 2 hấp thụ **toàn bộ BLOCKER và IMPORTANT** và **8/8 MINOR**.
**Không finding nào bị bác.** Mọi dòng code mà phản biện trích đã được mở kiểm
chứng lại trước khi hấp thụ (bài học lane G A.17: reviewer sai số dòng ba lần
trong lane G) — lần này cả 27 trích dẫn đều đúng.

| # | Mức | Đã làm gì | Ở đâu |
|---|---|---|---|
| 1 | BLOCKER | Bỏ hẳn tự-hủy-theo-ring-risk trong lúc chạy (không thể đúng: `:1281-1282` + `:617-650`); ring risk đọc ở `Preflight`/`Arm`; thay bằng RMS mic + peakiness cửa sổ nền tính trên thread lane M | §4.3(3)(4), §3 |
| 2 | BLOCKER | Tắt tiếng theo **kênh ngõ ra**, không theo `(slot, lane)`; viết lại hàng "không thể hú" cho đúng, kèm bảng độ trễ dừng xấu nhất | §3, §4.1 điểm 2, Q15 lật lại |
| 3 | BLOCKER | Bounds check mọi chỉ số soundcheck **mỗi callback** | §4.1, inv 3 |
| 4 | BLOCKER | Snapshot sáu atomic **một lần**, cạnh `bypass` `:509-514` | §4.1, inv 7 |
| 5 | BLOCKER | Tách `scCaptureActive_` khỏi `scOutChannel_`; invariant 6 viết lại | §4.1, inv 6 |
| 6 | BLOCKER | Sửa dấu: `needed = H_dB + kTargetMarginDb`, `depth_raw = −needed`; định nghĩa bão hoà ở −24 kèm `residualDb` | §4.4 |
| 7 | BLOCKER | Thêm `Origin origin` vào `SnapshotNotch` (cộng thêm); bỏ tuyên bố "không thêm API" | §4.6, §6 |
| 8 | BLOCKER | `scRampOutAtSample_`: callback tự sinh ramp-out và tự xoá `scOutChannel_` | §4.2, §4.1 điểm 3, inv 9 |
| 9 | IMPORTANT | `Results` chạy **với detection đã bật lại**; `kResultsTimeoutMs` 60 s → 20 s; thêm hàng §3 | §4.3, §3 |
| 10 | IMPORTANT | Khoá `SOUNDCHECK`/`AUTO`/`BYPASS`/`CLEAR ALL`/`PRESET LOAD`/`PRESET SAVE`/routing khi state ≠ `Idle`; `PRESET LOAD` khoá cả trong `Results`; restart đi đường abort | §4.9, §4.3 |
| 11 | IMPORTANT | Nêu con số thật: **~420 ms** trôi mỗi lần chạy, không phải "đóng băng"; snapshot đông cứng ⇒ overlay dùng dữ liệu riêng, đọc index sau khi tap chạy lại | §4.1 |
| 12 | IMPORTANT | Đồng hồ overlay lấy từ `ClockSource` của lane M, không từ `getSoundcheckRemainingMs` | §4.9 |
| 13 | IMPORTANT | Gỡ "≈80 dB SPL"; thay bằng "20 dB dưới toàn thang ở master hiện tại"; nguy cơ clipping biến mất nhờ F2 và đã nói rõ | §3 |
| 14 | IMPORTANT | `DỪNG` là đường dừng chính thức; `Esc` best-effort, overlay giành focus | §3, §4.9 |
| 15 | IMPORTANT | `onBeforeRestart` abort+join lane M trước; `micCapture_.clear()` trong khối drain; ghi SR + số kênh ở `Preflight`, abort khi đổi | §4.3, inv 20 |
| 16 | IMPORTANT | Slot LINKED: đo một lần trên làn 0, `ÁP DỤNG` đặt hai lệnh cùng `index`; nêu rõ **không cần** API mới | §4.6(c) |
| 17 | IMPORTANT | Test kẹp viết lại với seam `setSoundcheckPeakUnclampedForTest`, cộng test "kênh chỉ mang sweep" | §5.1 |
| 18 | IMPORTANT | Thêm 900 ms, và thêm `DecayLongerThanTheTailIsUnderRead` đo sai số truncation | §5.1 |
| 19 | IMPORTANT | `kTrustedHighHz = 6 kHz`; 6–10 kHz vẽ nhưng không đề xuất; pre-emphasis hoãn (xem bảng dưới); vào TESTER-NOTES | §4.4, §5.2 |
| 20 | MINOR | Thêm test cho bốn invariant chưa được chạm (lệnh notch, trần lẻ, phòng nhớ, drop ring thu) | §5.1 |
| 21 | MINOR | Sửa ngược lại: `logstats.py` **không** cần nhánh cho `ev` mới (`:58-70`); việc thật là `ClearReason::SoundcheckReplace` | §4.7, §4.6(b) |
| 22 | MINOR | `kMinUsefulCutDb = 3.0` tách ngưỡng **đánh dấu** khỏi ngưỡng **đề xuất** | §4.4, §4.10 |
| 23 | MINOR | Ghi nhận: bảng roadmap do điều phối viên sửa sau merge; §4.8 nay cập nhật **cả** `KY-THUAT-CHONG-HU.md` | §2, §4.8 |
| 24 | MINOR | Thêm Q16 (nút riêng vs tái dùng `SOUNDCHECK`); ghi đúng hai nhãn thật; ghi rõ **không có** placeholder "sweep the room" | §1, §4.9 |
| 25 | MINOR | `kCaptureCapacity` diễn đạt lại là **độ trễ rút chịu được** (0,34 s @ 192 kHz) | §4.1 |
| 26 | MINOR | `Preflight` kiểm cặp kênh cụ thể; `routingInvalid` tách khỏi `measured == false`; nêu rõ số kênh không bao giờ reset khi stop | §4.3, §4.5 |
| 27 | MINOR | Nêu tên thread: `ÁP DỤNG`/`BỎ` trên message thread; thread lane M chỉ đặt cờ | §4.6(e), inv 17 |

**Hoãn, kèm lý do — đúng một mục, và nó là một NỬA của finding 19:**

| Mục | Vì sao hoãn |
|---|---|
| Pre-emphasis sweep (+3 dB/octave), nửa sau của F19 | Nửa trước ("thu hẹp băng tin cậy") đã làm và là phần bắt buộc. Nghiêng phổ của sweep là **đổi thứ phát ra PA**, tức một quyết định mức riêng, và không có dữ liệu rig nào để chọn độ nghiêng. Vào TESTER-NOTES như một câu hỏi cho lần đo đầu (§5.2 mục 2) |

**Vòng 2 — để trống.** Reviewer read-only độc lập thứ hai điền vào đây, đọc file
thật chứ không đọc spec này như một bản báo cáo. Ba chỗ đáng bắn trước: cuộc đua
`index` còn lại ở §4.6(a); lập luận "tổng năng lượng ⇒ không cần bù trễ" ở §4.4
cùng test biên của nó; và liệu tắt theo **kênh ngõ ra** đã đủ chưa, hay phải đi
thẳng tới Q15 PA 3 (tắt mọi kênh).
