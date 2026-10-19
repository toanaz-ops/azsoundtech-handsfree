# Lane A1 — AFC: khử phản hồi thích nghi (PBFDAF + PEM), watchdog, notch làm lưới an toàn

**Ngày:** 2026-10-19. **Roadmap:** [`2026-09-04-anti-feedback-v2-roadmap.md`](2026-09-04-anti-feedback-v2-roadmap.md) (lane A; cần M + G — cả hai đã merge; main `ea6e93c`, suite 775/775).
**Sổ quyết định:** [`../decisions/2026-10-19-lane-a-afc.md`](../decisions/2026-10-19-lane-a-afc.md) (Q1–Q4 owner chọn; Q5+ điều phối chốt tạm).
**Phạm vi:** **A1** = lõi AFC, tham chiếu = đầu ra của chính app (Q1-1). **A2** (ngõ vào tham chiếu riêng + mở engine > 2 kênh, Q1-2) là PR/release sau (Q5).
**Trạng thái:** rev 1, chưa phản biện.
**Đụng audio path:** **có — nặng nhất của dự án.** Xem §6 "Mức thay đổi".
**Release:** 1.5.0 (`-Part minor`).

## 1. Vấn đề

Notch (lane G) chỉ trừ gain ở tần số đang hú: mỗi notch là một lỗ trên tiếng, và
notch luôn tới **sau** hú. AFC trừ **ước lượng của chính tín hiệu phản hồi** khỏi
mic: `e(n) = y(n) − ŷ(n)`, với `ŷ = F̂ * u` (u là tín hiệu app gửi ra loa, `F̂` là
ước lượng đường loa→phòng→mic). Đúng thì vòng hở bị cắt mà không khoét tiếng —
tài liệu báo +10..15 dB GBF (research §3). Sai thì app **tự phát tiếng lạ ra PA**.

## 2. Quyết định đã chốt

| Mục | Quyết định | Sổ |
|---|---|---|
| Tham chiếu | A1: đầu ra app (của chính làn đó); A2 sau: ngõ vào riêng, chọn được | Q1, Q5 |
| Với notch | AFC đứng TRƯỚC chuỗi notch; detector + notch giữ nguyên làm lưới an toàn | Q2 |
| Giảm tương quan | chỉ PEM, trong nhánh học; không đổi tiếng ra loa | Q3 |
| Ra mắt | tắt mặc định, công tắc "AFC (thử nghiệm)" chỉ ở Geek, Dumb không thấy | Q4 |

## 3. Thiết kế DSP (`src/dsp/AfcCanceller.{h,cpp}`, chỉ `juce_dsp`)

### 3.1 Vị trí trong callback

Mỗi làn (slot, lane) đang chạy, khi AFC bật và không Bypass:

```
y(n) = in[n]  ─►  e(n) = y(n) − ŷ(n)  ─►  notch chain  ─►  v(n)  ─►  out[n] += v(n)
                       ▲                                    │
                       └──── F̂ (học từ u = clamp(v)) ◄──────┘
```

- **Tham chiếu `u(n)` = `clamp(v(n), ±kMaxOutputLevel)` của chính làn đó**, không phải
  kênh ra sau khi cộng các làn: kênh ra chỉ xong sau khi mọi làn chạy, còn AFC cần
  `u` từng mẫu. Đúng cho cấu hình thường gặp (một làn một kênh ra); hai làn cùng
  một kênh ra thì mỗi làn chỉ khử phần của mình — ghi ở giới hạn §8.
- Bypass: không đổi (copy). AFC bị **reset** và không học.

### 3.2 Ràng buộc nhân quả — vì sao không thêm độ trễ

Mẫu ra viết trong callback `m` chỉ được phát sau khi callback trả về, nên chỉ có
thể xuất hiện trong ngõ vào của callback `m+1` trở đi. Vậy phản hồi tới `y(n)`
chỉ đến từ `u(n−k)` với `k ≥ D_min`, `D_min ≥` kích thước khối. Spec chốt
**`F̂` chỉ có tap `k ∈ [P, P + L)`, với `P = 64`** (kích thước partition):

- Tại đầu mỗi partition `j`, mọi `u` cần cho `ŷ` của cả partition đã có → `ŷ_j`
  tính một lần bằng FFT, **không thêm mẫu trễ nào**.
- Giả định `round-trip ≥ 64 mẫu` đúng với mọi driver thực (≥ 2 × buffer + trễ
  AD/DA). Phần phản hồi trễ hơn `P + L` (đuôi phòng rất dài) không được mô hình
  hóa — notch vẫn bắt.
- `L = 8192` tap (170 ms @ 48 kHz, 85 ms @ 96 kHz) → `K = 128` partition.

### 3.3 Bộ lọc thích nghi: PBFDAF (MDF), overlap-save

- FFT kích thước `2P = 128` (`juce::dsp::FFT` order 7, cấp phát lúc khởi tạo).
- Lịch sử phổ tham chiếu `X_{j−p}`, `p = 0..K−1` (vòng đệm), và bản **đã làm trắng**
  `X'_{j−p}` cho nhánh học.
- Lọc: `ŷ_j = last P of IFFT( Σ_p W_p · X_{j−1−p} )`.
- Học (sau khi có đủ `e_j`): `E'_j = FFT([0_P, e'_j])` với `e'` = `e` đã làm trắng;
  `W_p += μ · conj(X'_{j−1−p}) · E'_j / (S_j + δ)`, `S_j` = công suất trung bình
  theo bin của `X'` (chuẩn hóa NLMS theo bin). **Ràng buộc gradient** (để W là lọc
  tuyến tính thật, không vòng) áp xoay vòng: mỗi partition chỉ ràng buộc một
  `W_p` (IFFT → xóa nửa sau → FFT) — chi phí cố định, hội tụ gần như ràng buộc đủ.
- Hằng số khởi đầu (điều chỉnh bằng mô phỏng §7, ghi kết quả vào §9 rev sau):
  `μ = 0.02`, `δ = 1e-6 · P`, làm mượt `S` hệ số 0.9.

### 3.4 PEM (Q3)

- Mô hình nguồn AR bậc 16, ước lượng bằng Levinson–Durbin trên `e` (không phải `y`,
  để không học chính phản hồi) mỗi 20 ms, khối 1024 mẫu có cửa sổ, **trên audio
  thread nhưng chia nhỏ**: tự tương quan cộng dồn mỗi partition, Levinson chạy khi
  đủ khối (bậc 16: ~300 phép nhân).
- `A(z)` làm trắng **cả** `u` (→ `X'`) và `e` (→ `e'`) trong nhánh học. Đường ra
  loa không qua `A(z)`.
- `A(z)` không ổn định / hệ số không hữu hạn → giữ `A` cũ (hoặc `A = 1`).

### 3.5 Watchdog (bắt buộc — research §3, roadmap)

Mỗi làn, trên audio thread, ba điều kiện **trip**:

1. Bất kỳ `ŷ`, `e`, hệ số `W` hoặc `A` nào **không hữu hạn**.
2. **Phình đầu ra**: công suất làm mượt (τ = 50 ms) của `e` vượt của `y` hơn
   **+6 dB** liên tục **100 ms** — bộ khử đang **cộng** năng lượng.
3. **Hệ số vô lý**: `Σ|W|²` vượt ngưỡng tương ứng độ lợi vòng ước lượng +12 dB
   (`‖F̂‖² > 16` với tín hiệu chuẩn hóa) — đường phản hồi thật không bao giờ khuếch
   đại như vậy ở mic.

Khi trip:
- **Crossfade `e → y` trong 10 ms** (không cắt phựt), rồi xóa `W`, `X`, `A`, lịch sử.
- AFC của làn đó **nghỉ 5 s**, sau đó tự bật lại từ zero. **3 trip trong 60 s** →
  khóa tắt cho tới khi người vận hành bật lại công tắc; GUI báo một dòng.
- Notch + detector **không bị đụng** (Q2-1): hú lọt qua vẫn bị bắt bằng thang G.
- Đếm trip theo làn bằng atomic; message thread đọc → log `afc_trip` (lý do 1/2/3).

### 3.6 Tương tác với các phần khác

- **Lane M (`ĐO`)**: trong lúc chạy (`scOutChannel_ ≥ 0` hoặc thu mic đang bật), mọi
  AFC **đóng băng + reset**; sweep là tín hiệu lạ do chính app phát và đổi vòng.
  Sau lần đo, AFC bắt đầu lại từ zero.
- **Restart thiết bị** (`audioDeviceAboutToStart`): reset mọi AFC (sample rate /
  buffer mới). `P`, `L` tính theo mẫu nên không phụ thuộc rate.
- **Đổi routing** (`setSlotConfig`): reset AFC của slot đó.
- **Detector**: tap vẫn ghi từ `out` (sau AFC + notch) như hôm nay — detector thấy
  đúng cái ra loa, không đổi.

### 3.7 Bộ nhớ và CPU

- Cấp phát toàn bộ khi engine dựng (không trên audio thread): mỗi làn
  `W`: 128 × 65 complex, `X` và `X'`: 2 × 128 × 65 complex, đệm thời gian vài KB →
  ~200 KB/làn × 16 làn ≈ 3,2 MB.
- Mỗi partition (64 mẫu): 3 FFT/IFFT 128 điểm + 2 × 128 × 65 nhân phức + 1 ràng
  buộc (1 IFFT + 1 FFT) ≈ 1 100 phép nhân thực/mẫu → ~55 M/s mỗi làn @ 48 kHz.
  Engine 2 kênh → tối đa 2 làn → dưới 5 % một nhân. Có test benchmark (§7).

## 4. Điều khiển và GUI (Geek only, Q4)

- `AudioEngine::setAfcEnabled (bool)` (atomic toàn cục, mặc định **false**, **không
  lưu** trong `settings.json` — mỗi lần mở app là tắt, vì là thử nghiệm).
- Geek: một nút gạt `AFC (thử nghiệm)` trong dải DETECTION (`TuningPanel`), kèm đèn
  nhỏ: `TẮT` / `HỌC` (vừa bật < 5 s) / `CHẠY` / `NGHỈ` (sau trip) / `KHÓA` (3 trip).
- Dumb: không có gì mới; nếu AFC đang bật từ Geek thì vẫn chạy (không có đường
  điều khiển thứ hai, bất biến lane U).
- Log session: `afc_state` khi bật/tắt, `afc_trip` (slot, lane, lý do).

## 5. Bất biến

1. AFC tắt ⇒ đầu ra **bit-identical** với bản không có AFC.
2. Không cấp phát, không lock, không I/O trên audio thread.
3. Không thêm độ trễ mẫu nào vào đường tín hiệu.
4. Mọi giá trị không hữu hạn ⇒ trip; đầu ra vẫn qua kẹp ±`kMaxOutputLevel` + NaN
   guard có sẵn (không gỡ, global rule 10).
5. Trip luôn crossfade về `y`, không bao giờ cắt phựt.
6. Notch/detector chạy y như hôm nay dù AFC bật hay tắt.
7. Lane M đang chạy ⇒ AFC đóng băng.

## 6. Mức thay đổi dự kiến (CLAUDE.md: nêu trước)

- **Mặc định (AFC tắt): 0 dB** — bất biến 1, có test bit-identical.
- **AFC bật, đã hội tụ:** thành phần phản hồi giảm theo độ lệch chỉnh đạt được —
  mục tiêu mô phỏng ≥ 10 dB; đường trực tiếp không đổi về kỳ vọng. Trên **phòng
  thật không đoán trước được** con số: phụ thuộc độ dài phòng, mic di chuyển, và
  đường mixer (fader/EQ đổi = đường thay đổi, AFC phải học lại).
- **Lúc học / khi đường đổi:** có thể đổi màu tiếng vài dB (lọc lược nhẹ); tệ nhất
  watchdog cho phép **+6 dB trong ≤ 100 ms** rồi crossfade 10 ms về tín hiệu không
  khử. → **Bắt buộc nghe ở âm lượng thấp trước** (ghi vào tester notes).

## 7. Test

Headless (`ctest`), mô phỏng vòng kín trong test (không thiết bị):
- Nguồn: nhiễu qua lọc AR(10) giống phổ giọng + thành phần có cao độ.
- Phòng giả: trễ 300 mẫu + đáp ứng xung nhiễu suy giảm mũ 60 ms, chuẩn hóa.
- Vòng: `y = s + F*u`, `e = AFC(y)`, `u = G·e` (không notch trong harness).
- Khẳng định:
  - **Hội tụ**: độ lệch `‖F̂−F‖²/‖F‖²` < −10 dB sau 20 s ở độ lợi vòng −3 dB.
  - **MSG**: độ lợi ổn định lớn nhất tăng ≥ 6 dB so với không AFC (tìm bằng mô phỏng).
  - **Watchdog**: đổi `F` đột ngột (×4, đảo dấu) → đầu ra không vượt kẹp, trip hoặc
    hội tụ lại; NaN vào → trip, đầu ra = `y`; khóa sau 3 trip/60 s.
  - **Tắt = bit-identical** qua `AudioEngine` callback thật.
  - **Không thêm trễ**: xung đơn vị qua engine với AFC bật (F = 0) ra đúng mẫu.
  - **Lane M đóng băng AFC**.
  - **Benchmark**: 10 s @ 48 kHz × 2 làn < 1 s CPU (Release) — đèn báo, không phải gate
    cứng trên CI.
- GUI: ảnh `console-afc.png` (công tắc + đèn), đọc lại ảnh.
- Rig (không headless được): phòng thật, mic cầm tay di chuyển, đổi fader nhanh.

## 8. Giới hạn đã biết (A1)

- Hai làn cùng một kênh ra: mỗi làn chỉ khử phần mình gửi ra.
- Kênh khác / nhạc nền / effect trộn vào loa không bị khử (đó là A2).
- Đường mixer đổi nhanh (fader, EQ, mute) = đường phản hồi đổi: AFC phải học lại;
  watchdog + notch che khoảng đó.
- Phản hồi trễ hơn `P + L` (> ~171 ms @ 48 kHz) không được mô hình hóa.

## 9. Docs cập nhật cùng thay đổi

`docs/GIOI-THIEU.md`, `docs/KY-THUAT-CHONG-HU.md` (§ AFC: sơ đồ, hằng số, watchdog),
roadmap (dòng A), tester notes 1.5.0 (nghe nhỏ trước; AFC tắt mặc định).
