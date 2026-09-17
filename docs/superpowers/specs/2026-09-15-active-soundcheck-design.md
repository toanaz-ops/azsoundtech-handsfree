# Lane M — Soundcheck đo chủ động: quét sweep từng ngõ ra, đo loop gain, đề xuất notch phòng ngừa

**Ngày:** 2026-09-15. **Roadmap:** [`2026-09-04-anti-feedback-v2-roadmap.md`](2026-09-04-anti-feedback-v2-roadmap.md) (lane M, cần S — S đã hạ cánh; main `a6be099`, 1.2.0 alpha, suite 547/547).
**Sổ quyết định:** [`../decisions/2026-09-15-lane-m-active-soundcheck.md`](../decisions/2026-09-15-lane-m-active-soundcheck.md) (Q1–Q23, kèm Q3 lật lại lần 2; Q18–Q23 sinh ra trong lúc triển khai).
**Trạng thái:** **rev 5, 2026-09-16 — ĐÃ TRIỂN KHAI** trên nhánh `feat/lane-m-active-soundcheck` qua hết Task 10 (`a6be099..17f5225`, 34 commit, 11 task SDD + fix rounds, suite **712/712**), **owner CHƯA duyệt**. Rev 5 chỉ thêm **§9 "Sai lệch khi triển khai"** — bảng mọi chỗ code khác với phát biểu của rev 4, kèm lý do — và sửa chữ "bảy atomic" thành **tám** ở §4.1, inv 7, inv 16 và §5.2. **CHƯA merge, CHƯA đóng gói.** Trước đó: **rev 4, 2026-09-15** — vòng 3 kết luận **SẴN SÀNG CHO PLAN, không blocker**; rev 4 chỉ là ba chỗ chỉnh nhỏ cộng một khe hở vòng 3 chỉ ra (§8). Trước đó: **rev 3** — sau phản biện read-only độc lập **vòng 2** (2 BLOCKER / 3 IMPORTANT / 2 MINOR, **tất cả đều do chính bản viết lại rev 2 sinh ra**). Vòng 1 (8 BLOCKER / 11 IMPORTANT / 8 MINOR) đã hấp thụ ở rev 2. Bảng đối chiếu cả hai vòng ở §8. **Đã merge main `d64133d` + đóng gói 1.3.0 alpha 17/09/2026 (Q24: owner chấp nhận tạm mọi phương án khuyên dùng — lật lại sau khi nghe rig).** Rev 1 và rev 2 đều **không** được dùng làm cơ sở cho plan.
**Đụng audio path:** **có, và nặng nhất từ trước tới nay** — lần đầu app **tự sinh tín hiệu và phát ra PA**. **Release:** 1.3.0 (`-Part minor`), không gộp với lane nào khác.

> **Owner phải xác nhận chín mục — danh sách này viết TRƯỚC khi có code, và code
> đã tồn tại từ 2026-09-16 mà chưa mục nào được trả lời. Nó nay là danh sách
> chặn RELEASE, không chặn code:** Q2 (mức phát, với
> cách diễn đạt trung thực ở §3) · Q15-lật-lại (im **cả kênh ngõ ra**, tổng
> ~72 s xấu nhất) · Q6 (đề xuất hay tự đặt — prompt gốc của owner viết "đặt") ·
> Q3-lật-lại (bộ điều kiện tự hủy còn lại + độ trễ dừng xấu nhất) ·
> Q16 (nút `ĐO` riêng hay tái dùng `SOUNDCHECK`) · Q7 + Q9 (bất đối xứng khi nạp
> lại preset) · `kResultsTimeoutMs` · `ClearReason::SoundcheckReplace` ·
> trường `Origin origin` thêm vào `SnapshotNotch`.
>
> **Cộng sáu mục nữa, sinh ra TRONG lúc triển khai** (§9, sổ quyết định
> Q18–Q23, cả sáu do điều phối tự chốt 2026-09-16): **Q18** phạm vi dọn notch
> = sổ per-slot chứ không thêm `Origin` mới · **Q19** `applyModeGating` nay tắt
> hẳn detector của slot đang disable — **thay đổi hành vi NGOÀI lane M** ·
> **Q20** báo cáo sau `ÁP DỤNG` giữ console tới khi bấm `BỎ`, không timeout ·
> **Q21** mở khoá trong `Results` tách đôi (rail + `CLEAR ALL` sống, còn lại
> khoá) · **Q22** hoãn abort trong pha nền ≤ ~0,53 s, việc đổi sentinel bị
> PARKED · **Q23** `soundcheck_apply` một dòng mỗi slot.

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
| Tự hủy | Mic RMS > −6 dBFS giữ > 20 ms, **tính trên thread lane M từ `micCapture_`**; peakiness của cửa sổ nền vượt **ngưỡng peakiness đang sống của detector** (`getPeakinessThreshold()`, mặc định 10,0 — **không** phải `kConfirmScore`, xem §4.3); `micCaptureDrops_` tăng; sample rate hoặc số kênh đổi; `isRunning()` hoá false; `getLastDeviceError()` khác rỗng |
| Từ chối chạy | Engine chưa `isRunning()`; cặp (in, out) của slot không hợp lệ với số kênh **thật** của callback; slot disable; **RING RISK ở băng RISING trở lên**, nghĩa là `ringRiskValid && ringRiskScore >= kRiskFreezeFraction × ringRiskThreshold` = 0,55 × 0,7 = **0,385** (§4.3). `ringRiskValid == false` **KHÔNG** từ chối |
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

`AudioEngine` nhận **tám atomic, một bộ đếm, một ring** (rev 4 viết "bảy"; `scRampOutRequested_` là cái thứ tám — §9 D3):

```cpp
// Kênh ĐẦU RA đang đo. -1 = không chạy. Đây LÀ khoá tắt tiếng (Q15 lật lại):
// mọi làn có outputChannels[lane] == scOutChannel_ đều bị tắt.
// Nó về -1 ở MỖI Gap (kênh này đo xong), nên nó KHÔNG dùng được làm khoá
// treo tap -- xem scSuspendTaps_ ngay dưới.
std::atomic<int>      scOutChannel_       { -1 };
// Treo ghi tap cho TOÀN BỘ lần chạy: bật ở Arm, tắt ở cuối đuôi của kênh
// CUỐI CÙNG (hoặc khi Abort). Giữ qua mọi Gap.
std::atomic<bool>     scSuspendTaps_      { false };
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

**Snapshot một lần, đúng luật của nhà** (F4). Tám atomic trên được đọc **một
lần duy nhất** (mười lần load, cộng sample rate và seam test), ngay cạnh `bypass` ở `src/app/AudioEngine.cpp:509-514`, vào biến
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
   Khoá là **`scSuspendTaps_`**, không phải `scOutChannel_` (N3): `scOutChannel_`
   về −1 ở **mỗi** `Gap`, nên khoá theo nó sẽ **bật lại tap 300 ms một lần**
   giữa các kênh. Khi `scSuspendTaps_` (giá trị đã snapshot), bỏ qua **toàn bộ**
   vòng ghi tap và thay bằng một `micCapture_.write(capSource, numSamples)` khi
   `scCaptureActive_`. Ba lý do treo: (a) tap đọc từ `out` **sau** kẹp, tức sau
   điểm tiêm; (b) bỏ qua khác với drop, nên `tapDropCounts_` không tăng và không
   sinh false positive cho người đọc log; (c) tap im làm `liveMs_` của các
   controller dừng lại — nhưng **không tức thì**, xem ngay dưới.

**Đóng băng đồng hồ nhả: con số thật, không phải lời hứa** (F11, N3). `tapAlive =
(nowPolled − lastDataMs_) < kTapSilenceTimeoutMs`
(`src/app/NotchController.cpp:662`, 250 ms), và `lastDataMs_` được làm mới ở
`src/app/NotchController.cpp:572` mỗi lần một block được drain — tức chừng nào
detector vẫn rút được phần dư trong ring (`kTapCapacity = 8192` ≈ 170 ms @
48 kHz). Nên từ lúc treo tap tới lúc `tapAlive` thành false là **tới ~420 ms**,
và trong khoảng đó `quietMs` vẫn cộng (`src/app/NotchController.cpp:809-810`).

**Vì sao khoá phải là `scSuspendTaps_` — con số nếu làm sai.** Rev 2 khoá việc
treo tap theo `scOutChannel_`, thứ mà §4.3 đặt về −1 ở **mỗi** `Gap`. Hậu quả:
tap bật lại mỗi 300 ms, `lastDataMs_` được làm mới, và cửa sổ ~420 ms chạy lại
**cho từng kênh ngõ ra** — ≈ 0,72 s mỗi kênh, **≈ 11,5 s cho 16 kênh**, tức
**vượt `kReleaseStepMs` = 10 s**: một lần soundcheck sẽ làm mọi notch đang nhả
tụt hẳn một bậc, trong khi §3 khai 0 dB. Với `scSuspendTaps_` giữ suốt lần chạy,
phát biểu đúng trở lại: **mỗi lần chạy làm thang nhả lane G tiến thêm tối đa
~0,42 s, MỘT lần cho cả lần chạy** — nhỏ hơn hẳn bậc rẻ nhất 10 s, nên không
notch nào nhả thêm một bậc vì lý do đó.

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
                          ĐỌC ngưỡng cổng nền -> RunParams.noiseFloorGate;
                          scSuspendTaps_ = true  (GIỮ tới hết kênh CUỐI);
                          tắt detection MỌI slot; khoá control (§4.9);
                          log soundcheck_start
                └─→ [với mỗi kênh ngõ ra]
                     NoiseFloor 0,5 s  scOutChannel_ đặt (đã tắt tiếng),
                                       scCaptureActive_ = true,
                                       scSampleIndex_ = -noiseFloorSamples
                       └─ CỔNG: max peakinessAt(cửa sổ nền) >= kNoiseFloorRingingPeakiness
                                ⇒ Abort(room_ringing)
                     Sweep 3,0 s       scSampleIndex_ đi qua 0
                     Tail 0,7 s
                     Analyse           tính H, nhặt ứng viên; log soundcheck_output
                     Gap 0,3 s         scOutChannel_ = -1, scCaptureActive_ = false
                                       scSuspendTaps_ VẪN true
                └─(hết kênh)→ Restore-detection   scSuspendTaps_ = false;
                                                  BẬT LẠI detection ngay (applyModeGating)
                     └─→ Results       log soundcheck_result; mở khoá trừ PRESET LOAD
Results  (detection ĐÃ BẬT LẠI, tap chạy lại, snapshot sống lại)
 ├─(ÁP DỤNG)→ Apply  (message thread) → log soundcheck_apply → Idle
 ├─(BỎ)      → Idle
 └─(hết kResultsTimeoutMs = 20 s)→ Idle

Abort  ← từ bất kỳ pha phát nào: đặt scRampOutAtSample_ (callback lo phần còn
       lại), chờ scOutChannel_ về -1, scSuspendTaps_ = false, bật lại detection,
       mở khoá, log soundcheck_abort → Idle
```

**Tham số của một lần chạy (`RunParams`), đọc MỘT LẦN ở `Arm` trên message
thread rồi trao cho `SoundcheckController`:**

```cpp
struct RunParams
{
    float  noiseFloorGate = 0.0f;   // = controller.getPeakinessThreshold() lúc Arm
    float  peak           = 0.0f;   // đã kẹp <= kSoundcheckMaxPeak
    double sampleRate     = 0.0;    // ghi ở Preflight, dùng để phát hiện đổi rate
    int    numInputChannels = 0, numOutputChannels = 0;
    double ceilingDb      = 0.0;    // trần preset đang chạy
    double notchQ         = 0.0;    // = controller.getNotchQ()
};
```

Mọi trường ở đây **bất biến trong suốt lần chạy**. Đó là chủ ý: một ngưỡng đổi
giữa chừng sẽ làm kênh 1 và kênh 9 của **cùng một lần đo** bị chấm theo hai
thước khác nhau, và người đọc log không có cách nào biết. Đổi ngưỡng giữa chừng
cũng không xảy ra được trong thực tế, vì §4.9 khoá các control từ `Arm`.

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
   còn chạy.

   **Danh tính của "≥ RISING", viết ra để không ai phải đoán** (vòng 3). Từ chối
   chạy khi, và chỉ khi:

   ```
   snapshot.ringRiskValid
     && snapshot.ringRiskScore >= NotchController::kRiskFreezeFraction
                                  * snapshot.ringRiskThreshold
   ```

   `kRiskFreezeFraction = 0.55f` (`src/app/NotchController.h:140`) và
   `ringRiskThreshold` được publish bằng `CandidateScorer::kConfirmScore`
   (`src/app/NotchController.h:436`, gán ở `src/app/NotchController.cpp:648`),
   nên ngưỡng thực tế là **0,55 × 0,7 = 0,385**. Lấy tích từ **snapshot** chứ
   không chép 0,385: đó là cùng một hằng số mà băng RISING của GUI dùng
   (`gui::SpectrumView::kRingRiskRisingFraction` alias chính `kRiskFreezeFraction`),
   nên chip trên màn hình và cổng của lane M không thể trôi khỏi nhau.

   **`ringRiskValid == false` KHÔNG từ chối.** Nó có nghĩa là "chưa chấm được
   khung nào" — luôn đúng trong Bypass, và đúng sau mỗi lần reset cho tới khi
   scorer có lịch sử. Từ chối ở đó sẽ khoá chính cái trường hợp thường gặp nhất:
   mở app, chưa chạy gì, muốn đo phòng. Thay vào đó nó được **ghi lại**:
   `soundcheck_start` mang `ring_risk: null` khi không hợp lệ, và mang con số khi
   hợp lệ — nên khi một lần đo về sau hoá ra sai, người đọc log biết được lúc bấm
   nút phòng đang ở đâu. Phép "phòng có đang ngân không" **giữa lúc chạy** được thay bằng một
   phép đo của chính lane M: cửa sổ nền 0,5 s trước mỗi sweep đưa qua
   `PeakinessAnalyzer::peakinessAt`, và **bin peaky nhất** được so với
   `kNoiseFloorRingingPeakiness`; vượt ⇒ Abort(`room_ringing`). Cùng một câu hỏi,
   hỏi bằng dữ liệu còn sống.

   **Đơn vị: đây là chỗ rev 2 sai, và nó là bài học lane R nguyên văn** (N1).
   Rev 2 so `peakinessAt` với `CandidateScorer::kConfirmScore = 0.7f`
   (`src/dsp/CandidateScorer.h:49`). Hai đại lượng **khác đơn vị hoàn toàn**:
   `kConfirmScore` là ngưỡng của một **tích 0..1**, còn `peakinessAt`
   (`src/dsp/PeakinessAnalyzer.h:166`) là một **tỉ số không chặn trên** — đo trên
   rig thật, bin nhiễu tệ nhất qua 60 seed cho **7,35** và tone 1 kHz cho
   **131,70** (`src/dsp/PeakinessAnalyzer.h:60-65`). Mọi cửa sổ nền, trong mọi
   phòng, đều vượt 0,7 dễ dàng: rev 2 **hủy mọi lần chạy ở kênh đầu tiên, ở khắp
   mọi nơi**. Đây đúng là lỗi mà `memory/ring-risk-lane-r-2026-09-06.md` đã ghi
   ("`score` là tích 0..1 so với `kConfirmScore` 0.7, KHÔNG phải peakiness 10 như
   spec viết") — và nó quay lại trong cùng dự án, ở chiều ngược lại.

   Đúng là so với **ngưỡng peakiness đang sống của detector**:
   `kNoiseFloorRingingPeakiness = controller.getPeakinessThreshold()`
   (`src/app/NotchController.h:364-366`), mặc định
   `PeakinessAnalyzer::kDefaultThreshold = 10.0f`
   (`src/dsp/PeakinessAnalyzer.h:124`), chỉnh được trong [5, 20]. Lấy ngưỡng
   **sống** chứ không chép hằng số: nếu người vận hành hạ ngưỡng vì phòng khó,
   cổng của lane M phải đi theo, không thì hai con số trôi khỏi nhau đúng kiểu
   lane R.
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
nếu needed_dB < kMinUsefulCutDb  -> chỉ ĐÁNH DẤU, không đề xuất (xem "biên trên")
depth_raw  = -needed_dB                        // luôn <= -kMinUsefulCutDb, tức < 0
rung       = bậc NÔNG NHẤT của kDepthLadderDb SÂU ÍT NHẤT BẰNG depth_raw
             (tức bậc r nông nhất thoả r <= depth_raw)
             không có bậc nào thoả  ->  rung = kMaxDepthDb
depth      = max(rung, ceiling)                // trần preset nông hơn thì THẮNG
                                               // (đây LÀ "trần làm bậc cuối", Q13 lane G)
depth      = max(depth, kMaxDepthDb)           // -24, invariant 1 của lane G
residualDb = max(0, needed_dB - (-depth))      // dB còn thiếu sau khi cắt
saturated  = residualDb > 0
```

**Rev 2 phát biểu ngược quy tắc này** (N2): nó viết "bậc **SÂU NHẤT** không sâu
hơn `depth_raw`", tức `r >= depth_raw`. Với `depth_raw = −8` quy tắc đó cho
**−6** (không đủ cắt), và với `depth_raw = −5` nó cho **tập rỗng** — trong khi
chính các ví dụ và test của rev 2 (`DepthSignIsNegative`,
`DepthQuantisesOntoTheLadder`) lại đòi −12 và −6. Văn bản và test mâu thuẫn
nhau; quy tắc ở trên là cái đúng, và nó là cái mà test đã viết ra kỳ vọng.

**Kiểm lại TOÀN BỘ ví dụ dưới quy tắc đã sửa:**

| `H_dB` | `needed` | `depth_raw` | `rung` | trần | `depth` | `residualDb` | |
|---|---|---|---|---|---|---|---|
| +2 | 8 | −8 | **−12** (nông nhất trong {−12,−18,−24}) | −24 | −12 | 0 | khớp `DepthSignIsNegative` |
| +2 | 8 | −8 | −12 | **−10** | **−10** | 0 | trần làm bậc cuối (Q13) |
| −1 | 5 | −5 | **−6** | −24 | −6 | 0 | khớp `DepthQuantisesOntoTheLadder` |
| +30 | 36 | −36 | không bậc nào ⇒ **−24** | −24 | −24 | **12** | bão hoà, báo residual |
| −5,9 | 0,1 | — | — | — | — | — | < `kMinUsefulCutDb` ⇒ **chỉ đánh dấu** |
| +14 | 20 | −20 | −24 | **−10** | **−10** | **10** | bão hoà **do trần**, không do −24 |

**Biên dưới (bão hoà).** `residualDb > 0` xảy ra vì **một trong hai** lý do —
chạm `kMaxDepthDb = −24`, hoặc chạm **trần preset** — và kết quả **phải nói ra**
cả hai: `OutputResult` mang `saturatedBins` và `residualDb`, GUI hiện *"còn vượt
X dB sau khi cắt sâu nhất — chỉnh gain, hạ trần, hoặc đổi vị trí mic"*, và
`soundcheck_output` log `saturated: true` kèm `residual_db`. Một notch −24 im
lặng ở một bin cần −36 là một lời hứa sai.

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

- **Nhận biết "đang LINK" — `SnapshotBuffer::linked` MỘT MÌNH là sai** (N5).
  Trường đó là **công tắc của người vận hành**, không phải hành vi thật:
  `latest_.linked = linked_.load(...)` với comment nói thẳng *"The operator's own
  switch, not effectiveLinked()"* (`src/app/NotchController.cpp:637-640`). Hành vi
  thật là `effectiveLinked() = isLinked() || width_ < 2 || taps_[1] == nullptr`
  (`src/app/NotchController.h:217`) — một slot mono, hoặc một slot stereo thiếu
  tap làn 1, **bị ép LINK** dù công tắc đọc ra INDEP. Suy lại được nguyên vẹn từ
  snapshot, không cần API mới:

  ```
  effectiveLinked  ==  snapshot.linked || snapshot.laneCount < 2
  ```

  vì `laneCount = analysedLanes() = (width_ == 2 && taps_[1] != nullptr) ? 2 : 1`
  (`src/app/NotchController.h:655`, dùng ở `src/app/NotchController.cpp:636`).
  Lane M dùng **vế phải**, không dùng `snapshot.linked` trần.

  *Bẫy harness, ghi sẵn cho plan:* mục 18 của `memory/gain-aware-notch-lane-g-2026-09-07.md`
  — một harness mono có `width_ == 2` làm `effectiveLinked()` bật, và placement
  ghi thêm một entry làn 1 chết, thứ **đầu độc penalty harmonic của test SAU đó
  trong cùng file**. Test LINKED của lane M phải dựng `width_`/tap cho khớp với
  điều nó muốn khẳng định.
- Khi `effectiveLinked`: **đo một lần trên làn 0**, và `ÁP DỤNG` đặt **hai lệnh
  `setNotch` cùng `index`** cho làn 0 và làn 1 — đúng hình dạng `placeConfirmed`
  tạo ra, nhưng dựng từ phía message thread.
- `index` chọn là ô **trống trên cả hai làn**, suy từ `copySnapshot` (chỉ notch
  `active` vào danh sách, `src/app/NotchController.cpp:576-580`).
- **Cặp LINKED là ALL-OR-NOTHING** (N4). Nếu lệnh thứ nhất thành công và lệnh thứ
  hai trả `false`, lane M **phải gỡ lệnh thứ nhất** bằng
  `clearNotch(lane, index, ClearReason::PartialApplyUnwind)` — đúng giá trị enum
  và đúng hành vi mà cả `placeConfirmed`
  (`src/app/NotchController.cpp:1262-1264`) lẫn `adoptPreset`
  (`src/app/NotchController.cpp:528-530`) đã dùng cho cùng tình huống. Lý do là
  lý do của chính comment ở đó: để một làn không được bảo vệ trong khi GUI khai
  là có, thì tệ hơn là không đặt gì. **Quy tắc "dừng, không rollback" ở (a) chỉ
  áp cho INDEP** — ở đó mỗi notch độc lập nên một notch đặt được là bảo vệ thật.
- **Không cần API mới nào cho việc này.** Đã cân nhắc một accessor public bọc
  `firstFreeIndexAllLanesLocked`; nó **không cần thiết**. Ghi ra đây để plan
  không thêm thừa.
- Slot mono hoặc INDEP thật: đo và đặt theo từng làn như §4.4.

**d) Trần.** `setNotchImpl` đặt `ceilingDb = depth` cho mọi origin khác
`Detector` (`src/app/NotchController.cpp:243-245`). Notch phòng ngừa tự làm trần
của chính nó — đúng ý, nhưng là hành vi ngầm của một ternary, nên §5 có test.

**e) Thread** (F27). `setNotch`/`clearNotch` là **policy entry point, message
thread** (`src/app/NotchController.h:13-14`, `:219-220`); `setNotchImpl` còn đọc
`width_` (`:199`) và sample rate của detector (`:209`) **ngoài** `modelMutex_`.
Vì vậy: **`SoundcheckController` (thread riêng) không bao giờ gọi
`NotchController`.** Nó chỉ đặt cờ và công bố `OutputResult`; toàn bộ `ÁP DỤNG` /
`BỎ` chạy trên **message thread**, từ lambda của nút.

Điều đó bao gồm cả **ngưỡng cổng nền**. §4.3 nói cổng so với
`controller.getPeakinessThreshold()`; nếu thread lane M tự gọi hàm đó thì nó vừa
phá quy tắc vừa phá invariant 17 — `getPeakinessThreshold()` là một accessor
chính sách, cùng họ với những hàm mà header khai là message-thread
(`src/app/NotchController.h:13-14`). Nên: **message thread đọc ngưỡng đúng một
lần ở `Arm`** và trao nó vào qua `RunParams::noiseFloorGate` (§4.3). Thread lane
M chỉ so sánh với một `float` mà nó đã được đưa, và **không cầm con trỏ tới
`NotchController` nào cả**.

**f) Không đụng "phòng nhớ".** Lane G đã có cổng không cho Soundcheck tiêu ký ức
(`src/app/NotchController.cpp:1117`, và trần Soundcheck áp ở
`src/app/NotchController.cpp:1170`). Lane M không ghi và không tiêu ký ức
phòng; §5 có test.

> Hai con số này là `:1116` / `:1169` cho tới rev 5; Task 11 mở file trên nhánh
> `feat/lane-m-active-soundcheck` và đếm lại ra `:1117` / `:1170`. Sửa trong đợt
> fix cuối 16/09/2026. Bảng §6 dưới đây sửa theo. Các dòng trong nhật ký phản
> biện vòng 2 (§9) giữ nguyên `:1116` vì chúng ghi lại điều reviewer nói lúc đó.

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

**Một giá trị KHÔNG thuộc bảng trên, và phải đứng riêng vì thế:**

| Giá trị | Nó là gì | Nguồn |
|---|---|---|
| `RunParams::noiseFloorGate` | **Sống, không phải hằng số.** = `controller.getPeakinessThreshold()` (`src/app/NotchController.h:364-366`) đọc **một lần ở `Arm`** trên message thread rồi bất biến suốt lần chạy. Mặc định `PeakinessAnalyzer::kDefaultThreshold = 10.0f` (`src/dsp/PeakinessAnalyzer.h:124`), người vận hành chỉnh được trong [5, 20]. Tuyệt đối **không** phải `kConfirmScore` — xem N1 | Q3 lật lại lần 2 |

Nó nằm ngoài bảng hằng số một cách có chủ ý: xếp nó chung sẽ ngụ ý "cố định cho
1.3.0, không lên GUI", mà nó **đã** trên GUI — dưới tên ngưỡng peakiness của
detector — và chính chỗ đó là lý do nó phải là giá trị sống.

### 4.11 Invariant an toàn

Mỗi dòng dưới đây là một phát biểu **test được**, và §5 có ít nhất một test cho
mỗi dòng — **trừ đúng một ngoại lệ, invariant 16**, thứ không phải một hành vi
quan sát được từ ngoài mà là một tính chất của mã nguồn ("không lock, không cấp
phát, không log"). Nó được **reviewer cưỡng chế**, không phải test cưỡng chế, và
§5.2 ghi nó thành một dòng trong checklist của reviewer SDD. Rev 2 nói "có test
cho **mỗi** dòng" và câu đó không đúng (N6).

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
7. **Tám** atomic soundcheck được đọc **một lần** mỗi callback (mười lần load, cộng `currentSampleRate_` và seam test), cạnh `bypass`
   (`src/app/AudioEngine.cpp:509-514`); phần còn lại của callback chỉ dùng bản
   sao stack.
8. Trong lúc chạy, **mọi** làn có `outIdx == scOutChannel_` bị tắt: kênh đó chỉ
   mang sweep, không mang mẫu nào từ chain nào.
9. `Abort` từ bất kỳ đâu ⇒ **callback** sinh ramp-out raised-cosine ≤
   `kRampOutMs` rồi **tự** đặt `scOutChannel_ = -1`. Không cần thread nào khác
   còn sống. Không có đường cắt phựt nào.
10. Việc treo ghi tap khoá theo **`scSuspendTaps_`**, và cờ đó **giữ nguyên qua
    mọi `Gap`**: từ `Arm` tới hết đuôi của kênh **cuối cùng**, không tap nào
    được ghi và `tapDropCounts_` **không tăng**. Khoá theo `scOutChannel_` (thứ
    về −1 ở mỗi `Gap`) là sai và tốn ≈ 11,5 s đồng hồ nhả cho 16 kênh — §4.1.
10b. Tổng phần `liveMs_` trôi vì một lần chạy là **≤ ~420 ms**, một lần cho cả
    lần chạy, không phải một lần cho mỗi kênh.
11. Trong lúc chạy, detection tắt trên mọi slot và **không `NotchCommand` nào**
    được đẩy vào bất kỳ command ring nào.
12. Detection được bật lại **trước** khi vào `Results`, và
    `kResultsTimeoutMs <= 20 s`.
13. Không notch nào được đặt trừ khi người vận hành bấm `ÁP DỤNG`.
14. Mọi độ sâu đề xuất `<= 0`, `>= kMaxDepthDb`, và là một bậc của
    `kDepthLadderDb` **hoặc chính trần preset** — kể cả khi trần không phải bội
    của 6 (ví dụ `presets/Music.json` mang −10).
15. Không notch phòng ngừa nào đặt lên bin đã có notch sống trong ±1 bin.
16. Callback chỉ đụng **tám atomic + một bộ đếm + một ring**; không lock, không
    cấp phát, không log. **(Cưỡng chế bằng review, không bằng test — §5.2.)**
17. `SoundcheckController` (thread riêng) **không bao giờ** gọi `NotchController`;
    mọi `setNotch`/`clearNotch` chạy trên message thread. Ngưỡng cổng nền cũng
    vậy: message thread đọc `getPeakinessThreshold()` ở `Arm` và trao vào qua
    `RunParams::noiseFloorGate`; thread lane M không cầm con trỏ tới
    `NotchController` nào.
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
  `RefusesWhenSlotDisabled`, `RefusesOnInvalidChannelPair` *(F26)* — mỗi cái
  khẳng định **không mẫu nào được phát** và về `Idle`. *(inv 19)*
- `RefusesWhenRingRiskIsRising` — ba trường hợp trên cùng một
  `ringRiskThreshold = 0.7`: `valid=true, score=0.4` (> 0,385) ⇒ **từ chối**;
  `valid=true, score=0.3` (< 0,385) ⇒ **chạy**; `valid=false, score=0.9` ⇒
  **chạy**, và `soundcheck_start` ghi `ring_risk: null`. Trường hợp thứ ba là
  cái dễ bị hiện thực sai nhất. *(§4.3, vòng 3)*
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
- `LinkedSlotPlacesBothLanesAtOneIndex` — dựng `width_`/tap cho khớp, không dựa
  vào harness mặc định (bẫy ở §4.6c). *(F16)*
- `LinkedIsDerivedFromLaneCountNotJustTheSwitch` — snapshot có `linked == false`
  nhưng `laneCount == 1` ⇒ lane M vẫn coi là LINKED. *(N5)*
- `LinkedPairUnwindsWhenTheSecondLaneFails` — ép `setNotch` làn 1 trả false
  (`failNextSetNotchOnLaneForTest`, `src/app/NotchController.h:298`); khẳng định
  làn 0 bị gỡ, và gỡ bằng đúng `ClearReason::PartialApplyUnwind`. *(N4)*
- `IndepApplyDoesNotUnwind` — cùng kịch bản trên slot INDEP: notch đã đặt **ở
  lại**, `refused` đếm đúng. *(N4)*
- `NoiseFloorOfAQuietRoomDoesNotAbort` — cửa sổ nền là nhiễu tổng hợp
  (peakiness đỉnh quanh 7,35 như đo thật ở `src/dsp/PeakinessAnalyzer.h:60-65`)
  ⇒ lần chạy **đi tiếp**. Trên ngưỡng `kConfirmScore` của rev 2 test này **đỏ**,
  và đó là toàn bộ điểm của nó. *(N1)*
- `NoiseFloorWithARingingToneAborts` — cùng cửa sổ, cộng một tone
  (peakiness ≫ 10) ⇒ `Abort(room_ringing)`. *(N1)*
- `NoiseFloorGateIsReadAtArm` — `setPeakinessThreshold(5)`, chạy, khẳng định cổng
  dùng 5; rồi `setPeakinessThreshold(20)`, chạy lại, khẳng định cổng dùng 20. Tức
  ngưỡng **đi theo detector giữa các lần chạy**, không chép cứng. *(N1)*
- `NoiseFloorGateIsStableWithinARun` — đổi `setPeakinessThreshold` **giữa** kênh
  1 và kênh 2 của một lần chạy; cả hai kênh vẫn chấm theo giá trị đọc lúc `Arm`.
  *(RunParams, vòng 3)*
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
- `TapsStaySuspendedAcrossTheGap` — chạy hai kênh liên tiếp, bơm callback suốt
  `Gap` 300 ms, khẳng định **không** mẫu tap nào được ghi trong khoảng đó. Trên
  thiết kế của rev 2 (khoá theo `scOutChannel_`) test này **đỏ**. *(inv 10, N3)*
- `SweepTouchesOnlyTheMeasuredChannel` — mọi kênh ra khác nhận đúng 0 từ đường
  soundcheck. Rev 2 làm rơi mất test này khi viết lại; nó là test **duy nhất**
  cho invariant 5. *(inv 5, N6)*
- `IdleEngineEmitsNoSweep` — và khẳng định thêm: khi nghỉ, **không làn nào bị
  tắt** (một slot enabled trỏ vào kênh bất kỳ vẫn đóng góp bình thường). Nửa sau
  của invariant 6 không có test nào cho tới rev 3. *(inv 6, N6)*

**`test_notchcontroller`**
- `SnapshotCarriesOrigin` — `Origin::Soundcheck` đặt qua `setNotch` đọc lại được
  từ `copySnapshot()`. *(F7)*
- `SoundcheckReplaceIsItsOwnClearReason`. *(F21)*

**`test_sessionlogger`** — hình dạng năm `ev` mới; một test khẳng định
`logstats.py` **không cần** nhánh nào cho chúng (chuỗi if/elif ở
`tools/logstats.py:58-70` để chúng rơi xuyên qua), nhưng **có** nhánh cho
`soundcheck_replace`.

### 5.2 Cưỡng chế bằng review, không bằng test

Một dòng, và nó phải nằm trong brief của reviewer SDD cho mọi task đụng
`AudioEngine`:

> **Invariant 16** — đọc lại toàn bộ khối soundcheck trong
> `audioDeviceIOCallbackWithContext` và khẳng định: không `lock`, không cấp phát
> (không `new`, không container tăng trưởng, không `juce::String`), không log,
> và không đụng gì ngoài **tám atomic + `micCaptureDrops_` + `micCapture_`**.
> Không có test nào bắt được vi phạm ở đây; chỉ có người đọc.

### 5.3 Chỉ làm được trên rig

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
| **G** | Cổng không cho Soundcheck tiêu "phòng nhớ" | **Đủ** (`src/app/NotchController.cpp:1117`; trần áp ở `:1170`) |
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
| 4 | BLOCKER | Snapshot **mọi** atomic soundcheck **một lần**, cạnh `bypass` `:509-514` (rev 2 có sáu; rev 3 có bảy — N3) | §4.1, inv 7 |
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

---

### Vòng 2 — 2026-09-15, cùng ngày, trên chính bản rev 2

**2 BLOCKER, 3 IMPORTANT, 2 MINOR — cả bảy đều do BẢN VIẾT LẠI sinh ra**, không
phải sót lại từ rev 1. Đó là bài học đáng ghi nhất của vòng này: hấp thụ tám
BLOCKER trong một lượt viết lại **tạo ra lỗi mới với tốc độ đáng kể**, và một
vòng phản biện thứ hai trên bản đã sửa không phải là thủ tục thừa. Rev 3 hấp thụ
**cả bảy**. Không finding nào bị bác. Mọi dòng code vòng 2 trích đều đã mở kiểm
chứng lại (và trích dẫn `NotchController.cpp:1116` của reviewer đúng, còn con số
`1105-1113` của rev 2 sai).

| # | Mức | Đã làm gì | Ở đâu |
|---|---|---|---|
| N1 | BLOCKER | Cổng nền so `peakinessAt` (tỉ số, nhiễu ~7,35 / tone ~131,7) với `kConfirmScore = 0.7` (tích 0..1) ⇒ **hủy mọi lần chạy ở mọi phòng**. Nay so với `getPeakinessThreshold()` đang sống (mặc định 10,0), đặt tên `kNoiseFloorRingingPeakiness`, cộng ba test (nhiễu **không** hủy, tone **có** hủy, cổng đi theo ngưỡng sống). Đây là `memory/ring-risk-lane-r-2026-09-06.md` lặp lại ở chiều ngược | §3, §4.3(3), §4.10, §5.1 |
| N2 | BLOCKER | Quy tắc lượng tử **phát biểu ngược** ("bậc sâu nhất không sâu hơn `depth_raw`") ⇒ −6 cho −8 và **tập rỗng** cho −5, mâu thuẫn với chính ví dụ và test của rev 2. Nay: **bậc nông nhất sâu ít nhất bằng `depth_raw`**, bão hoà ở `kMaxDepthDb` kèm `residualDb`, trần áp bằng `max(rung, ceiling)`. Kiểm lại **toàn bộ** sáu ví dụ dưới quy tắc mới, thành bảng | §4.4 |
| N3 | IMPORTANT | "~420 ms một lần" sai ~20×: `scOutChannel_` về −1 ở **mỗi** `Gap`, nên tap bật lại 300 ms một lần ⇒ ≈ 0,72 s mỗi kênh ⇒ **≈ 11,5 s cho 16 kênh, vượt `kReleaseStepMs` 10 s**. Thêm atomic thứ bảy `scSuspendTaps_` giữ suốt lần chạy; sửa số; thêm invariant 10/10b và test `TapsStaySuspendedAcrossTheGap` | §4.1, §4.3, §4.11, §5.1 |
| N4 | IMPORTANT | Cặp LINKED nay **all-or-nothing** với `ClearReason::PartialApplyUnwind`, khớp `placeConfirmed` (`:1262-1264`) và `adoptPreset` (`:528-530`); "dừng, không rollback" thu hẹp về **chỉ INDEP**; hai test | §4.6(c), §5.1 |
| N5 | IMPORTANT | `SnapshotBuffer::linked` là **công tắc**, không phải hành vi (`:637-640`). Nay dùng `snapshot.linked \|\| snapshot.laneCount < 2` (`NotchController.h:655`, `:217`), cộng ghi chú bẫy harness (lane G mục 18) và một test | §4.6(c), §5.1 |
| N6 | MINOR | Lời mở §4.11 nói quá về độ phủ: invariant 16 nay ghi rõ **cưỡng chế bằng review** và có §5.2 là một dòng checklist cho reviewer SDD; khôi phục `SweepTouchesOnlyTheMeasuredChannel` (inv 5, rev 2 làm rơi mất); thêm khẳng định "nghỉ thì không làn nào bị tắt" vào `IdleEngineEmitsNoSweep` (inv 6 nửa sau) | §4.11, §5.1, §5.2 |
| N7 | MINOR | Dòng cổng "phòng nhớ" là `src/app/NotchController.cpp:1116`, không phải `1105-1113` | §4.6(f), §6 |

---

### Vòng 3 — 2026-09-15, trên bản rev 3

**Kết luận: SẴN SÀNG CHO PLAN, không blocker.** Ba mục chỉnh nhỏ cộng một khe hở
thật mà hai vòng trước đi qua. Cả bốn đã hấp thụ ở rev 4; không mục nào hoãn,
không mục nào bị bác.

| # | Mức | Đã làm gì | Ở đâu |
|---|---|---|---|
| R3-1 | Khe hở | §4.3 bảo cổng nền so với `controller.getPeakinessThreshold()`, nhưng **ai gọi hàm đó** thì không nói — và nếu thread lane M tự gọi thì nó phá chính invariant 17. Nay: **message thread đọc một lần ở `Arm`** và trao vào qua `RunParams::noiseFloorGate`; thread lane M chỉ so với một `float` đã được đưa và **không cầm con trỏ tới `NotchController`**. Invariant 17 giữ nguyên câu "không bao giờ gọi `NotchController`". Thêm `RunParams` (mọi trường bất biến trong một lần chạy) và đổi test: `NoiseFloorGateIsReadAtArm` + `NoiseFloorGateIsStableWithinARun` | §4.3, §4.6(e), §4.10, §4.11 inv 17, §5.1 |
| R3-2 | Chỉnh | "≥ RISING" nay viết ra thành **danh tính**: từ chối khi `ringRiskValid && ringRiskScore >= kRiskFreezeFraction × ringRiskThreshold` = 0,55 × 0,7 = **0,385** (`src/app/NotchController.h:140`, `:436`, gán ở `src/app/NotchController.cpp:648`) — lấy tích từ snapshot, không chép 0,385. Và nói rõ `ringRiskValid == false` (luôn đúng trong Bypass, hoặc chưa có lịch sử kể từ lần reset) **KHÔNG** từ chối, chỉ được ghi vào `soundcheck_start` là `ring_risk: null`. Test ba nhánh: 0,4 từ chối / 0,3 chạy / `valid=false` chạy | §3, §4.3, §5.1 |
| R3-3 | Chỉnh | `kNoiseFloorRingingPeakiness` gỡ khỏi bảng "Cố định cho 1.3.0, không lên GUI" — nó **không** cố định và nó **đã** trên GUI (dưới tên ngưỡng peakiness của detector). Nay đứng riêng thành `RunParams::noiseFloorGate`, ghi rõ "sống, = ngưỡng detector đọc lúc `Arm`" | §4.10 |
| R3-4 | Chỉnh | Tham chiếu chéo sai sau khi rev 3 chèn thêm một mục: §4.11 và invariant 16 trỏ "§5.3", đúng phải là **§5.2** | §4.11 |

**Vòng 4 — để trống.** Spec này đã qua ba vòng phản biện độc lập và vòng 3 kết
luận sẵn sàng cho plan, nên vòng tiếp theo **không nên là một vòng đọc spec
nữa**: nó nên là người đối chiếu **plan** với code thật trước khi dispatch (bài
học lane G mục 16: một plan chưa được kiểm chứng cho tới khi có người MỞ file nó
trích dẫn — ba helper mà plan lane G dựa vào không tồn tại ở đâu trong repo).

Bốn chỗ vẫn đáng bắn, và chúng là rủi ro **thiết kế**, không phải lỗi văn bản:
cuộc đua `index` còn lại ở §4.6(a); lập luận "tổng năng lượng ⇒ không cần bù
trễ" ở §4.4 cùng test biên của nó; liệu tắt theo **kênh ngõ ra** đã đủ chưa hay
phải đi thẳng tới Q15 PA 3; và — bài học đắt nhất của lane này — **mọi đơn vị đo
xuất hiện trong một phép so sánh**, vì lane M đã sai đúng kiểu đó một lần
(N1) và lane R đã sai đúng kiểu đó trước nó.

---

## 9. Sai lệch khi triển khai (rev 5, 2026-09-16)

Bảng này ghi **mọi chỗ code đã hạ cánh khác với phát biểu của rev 4**, cùng lý
do. Nguồn là ledger SDD `.superpowers/sdd/2026-09-15-active-soundcheck/progress.md`
— mỗi dòng dưới đây tương ứng một `RULING` / `NOTE` / `parked` trong đó. Không
mục nào ở đây là một quyết định của owner; mục nào cần owner thì đã lên danh
sách ở đầu file (Q18–Q23).

**Quy ước:** §4.x là mục của chính spec này bị phát biểu sai hoặc thiếu; "vì
sao" là lý do kỹ thuật, không phải sở thích.

| # | Spec rev 4 nói | Đã hạ cánh | Vì sao |
|---|---|---|---|
| D1 | **Bảy** atomic soundcheck, **chín** lần load (§4.1, inv 7, inv 16, §5.2) | **TÁM atomic, MƯỜI lần load** (cộng `scRampOutRequested_`) | Xem D3. Văn bản inv 7/16 và §4.1/§5.2 đã sửa ở rev 5; các bảng lịch sử ở §8 giữ nguyên chữ "bảy" vì chúng ghi lại vòng phản biện, không ghi lại code |
| D2 | Kênh ngõ ra được arm từ đầu pha `NoiseFloor`; im **4,5 s** mỗi kênh, ~72 s cho 16 kênh | `scOutChannel_` **vẫn là −1 suốt pha nền**; chỉ được arm **sau khi cổng nền nói phòng đã yên**, với `scSampleIndex_` publish ở `−kSweepLeadInMs` = **−20 ms**. Im **4,52 s** mỗi kênh, **~72,3 s** cho 16 kênh | Bản đầu là một **cuộc đua thời gian**: chỉ số audio đã chạy về 0 trong lúc thread lane M còn đang chấm cổng, nên một lần poll trễ > 20 ms **phát sweep vào một phòng đang ngân** (đo được 1,9 mV sweep trước khi cổng kịp hủy). Không arm thì không có đồng hồ nào để đua. `kSweepLeadInMs` là **chạy đà**, không phải biên an toàn: nó bảo đảm mẫu phát đầu tiên là index 0, đúng đầu ramp, với **mọi** buffer size |
| D3 | Ramp-out: message thread chốt mỏ neo `scRampOutAtSample_` | Message thread **xin** qua `scRampOutRequested_`; **callback chốt** mỏ neo ở block đầu tiên nó thấy cờ. `setSoundcheckOutputChannel()` xoá cả hai (release store); `(-1)` là chốt chặn abort | Hai lỗi thật, một cơ chế: (a) chốt trên message thread làm block ramp đầu tiên bắt đầu ở envelope **0** — **cắt phựt** — khi buffer ≥ 1440; (b) abort kiểu `device_changed`/`engine_stopped` để lại mỏ neo không ai xoá, treo controller ở "chờ về −1" và **cắt cụt lần chạy sau** |
| D4 | Deadline của `Sweep`/`Tail` cộng dồn từ deadline pha nền | **Đóng dấu lại từ chính lần poll của cổng**; `releaseOutputChannelSafely()` dùng chung cho `enterGap`/`finishRun` | Sau D2, audio được neo vào lần poll của cổng còn deadline thì không — một lần poll trễ `L` rút ngắn đuôi đi `L`, và `L > 700 ms` **nhả một kênh chưa im** = cắt phựt |
| D5 | Làn định tuyến vào kênh đang đo bị **bỏ qua** (`continue`) | Bỏ qua **và** gọi `NotchChain::clearState()` **mỗi block** cho làn đó — state-only, **không** đụng `rampRemaining_` | `continue` đóng băng state DF1 tới 4,5 s; lúc mở lại, đáp ứng tự do từ state cũ tới **~5–6×** mức lúc tắt = "cạch" tại tần số notch, mỗi kênh. **Mức thay đổi dự kiến** (bắt buộc theo CLAUDE.md): lúc tắt = im đúng như §3 khai; lúc mở lại = chương trình quay lại từ **state 0**, không có đáp ứng tự do. Bản sửa đầu dùng `reset()`, thứ xoá luôn `rampRemaining_` và **làm kẹt** mọi ramp độ sâu lane G đang bay — nên phải là `clearState()` |
| D6 | Cổng nền so `peakinessAt` với ngưỡng peakiness **trên toàn phổ**; không nói hỏng-thế-nào | Ngưỡng = `getPeakinessThreshold()` **đọc một lần trên message thread lúc `Arm`**, trao vào qua `RunParams::noiseFloorGate`; chấm **chỉ trong băng `[kSweepLowHz, kTrustedHighHz]`**; **không chấm được khung nào ⇒ HỎNG THEO HƯỚNG ĐÓNG**, hủy với lý do riêng `noise_floor_unmeasured` | Max trên cả 1025 bin (0 Hz–Nyquist) thì một tiếng rít 15 kHz của đèn LED hoặc ù nguồn **hủy mọi lần chạy**; detector cũng chỉ nhìn băng đó. "Không có dữ liệu" mà đoán là "phòng sạch" là hỏng theo hướng nguy hiểm — nên nó là lý do hủy thứ **chín**, phân biệt được với `room_ringing` |
| D7 | `AbortReason` có **tám** giá trị (§4.7) | **CHÍN**: thêm `noise_floor_unmeasured`. Mỗi lý do trừ `user_stop`/`esc` có **một câu tiếng Việt riêng** trên màn hình | Xem D6. Câu riêng: mọi abort khác được quyết trên thread lane M, thứ không được đụng component nào — nếu không có câu đó thì một lần chạy **biến mất không lời giải thích** |
| D8 | `arm()` trả `bool` | Trả `Refusal` (`None`, `EngineNotRunning`, `NoChannels`, `SlotDisabled`, `InvalidChannelPair`, `RingRiskRising`, `InvalidParams`, `AlreadyRunning`, `RampOutPending`), **append-only** | Một cú từ chối phải nói được **vì sao**: bốn lý do khác nhau cần bốn câu khác nhau trên thanh trạng thái |
| D9 | Ring-risk chỉ kiểm ở `Preflight` | `arm()` **nhận `SnapshotBuffer` và kiểm lại**, cùng một danh tính (`ringRiskValid && ringRiskScore >= 0,55 × ringRiskThreshold`) | Hộp thoại xác nhận có thể đã mở hàng chục giây; phòng đổi trạng thái trong khoảng đó |
| D10 | `abortAndJoin()`: chạy `runOnce()` rồi join (thứ tự của plan) | **Join TRƯỚC**, rồi `runOnce()` trên message thread, khẳng định `Idle`. `stop()` và destructor **stand down** toàn bộ (backstop −1, tắt thu, gỡ treo tap, bật lại detection). Header khai: caller phải `start()` lại sau restart, và ba lambda tiêm vào được gọi **trên thread của caller** | Thứ tự của plan để **hai thread cùng trong máy trạng thái**, và lane M có thể **arm lại cả một lần quét sau khi đã abort**. Destructor/`stop()` giữa lần chạy để engine **còn armed và còn phát**, tap còn treo, detection còn tắt, và không còn ai để khôi phục — class này là người ghi duy nhất của các atomic đó nên nó phải là người dọn |
| D11 | Abort trong pha `NoiseFloor` được nghe thấy ngay | **Hoãn tới khi `scSampleIndex_` về 0** (≤ ~0,53 s), vì mỏ neo chốt lúc index còn âm trùng với sentinel "−1 = chưa có mỏ neo". **Không có mẫu nào phát ra trong cửa sổ đó** (biên độ đã bằng 0, inv 9 vẫn đúng); hệ quả duy nhất là kênh nằm im thêm ≤ ~0,53 s. `setSoundcheckOutputChannel(-1)` là chốt chặn bắt buộc, có test ghim | **PARKED, chờ quyết**: đổi sentinel (`INT64_MIN` hoặc một bit hợp lệ) là một thay đổi đúng nhưng không được nhét vào một fix round; nó nằm ở Q22 |
| D12 | `SoundcheckCandidates::Input::ceilingDb` mặc định `0.0` | Mặc định **`NaN` = "chưa đặt"**; `pick()` **từ chối đề xuất** (vẫn đánh dấu) và bật `Output::ceilingMissing`; `depthFor()` kiểm `isfinite` **tại cửa** | `0.0` là **hướng an toàn** (không cắt) nhưng **không phân biệt được** với "phòng sạch" — GUI sẽ hiện "không tìm thấy điểm nào" cho một lỗi cấu hình. Bẫy kèm theo: `std::max(rung, NaN)` **trả về `rung`**, nên sentinel đi lọt nếu chỉ trông vào phép toán |
| D13 | `applySoundcheckResults` dọn **mọi** notch `Origin::Soundcheck` của slot | Dọn theo **sổ per-slot `SoundcheckApplyLedger`** `(lane, index, hz)` do caller giữ, khớp `active && origin == Soundcheck && \|Δhz\| ≤ 1 bin`. **Không** thêm enumerator `Origin` mới | Mode `SOUNDCHECK` 15 giây thụ động **cũng** sinh notch mang origin đó, nên bản cũ **xoá im lặng** lớp bảo vệ người vận hành tự khoá vào. Thêm `Origin` mới thì phải rà lại mọi chỗ lane G kiểm `origin != Soundcheck` (Q7 cấm thêm API cho `NotchController`). **Q18** |
| D14 | Inv 15 (±1 bin) chỉ so với notch **đang sống** | Cộng danh sách **`placedThisCall`**: một đề xuất cũng không được đặt cạnh notch **vừa do chính lượt này đặt** | `pick()` không có ràng buộc tách nhau giữa các ứng viên, nên hai đề xuất kề bin **chồng nhau tới ~−48 dB** ở chỗ trần chỉ cho phép −24 |
| D15 | `applySoundcheckResults(controller, results, ledger)` | Thêm tham số **`int slot`**; kết quả gửi nhầm slot bị **bỏ qua và đếm** (`skippedOtherSlot`), làn không hợp lệ cũng vậy (`skippedBadLane`) | `result.slot` chưa bao giờ được so, nên `copyResults()` có thể **đặt chéo slot** |
| D16 | Độ sâu đặt xuống = độ sâu đề xuất (đóng băng lúc `Arm`) | `std::max(cand.depthDb, controller.getNotchDepthDb())` ở **cả hai** chỗ gọi `setNotch` | Slider DEPTH hạ trong lúc chạy/xem kết quả ⇒ `ÁP DỤNG` đặt **sâu hơn slider**. Đây là bài học M-B của lane G: kẹp một chiều cho một giá trị có hai đường trở nên cũ là **nửa cái kẹp** |
| D17 | `Results`: khoá mọi thứ trừ `PRESET LOAD` | **Khoá tách đôi.** `Measuring` = khoá hết. `Pending` (`Results` **và** `Applied`): rail mode + `CLEAR ALL` **sống**; `ĐO`, `PRESET LOAD/SAVE`, `slotPanel_` (LINK/width — snapshot `linked` cũ ~10 ms), `tuningPanel_` (`RunParams` đã đóng băng) và `notchListPanel_` (nút FALSE = một cú CLEAR ALL cục bộ lên chính chuỗi vừa đo) **khoá** | `BYPASS` là cách một soundman cứu một show — khoá nó 20 giây là sai hướng. **Q21** |
| D18 | Báo cáo sau `ÁP DỤNG` biến mất theo `kResultsTimeoutMs` | `Mode::Applied` **giữ console tới khi bấm `BỎ`**, không timeout | `clearedPrevious > 0 && placed == 0` nghĩa là **đã gỡ notch cũ mà không đặt lại được cái nào — phòng KÉM an toàn hơn trước khi bấm**. Thứ đó không bao giờ được biến mất im lặng. **Q20** |
| D19 | Detection bật lại **trước** khi vào `Results` (inv 12) — và hết | Bật lại **theo luật của MODE**, gated ở **cả hai** cạnh máy tự chạy: lúc `finishRun()` và lúc **vào `Results`** | `finishRun()` khôi phục **vô điều kiện**, nên cả cửa sổ `Results` (≤ 20 s) chạy với detection bật kể cả trong `BYPASS`, và slot đang disable bị **arm** |
| D20 | (không nói) | **`applyModeGating` nay TẮT HẲN detector của một slot đang disable**, thay vì bỏ qua nó | Early-return cũ vô hại **cho tới khi** lane M bắt đầu khôi phục hàng loạt. Đây là **thay đổi hành vi NGOÀI phạm vi lane M** và owner phải nhìn. **Q19** |
| D21 | Hộp thoại xác nhận: kiểm "state != Idle" | **Bộ đếm thế hệ console**: mọi lần teardown/restart tăng bộ đếm; một `OK` mang thế hệ cũ bị bỏ **kèm một câu**. `updateMeasureEnabled()` là **người ghi duy nhất** của trạng thái enable của `ĐO` | Một restart device **lúc đang Idle** làm trạng thái "trông vẫn ổn", nên một `OK` cũ **arm một lần quét trên rig khác**. Một bộ đếm ghi lại rằng **có chuyện đã xảy ra** — thứ một phép kiểm trạng thái không thấy được |
| D22 | `soundcheck_apply` = `placed`, `refused`, `cleared_previous`; một sự kiện cho một lần `ÁP DỤNG` | **Một dòng mỗi slot**, có trường `slot`; cộng `skipped_other_slot` và `skipped_bad_lane`. `skipped_live` **cố tình không có** (nó là **tập con** của `refused`) | Hai cái `skipped_*` là **lỗi**, không phải thống kê, và không nhìn thấy được ở đâu ngoài GUI nếu không ghi. Ghi `skipped_live` cạnh các tổng sẽ mời người đọc cộng nó vào. **Q23** |
| D23 | `soundcheck_abort.at_output` (không định nghĩa) | Là chỉ số lượt đo **đang dở**, không phải lượt cuối đã xong — hủy giữa kênh 1 ghi `at_output` **1** | Ghi rõ để người đọc log không lệch một |
| D24 | `SnapshotNotch` thêm `Origin origin` | Đã thêm, **append-only**, không có initialiser thứ hai; `ClearReason::SoundcheckReplace` cũng append-only và `logstats.py` có đúng một nhánh mới cho nó | Không sai lệch — ghi vào đây để bảng này là danh sách **đầy đủ** những gì đã đụng tới API của `NotchController`/lane D |
| D25 | `audioDeviceAboutToStart` xoá các ring | Xoá **cả tám atomic soundcheck** cạnh `micCapture_.clear()` | Không xoá thì `scOutChannel_` cũ **phát sweep ra kênh N của thiết bị KẾ TIẾP** |
| D26 | (không nói) | `micCapture_` là **một người đọc duy nhất**, và `onBeforeRestart` phải **abort + join lane M TRƯỚC** khi drain — khai trong header | `audioDeviceAboutToStart` nay ghi `scSuspendTaps_`/`scCaptureActive_`, nên hợp đồng abort-và-join-trước-restart không còn là khuyến nghị |

**Ba mục hoãn, ghi để khỏi mất dấu:** (1) sentinel "chưa có mỏ neo" — D11/Q22;
(2) `releaseOutputChannelSafely()` thiếu chốt `!engine_.isRunning()` mà
`stopEmissionSafely()` có (hôm nay không tới được); (3) một mục sổ mồ côi bởi
`CLEAR ALL` hoặc một lần đổi width được mang theo vô hạn (chặn trên
`kTotalSlots`) — `CLEAR ALL` **không** reset sổ, và tester notes nói ra điều đó.
Danh sách minor hoãn đầy đủ nằm ở các dòng `minor (deferred)` của ledger SDD.

**Một invariant không test nào giữ được, lần thứ hai nói ra:** đường `ÁP DỤNG`
**không tới được trong headless** — nó cần GUI thật và device thật. Lần
`ÁP DỤNG` thật đầu tiên trong đời tính năng này sẽ xảy ra **trên một dàn thật**.
`installer/TESTER-NOTES.md` phải nói câu đó, và nó có nói.
