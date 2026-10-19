# Lane A1 — AFC: khử phản hồi thích nghi (PBFDAF + PEM), watchdog, hạ tạm khi trip, notch làm lưới an toàn

**Ngày:** 2026-10-19. **Roadmap:** [`2026-09-04-anti-feedback-v2-roadmap.md`](2026-09-04-anti-feedback-v2-roadmap.md) (lane A; cần M + G — đã merge; main `ea6e93c`, suite 775/775).
**Sổ quyết định:** [`../decisions/2026-10-19-lane-a-afc.md`](../decisions/2026-10-19-lane-a-afc.md) (Q1–Q4, Q7 owner chọn; Q5, Q6 điều phối chốt tạm).
**Phạm vi:** **A1** = lõi AFC, tham chiếu = đầu ra của chính làn (Q1-1). **A2** (ngõ vào tham chiếu riêng + mở engine > 2 kênh, Q1-2) là PR/release sau (Q5).
**Trạng thái:** **DỪNG 2026-10-19 — KHÔNG triển khai vào app.** Owner đặt ràng buộc cứng "không chấp nhận bất kỳ méo mó tiêu cực nào về âm thanh" (sổ, Q3 lật lại); mô phỏng không chứng minh được PEM-AFC đạt ràng buộc đó trên tín hiệu có màu (§9). Rev 2 giữ nguyên làm nghiên cứu; code ở `tools/afc/` (không biên dịch vào app). Trước đó: rev 2 sau phản biện vòng 1 (3 BLOCKER / 7 IMPORTANT, §10).
**Đụng audio path:** **có — nặng nhất của dự án.** Mức thay đổi: §6.
**Release:** 1.5.0 (`-Part minor`).

## 1. Vấn đề

Notch (lane G) trừ gain ở tần số đang hú: mỗi notch là một lỗ trên tiếng, và luôn
tới **sau** hú. AFC trừ **ước lượng của chính tín hiệu phản hồi** khỏi mic:
`e(n) = y(n) − ŷ(n)`, `ŷ = F̂ * u` (u = cái app gửi ra loa, `F̂` ≈ đường loa→phòng→mic).
Đúng thì vòng hở bị cắt mà không khoét tiếng — tài liệu báo +10..15 dB GBF. Sai
thì app **tự phát tiếng lạ ra PA**.

## 2. Quyết định đã chốt

| Mục | Quyết định | Sổ |
|---|---|---|
| Tham chiếu | A1: đầu ra của chính làn; A2 sau: ngõ vào riêng, chọn được | Q1, Q5 |
| Với notch | AFC đứng TRƯỚC chuỗi notch; detector + notch giữ nguyên làm lưới an toàn | Q2 |
| Giảm tương quan | chỉ PEM, trong nhánh học; không đổi tiếng ra loa | Q3 |
| Ra mắt | tắt mặc định, công tắc "AFC (thử nghiệm)" chỉ ở Geek, không lưu qua lần mở | Q4, Q6 |
| Trip / tắt giữa show | **hạ tạm làn đó 6 dB** (ramp 10 ms), giữ 5 s, nhả dần 5 s | Q7 |

## 3. Thiết kế DSP (`src/dsp/AfcCanceller.{h,cpp}`, chỉ `juce_dsp`)

### 3.1 Vị trí trong callback

Mỗi làn đang chạy, AFC bật, không Bypass, không có lần đo lane M:

```
y = guard(in[n]) ─► e = y − ŷ ─► guard(e) ─► notch chain ─► v ─► out[n] += duck·v
                         ▲                                │
                         └──── F̂  (học từ u = clamp(v)) ◄─┘
```

- `guard` = `isfinite` có sẵn trên `y` (giữ nguyên) + một guard mới trên `e` trước
  `chain.processSample`.
- Tham chiếu `u(n) = clamp(v(n), ±kMaxOutputLevel)` **của chính làn**: kênh ra chỉ
  xong sau khi mọi làn chạy, còn AFC cần `u` từng mẫu. Đúng cho cấu hình một làn
  một kênh ra (giới hạn §8).
- `duck` = hệ số hạ tạm của Q7 (1.0 khi không có sự kiện). Áp **sau** notch, lên
  đúng cái ra loa; vì vậy `u` (tham chiếu) lấy `v` *trước* duck nhân với `duck`
  — tức tham chiếu luôn là cái thật sự đi ra.

### 3.2 Nhân quả — vì sao không thêm độ trễ

AFC xử lý **từng mẫu** với bộ đếm partition (`P = 64`) **sống qua các callback**:
kích thước callback không liên quan tới toán. Tại biên partition `j` (mẫu `jP`),
`u(jP − 1)` đã được tạo ra (mẫu trước đó, cùng hay khác callback), nên
`X_{j−1} = FFT([u_{j−2}, u_{j−1}])` đã đủ. Vì `F̂` chỉ có tap `k ∈ [P, P + L)`, mọi
`u` cần cho `ŷ` của cả partition `j` đã có → `ŷ_j` tính một lần, **không thêm mẫu
trễ nào**.

Trễ vật lý của vòng chỉ quyết định cái **không được mô hình hóa**: nếu round-trip
thật < 64 mẫu (≈ 1,3 ms @ 48 kHz, 0,67 ms @ 96 kHz — gần như không thể với ASIO +
AD/DA + khoảng cách loa–mic), phần phản hồi dưới 64 mẫu không bị khử, **không gây
mất ổn định**; notch vẫn bắt.

`L = 8192` tap (170 ms @ 48 kHz; **85 ms @ 96 kHz**) → `K = 128` partition.

Worst case CPU theo callback: buffer 32 mẫu ⇒ cứ hai callback thì một callback mang
nguyên phần việc của một partition; benchmark đo đúng trường hợp này (§7).

### 3.3 Bộ lọc thích nghi: PBFDAF (MDF), overlap-save

**Quy ước FFT:** `juce::dsp::FFT` (order 7, N = 128) — forward không chuẩn hóa,
inverse nhân 1/N. Mọi năng lượng dưới đây tính trên phổ forward.

- Vòng đệm phổ tham chiếu `X_{j−p}` và bản làm trắng `X'_{j−p}`, `p = 0..K−1`.
- **Lọc:** `ŷ_j = nửa sau của IFFT( Σ_{p=0}^{K−1} W_p · X_{j−1−p} )`.
- **Học** (khi xong `e_j`): `E'_j = FFT([0_P, e'_j])`;
  `W_p += μ · conj(X'_{j−1−p}) · E'_j / (Σ_q |X'_{j−1−q}|² + δ_k)` — **chuẩn hóa
  bằng tổng công suất qua K partition** (B1: chia cho công suất một partition thì
  `1 − μK` ra ngoài (0, 2) và phân kỳ — đã tái hiện trong mô phỏng).
- `δ_k` = sàn **tương đối**: `10⁻⁴ ×` công suất dài hạn trung bình của bin đó (I3).
- **Không học** ở bin có công suất tham chiếu dưới `−40 dB` so với dài hạn (notch
  đang khoét, im lặng, mic mute — I3), và không học cả frame khi năng lượng
  `y` của frame dưới −70 dBFS.
- **Ràng buộc gradient** xoay vòng: mỗi partition ràng buộc một `W_p`
  (IFFT → xóa nửa sau → FFT), như Speex AUMDF; các partition chưa tới lượt có
  phần quấn vòng nhỏ, chấp nhận.
- Hằng số (`μ`, thời gian hội tụ) **lấy từ mô phỏng §7**, ghi số đo vào §9 khi có.
  Xuất phát: `μ = 0.1`.

### 3.4 PEM (Q3)

- Mô hình nguồn AR bậc 16 ước lượng trên `e` (chuẩn PEM-AFROW), Levinson–Durbin
  mỗi 20 ms trên khối 1024 mẫu có cửa sổ Hann. Tự tương quan cộng dồn theo
  partition; Levinson ~300 phép nhân, chạy trên audio thread.
- Chấp nhận `A` mới chỉ khi mọi hệ số phản xạ `|k_i| < 1` và mọi hệ số hữu hạn;
  ngược lại giữ `A` cũ. (`A(z)` là FIR nên luôn ổn định; điều kiện trên là để mô
  hình có nghĩa.)
- `A(z)` làm trắng **cả** `u` (→ `X'`) và `e` (→ `e'`), chỉ trong nhánh học. Lịch sử
  `X'` trải qua ~9 bản `A` khác nhau trên 170 ms — chấp nhận, ghi ở đây.
- **Giới hạn đã biết:** AR(16) không làm trắng cao độ (giọng hữu thanh, nhạc có
  nốt dài) → vẫn còn bias trên tín hiệu tonal. Không có bộ dự đoán dài hạn trong
  A1; rig là nơi đo thật. Mô phỏng dùng nguồn có cao độ biến thiên (§7), không chỉ
  AR thuần.

### 3.5 Watchdog (bắt buộc)

Ba điều kiện **trip cứng** trên mỗi làn:

1. **Không hữu hạn** ở `ŷ`, `e`, `W`, `A` → **chuyển về `y` ngay lập tức**, không
   crossfade (crossfade từ NaN ra NaN → guard ra 0 → cắt phựt).
2. **Phình theo bin** (I2): với phổ `|E_k|²`, `|Y_k|²` làm mượt (τ ≈ 100 ms), nếu ở
   bất kỳ bin nào trong 100 Hz – 8 kHz có `|E_k|²/|Y_k|² > +6 dB` liên tục 150 ms (và
   `|E_k|²` trên sàn nhiễu) → trip. Cần thêm 2 FFT/partition (`Y`, `E` không làm trắng).
3. **Hệ số vô lý**: năng lượng thời gian của `F̂` (Parseval: `Σ_p Σ_k |W_p,k|² / N`)
   > 4 (tức `‖F̂‖₂ > 2`). Chỉ bắt phân kỳ thô; bin 2 là cái bắt phân kỳ cục bộ.

Một điều kiện **trip mềm** (I2): khi AFC đang chạy trên một slot mà detector **đặt
notch mới** trên slot đó (lệnh `Set` đi qua hàng lệnh của engine, đếm trên audio
thread), đếm một lần; **2 lần trong 10 s** → trip. Lý do: hú lọt qua AFC — hoặc
do AFC sinh ra — là dấu hiệu AFC không giữ được vòng.

Khi trip (trừ điều 1): **crossfade `e → y` trong 10 ms** (notch nhìn thấy tín hiệu
đã crossfade), đồng thời **hạ tạm làn 6 dB** (Q7, §3.6), rồi xóa `W`, `X`, `X'`, `A`.
AFC của làn đó **nghỉ 5 s**, rồi bật lại từ zero. **3 trip trong 60 s** → khóa tắt
cho tới khi người vận hành gạt lại công tắc; GUI báo một dòng.

### 3.6 Hạ tạm 6 dB (Q7)

- Kích hoạt khi: trip (mọi loại), **gạt công tắc tắt AFC**, và khi AFC bị đóng
  băng vì lane M (đã có sweep là chuyện khác — xem §3.7).
- Hình dạng: ramp tuyến tính theo dB từ 0 xuống −6 dB trong 10 ms, giữ 5 s, ramp
  về 0 dB trong 5 s. Sự kiện mới trong lúc đang hạ: kéo dài phần giữ.
- Tắt công tắc AFC cũng **crossfade 10 ms** (B2), không bước nhảy `e → y`.
- Chỉ áp cho làn mà AFC đang chạy (hoặc vừa chạy) — làn không có AFC không đổi.

### 3.7 Tương tác

- **Lane M (`ĐO`)**: đóng băng theo **`scSuspendTaps_ || scCaptureActive_`** (B3 —
  `scOutChannel_` về −1 ở mỗi Gap), đọc một lần đầu callback như mười giá trị
  lane M khác. Khi bắt đầu đóng băng: crossfade về `y` + reset (không hạ tạm: người
  vận hành vừa xác nhận hạ master cho lần đo). Làn bị tắt tiếng vì sweep (`continue`
  sớm) cũng reset AFC tại chỗ đó.
- **Restart thiết bị** (`audioDeviceAboutToStart`): reset mọi AFC.
- **Đổi routing** (I6): `setSlotConfig` chạy trên message thread nên **không** reset
  trực tiếp; nó tăng một bộ đếm thế hệ per-slot (atomic) **chỉ khi config thật sự
  đổi**; audio thread thấy thế hệ đổi thì reset AFC của slot đó.
- **Detector**: tap vẫn ghi từ `out` (sau AFC + notch + duck) — detector thấy đúng
  cái ra loa.
- **Bypass**: AFC không chạy và reset; bật lại Auto thì học lại từ zero.

### 3.8 Bộ nhớ, CPU, giới hạn số làn

- Cấp phát khi engine dựng: mỗi làn `W`, `X`, `X'` (3 × 128 × 65 complex float) +
  đệm thời gian → ~210 KB/làn.
- **Giới hạn `kMaxAfcLanes = 4`** (I5 — 8 slot × 2 làn có thể là 16 làn trên 2 kênh):
  AFC chạy trên tối đa 4 làn đầu tiên theo thứ tự slot; làn còn lại chạy như không
  có AFC, GUI ghi rõ. Bộ nhớ: 4 × 210 KB.
- Mỗi partition: 7 FFT/IFFT 128 điểm (lọc 1, X 1, X' 1, E' 1, ràng buộc 2, watchdog Y/E
  2 − dùng chung 1) + 2 × 128 × 65 nhân phức ≈ 1,5 k nhân thực/mẫu → ~72 M/s mỗi làn
  @ 48 kHz, gấp đôi @ 96 kHz. 4 làn @ 96 kHz ≈ 0,6 G/s — benchmark quyết.
- `juce::dsp::FFT` engine fallback dùng `alloca` cho N = 128, không heap. Test ghim
  điều này (đếm cấp phát quanh `perform`), phòng khi bật IPP/FFTW sau này.

## 4. Điều khiển và GUI (Geek only)

- `AudioEngine::setAfcEnabled (bool)` — atomic toàn cục, mặc định **false**, không
  lưu (Q6). Tắt → crossfade + hạ tạm (§3.6).
- Geek: nút gạt `AFC (thử nghiệm)` trong dải DETECTION, kèm đèn: `TẮT` /
  `ĐANG HỌC (~20 s)` / `CHẠY` / `NGHỈ` (sau trip) / `KHÓA` (3 trip) / `GIỚI HẠN`
  (có làn vượt `kMaxAfcLanes`).
- Dumb: không có gì mới.
- Log: `afc_state` (bật/tắt), `afc_trip` (slot, lane, lý do 1/2/3/mềm).

## 5. Bất biến

1. AFC tắt (và không có duck đang chạy) ⇒ đầu ra **bit-identical** với bản trước.
2. Không cấp phát, không lock, không I/O trên audio thread.
3. Không thêm mẫu trễ nào; kết quả **bất biến theo cách chia callback** (cùng luồng
   vào qua khối 64, 37, 1, ngẫu nhiên → đầu ra giống hệt từng bit).
4. Không hữu hạn ⇒ về `y` ngay; kẹp ±`kMaxOutputLevel` + NaN guard có sẵn giữ nguyên.
5. Trip (trừ NaN) và tắt công tắc luôn crossfade 10 ms + hạ tạm 6 dB.
6. Notch/detector chạy như hôm nay, AFC bật hay tắt.
7. Lane M đang chạy (cả các Gap) ⇒ AFC đóng băng.

## 6. Mức thay đổi dự kiến (CLAUDE.md: nêu trước)

- **Mặc định (AFC tắt): 0 dB**, bit-identical (bất biến 1).
- **AFC bật, đã hội tụ**: thành phần phản hồi giảm theo độ lệch chỉnh đạt được;
  đường trực tiếp không đổi về kỳ vọng. Con số trên phòng thật **không đoán trước
  được** (phòng, mic di chuyển, fader/EQ đổi đường).
- **Lúc học (~20 s) / khi đường đổi**: có thể đổi màu tiếng vài dB.
- **Trip, tắt công tắc, khóa**: làn đó **−6 dB trong 5 s**, nhả trong 5 s (Q7).
- **Rủi ro đã biết (B2):** nếu người vận hành đã dùng headroom AFC cho (đẩy gain cao
  hơn biên cũ hơn 6 dB), thì sau khi AFC rời cuộc chơi, −6 dB **không đủ** và hệ
  thống có thể hú cho tới khi thang notch bắt kịp. Ghi vào tester notes: đừng dùng
  quá 6 dB headroom từ AFC.
- **Mỗi lần đo lane M**: AFC học lại từ zero sau đó.
- → **Bắt buộc nghe ở âm lượng thấp trước.**

## 7. Test

Mô phỏng vòng kín trong test (không thiết bị), tool `tools/AfcSim` để quét tham số:
- Nguồn: (a) nhiễu lọc formant + đường bao âm tiết; (b) như (a) + cao độ trượt
  110–180 Hz có vibrato; (c) hợp âm tĩnh (ca khó, chỉ đo watchdog, không đòi hội tụ).
- Phòng giả: trễ 300 mẫu + đuôi nhiễu suy giảm mũ 60 ms.
- Vòng: `y = s + F*u`, `e = AFC(y)`, `u = G·e`.
- Khẳng định (ngưỡng chốt theo số đo mô phỏng, ghi §9):
  - **MSG**: độ lợi ổn định lớn nhất tăng **≥ 6 dB** so với không AFC, nguồn (a) và (b).
  - **Độ lệch** đo trên tap `[64, 8256)`, giảm theo thời gian; ngưỡng ghi từ mô phỏng.
  - **Watchdog**: đổi `F` đột ngột → trip hoặc hội tụ lại, đầu ra không vượt kẹp;
    NaN → về `y` cùng mẫu; AFC-gây-hú gần 0 dB vòng → trip theo bin; 3 trip/60 s → khóa.
  - **Hạ tạm**: trip / tắt công tắc → −6 dB đúng hình dạng; không bước nhảy.
  - **Tắt = bit-identical** qua callback thật của `AudioEngine`.
  - **Bất biến chia khối** (64, 37, 1, ngẫu nhiên) bit-exact.
  - **Không thêm trễ** với `F` khác 0 đã hội tụ.
  - **Lane M**: đóng băng suốt lần chạy kể cả Gap.
  - **Tham chiếu im / bị notch khoét**: `W` không trôi ở bin không kích thích.
  - **Đổi routing**: reset qua bộ đếm thế hệ, không reset khi config không đổi.
  - **Trip mềm** từ lệnh `Set` của detector.
  - **Benchmark**: 4 làn @ 96 kHz, buffer 32, 10 s audio; đèn báo, không gate CI.
- GUI: ảnh `console-afc.png`, đọc lại.
- Rig: giọng thật, nhạc thật, mic cầm tay di chuyển, đổi fader nhanh.

## 8. Giới hạn đã biết (A1)

- Hai làn cùng một kênh ra: mỗi làn chỉ khử phần mình gửi ra.
- Kênh khác / nhạc nền / effect trộn vào loa không bị khử (A2).
- Đường mixer đổi nhanh = AFC phải học lại; watchdog + notch + hạ tạm che khoảng đó.
- Phản hồi trễ hơn `P + L` (> 171 ms @ 48 kHz, > 86 ms @ 96 kHz) không được mô hình hóa.
- Tín hiệu tonal (nốt dài, hợp âm) để lại bias (§3.4).
- Tối đa 4 làn có AFC.

## 9. Số đo mô phỏng (2026-10-19, `tools/afc/afcsim.cpp`)

Vòng kín, phòng giả trễ 300 mẫu + đuôi 60 ms, 48 kHz, PEM bậc 32 (16 không đủ ở
48 kHz: sai số dải formant +6,7 dB → +0,6 dB khi lên 32).

| Cấu hình | MSG | SNR e/s ở −3 dB (nền 13,5 dB) |
|---|---|---|
| Vòng hở, nhiễu trắng | độ lệch −57 dB (không PEM), −18 dB (PEM) | — |
| Vòng kín, nguồn trắng, μ 0.005 | +11 dB (nền +1) | 16,9 dB |
| Vòng kín, nguồn formant, μ 0.1 | +13 dB "ổn định" | **6 dB — tiếng hỏng** |
| Vòng kín, nguồn formant, μ 0.005–0.02 | +3..+5 dB | 7–10 dB (xấu hơn không AFC) |
| Vòng kín, μ 0.002, học 60 s, 4 nguồn (formant, giọng, hợp âm) | +3 dB | 15,7 dB — **cùng con số tới 0,1 dB cho mọi nguồn**, độ lệch +0,1 dB: bộ lọc gần như không học |
| Dịch tần 5 Hz + AFC | +7 dB | 20,8 dB — **bị loại**: đổi tiếng |

Đọc: lõi đúng (vòng hở, nguồn trắng). Trên tín hiệu có màu, nguồn đóng vai nhiễu
mạnh hơn phản hồi ~17 dB và PEM không khử hết bias; bước học đủ lớn để có lợi thì
làm méo, bước đủ nhỏ để không méo thì gần như không học. Không đạt ràng buộc owner.

## 10. Phản biện vòng 1 → rev 2

| # | Finding | Xử lý |
|---|---|---|
| B1 | chuẩn hóa NLMS theo một partition ⇒ phân kỳ | §3.3: chia tổng công suất K partition (tái hiện bằng mô phỏng) |
| B2 | trip/tắt trả hệ thống về vượt biên | Q7 owner: hạ tạm 6 dB; §3.6; §6 nêu rủi ro còn lại |
| B3 | đóng băng theo `scOutChannel_` hở ở mỗi Gap | §3.7: `scSuspendTaps_ || scCaptureActive_` |
| I1 | lập luận nhân quả sai lý do | §3.2 viết lại; test bất biến chia khối |
| I2 | watchdog băng rộng không thấy hú do AFC | §3.5: theo bin + trip mềm; NaN về `y` ngay; guard trên `e` |
| I3 | notch khoét / im lặng ⇒ W trôi | §3.3: sàn tương đối, không học ở bin/khung thiếu kích thích |
| I4 | PEM: "A không ổn định" vô nghĩa; thiếu dự đoán dài hạn | §3.4: kiểm `|k_i|<1`; nêu giới hạn; nguồn có cao độ trong mô phỏng |
| I5 | 16 làn, đếm FFT sai | §3.8: `kMaxAfcLanes = 4`, đếm lại, benchmark 96 kHz/32 |
| I6 | reset từ message thread = race | §3.7: bộ đếm thế hệ per-slot, chỉ khi config đổi |
| I7 | câu mức thiếu B2 / lane M / tắt công tắc | §6 viết lại |
| M1–M5 | quấn vòng, Parseval, crossfade qua notch, tham chiếu per-lane, 96 kHz | §3.3, §3.5, §3.1, §8 |
