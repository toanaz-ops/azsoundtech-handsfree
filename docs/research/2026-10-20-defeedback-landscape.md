# Khảo sát sản phẩm & nghiên cứu chống hú 2023–2026 — Hands-free nên lấy gì

**Ngày:** 2026-10-20. **Người nghiên cứu:** Claude (sub-agent), đọc-chỉ, không sửa repo.
**Đã đọc trước để không nghiên cứu lại:** `docs/research/2026-09-04-anti-feedback-next-gen.md`,
`docs/research/2026-09-06-cedar-realtime-plugins.md`, `memory/anti-feedback-research-2026-09-04.md`,
`memory/lane-a-afc-stopped-2026-10-19.md`, `docs/superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md`.
**Ràng buộc cứng của owner:** không chấp nhận bất kỳ méo/đổi tiếng nào sau Hands-free (lane A dừng 19/10/2026 vì lý do này).

## 0. Giới hạn của nghiên cứu này (đọc trước)

- Proxy của phiên này **chặn tải trực tiếp** mọi trang ngoài GitHub (prosoundweb, soundonsound,
  arxiv, ieee, drewbrashler… đều `connect_rejected`). Mọi thông tin web dưới đây lấy từ **bản tóm
  tắt của công cụ tìm kiếm** trên các trang đó, không phải đọc toàn văn. Con số đã được ghi đúng như
  nguồn nêu, nhưng **cần kiểm lại trên toàn văn** trước khi đưa vào spec.
- Ký hiệu chất lượng bằng chứng: **[vendor]** = nhà sản xuất tự nói; **[báo]** = báo ngành/review;
  **[đo độc lập]** = người dùng/reviewer đo; **[forum]** = giai thoại; **[paper]** = bài khoa học;
  **[patent]**; **[code]** = đã đọc mã nguồn.
- "Antares" trong đề bài: **không tìm thấy** bằng chứng Alpha Labs/De-Feedback liên quan tới Antares
  (Auto-Tune). Alpha Labs LLC tách ra từ **Alpha Sound** (công ty cho thuê PA ở Pacific Northwest).
- "REA repo" được hiểu là **`toanaz-ops/rta-tool`** (repo public gần nhất trong tài khoản owner) —
  xem §6.

## 1. Bảng tổng hợp

| Sản phẩm / kỹ thuật | Phát hiện | Khử | GBF thêm (nguồn) | Trễ | Đổi tiếng? | Bằng chứng |
|---|---|---|---|---|---|---|
| **Alpha Labs De-Feedback V1** (11/2024) | không "dò hú": mạng nơ-ron học giọng người, coi mọi thứ không phải giọng trực tiếp là thứ cần trừ | tách nguồn liên tục (trừ feedback + reverb + noise), 1 nút Strength 0–100 % | ~10 dB (blog đo bằng Smaart; Reddit) | plugin "0" [vendor]; hệ thống 4,83–4,9 ms @48k buffer 16 [đo độc lập] | **Có, theo thiết kế**: bỏ reverb/ambience; "phá" nhạc cụ (tambourine biến mất khỏi mic vox) | vendor + báo + đo độc lập ít; không có paper |
| **Clear Voice Live** (beta 0.9.7, 2026) | AI, không notch/không GEQ | tách nguồn, 2 engine SOFT/HARD, GPU DirectML | "đẩy được cao hơn", "không 100 %" [forum] | không rõ | có báo cáo "boxiness" ở mức vừa [forum] | chỉ forum, không trang vendor |
| **Waves X-FDBK** (2016) | dò sine "thuần" | ≤ 20 notch hẹp, tự chọn độ rộng & độ sâu; SETUP tự thoát sau 1 phút; nút ADD giữ-để-học | không công bố số | không công bố | notch hẹp; reviewer: SENSE cao → cắt quá sâu, quá nhiều, có lúc cắt vào giọng | báo |
| **Waves Feedback Hunter** (01/2024) | phát white noise, đo mic↔loa trước show | dựng **đường EQ toàn dải** (không phải notch đơn), Depth + Frequency Brackets | không công bố; "30 s" [vendor] | – | **có** (EQ toàn dải) — dùng trước show | vendor/báo |
| **dbx AFS / AFS2** (Harman; cả Soundcraft Ui, DriveRack PA2) | "Precision Frequency Detection" | 24 notch/kênh, Q tới **1/80 oct**, băng thông thích nghi, **Fixed** (khóa sau setup) + **Live** (tự nhả, tái dùng cái cũ nhất), preset Speech/Music, "virtual HPF" | SOS: "ấn tượng", không số | – | ít (notch rất hẹp) | vendor + SOS review |
| **Sabine FBX** (US 5,245,665) | FFT; đỉnh lớn hơn **hài/hạ hài của chính nó ≥ 33 dB** qua nhiều phổ liên tiếp ⇒ hú ("feedback ít hài, nhạc/giọng nhiều hài") | notch constant-Q, 1/10 hoặc 1/5 oct (FBX-SOLO), độ phân giải 1/50 oct; Fixed + Live | – | – | notch rộng hơn dbx ⇒ nhuộm màu hơn | patent (qua aggregator) + brochure |
| **Behringer FBQ2496** | FFT, phát hiện "> 0,2 s" | ≤ 20/40 notch, 1/60 oct; Auto / Single-shot (khóa) / Manual | – | – | ít | retailer spec |
| **Klark Teknik DF1000** | tự động | ≤ 32 notch/kênh, 1/30 oct, áp trong 2–4 s | "tới 10 dB" [vendor] | – | ít | vendor/retailer; ngừng bán |
| **Shure DFR22** (ngừng) | notch thích nghi (patent Shure) | ≤ 16 notch/khối, 1/10–1/70 oct, Auto Clear | – | – | ít | retailer |
| **Yamaha** EMX5/7; DXR mkIII | – | EMX: 7 notch, 1 nút; DXR: **DYNAMIC** (mic cầm tay) vs **FIXED** (mic cố định, dò trước) | – | – | ít | vendor |
| **Q-SYS NFC** | để hú bắt đầu rồi dò | notch Fixed + Dynamic; gợi ý ngưỡng ≈ −46 dB, 6 fixed, 0 dynamic | – | – | ít | Q-SYS help |
| **Biamp Tesira Feedback Suppressor** | floating bands | ≤ 16 band, Max Depth/Float Limits; workflow: ring-out, **fix band rồi chép sang PEQ** | – | – | ít | Biamp help |
| **Allen & Heath AHM** | – | firmware ~10/2025 thêm "integrated feedback suppression" | – | – | – | báo (chưa xác minh chi tiết) |
| **Sennheiser TruVoicelift** (TeamConnect Ceiling 2) | ngưỡng | **frequency shifter vài Hz** (+3…6 dB) + **Feedback Prevent Mute** (tắt mic vài giây) | tổng +6…9 dB [vendor] | – | **có** (dịch tần; mute nghe được) | vendor |
| **GN ReSound DFS Ultra** (máy trợ thính) | đo đường feedback lúc fitting (lọc tĩnh) + lọc động | khử pha ngược (AFC) + "Whistle Control" = EQ thay vì giảm gain | – | rất thấp | phiên bản cũ: noise tiêm vào nghe được; **nhạc khó phân biệt với hú** [vendor tự nhận] | vendor |
| **CEDAR DNS/StageVox/Voxis**, **Waves Clarity Vx** (trễ 2048 sample @48k [báo]), **iZotope Dialogue Isolate** | – | giảm noise/reverb/bleed ⇒ GBF gián tiếp | – | 14 sample (CEDAR Quantum) … 10 ms (Voxis) … 43 ms (Clarity Vx) | **có** (đó là mục đích) | đã nghiên cứu 06/09 |
| **DiGiCo, BSS Soundweb London, Klark Teknik DN-series, Behringer WING** | — | **không tìm thấy** tài liệu suppressor tích hợp qua tìm kiếm; không suy đoán | | | | |

**Mẫu số chung:** mọi thứ cho GBF vượt vùng của notch (dịch tần, AFC, tách nguồn ML) đều **đổi tín
hiệu**. Các sản phẩm "không đổi tiếng" đều là họ **notch hẹp** và khác nhau ở: độ chính xác tần số,
độ rộng, số notch, cách phân biệt nhạc, Fixed/Live, cách nhả. Đó chính là trục cạnh tranh còn lại
của Hands-free dưới ràng buộc của owner.

## 2. Alpha Labs De-Feedback — chi tiết (đối thủ "AI" đáng chú ý nhất)

- **Cơ chế** (theo reviewer và một ghi chú GitHub bên thứ ba, KHÔNG phải tài liệu vendor): mạng nơ-ron
  huấn luyện trên giọng người (vài TB dữ liệu tự sinh từ IR venue), làm **tách nguồn liên tục**:
  ước lượng phần "không phải giọng trực tiếp" (feedback, reverb, noise) rồi trừ đi — một reviewer mô tả
  là "đảo cực phần không phải giọng và cộng lại". Dải 100 Hz–22 kHz, 44,1–96 kHz, mono/stereo.
- **GBF:** ~10 dB [đo độc lập, blog nhà thờ, Smaart] và ~10 dB [forum, Reddit 03/2026]. Vendor FAQ tự
  nhận: mic gần loa < ~6 ft thì kém đi; đẩy gain đủ cao vẫn "đánh bại" được thuật toán.
- **Trễ:** plugin 0 [vendor]; Reddit đo bằng Smaart 0,00–0,02 ms; toàn hệ 4,83 ms @48k buffer 16
  [đo độc lập]. Phần cứng: NUC + Focusrite theo cấu hình vendor kiểm định; 1 instance/kênh; Fourier
  transform.engine chạy 6 instance (06/2026) [vendor].
- **Tone:** đáp tuyến EQ gần như không đổi [đo độc lập — Smaart], nhưng **nội dung bị đổi**: bỏ
  reverb/noise là mục đích; nhạc cụ trong mic vox bị xóa (tambourine) [báo Live Sound 05/2026];
  reviewer thấy 100 % quá nặng, chọn 70 %. Bị phê là **hộp đen** (không cho thấy đã trừ gì).
- **Kết luận cho Hands-free:** vi phạm ràng buộc owner theo định nghĩa (đổi tín hiệu kể cả khi không
  hú; hỏng nhạc cụ). Không chép kiến trúc. Hai bài học lấy được: (1) **không để "nghĩa địa notch"
  tích tụ** — đó là điểm De-Feedback được khen; Hands-free đã có nhả dần (lane G), nên đo và hiển thị
  "chi phí tone" hiện hành; (2) **minh bạch** là điểm yếu của đối thủ AI — Hands-free nên khai thác.

## 3. "isovoice"

**Không tìm thấy sản phẩm nào tên "isovoice"/"IsoVoice"/"Iso Voice"** (3 lượt tìm, cả web và các
cụm "AI vocal", "anti-feedback"; tìm GitHub bị proxy chặn). Những thứ gần tên hoặc gần chức năng:

- **ISOVOX IsoPlug** — channel strip cho voice-over (EQ, de-esser, comp, reverb), không AI, không
  chống hú; ISOVOX vốn là hãng **buồng thu di động**.
- Nếu owner nghĩ tới một **voice isolator chạy live**: CEDAR **Voxis** (DNN < 10 ms, đã nghiên cứu
  06/09), Alpha Labs **De-Feedback** (thực chất là vocal isolator), **Clear Voice Live** (beta AI).

Cần owner cho link hoặc tên hãng để tìm tiếp; không đoán.

## 4. Nghiên cứu học thuật (2010 → 2026)

| Công trình | Nội dung | Liên quan |
|---|---|---|
| van Waterschoot & Moonen, *Comparative evaluation of howling detection criteria in NHS*, JAES 58(11), 2010 [paper] | Khung thống nhất cho NHS (notch-filter howling suppression): tiêu chí **PAPR, PHPR (peak-to-harmonic), PNPR (peak-to-neighbour), IPMP (persistence), IMSD (độ đều của dốc tăng giữa các frame)**. Một tiêu chí đơn lẻ có **tỉ lệ báo động giả trên nhạc tới 33 %**; kết hợp nhiều tiêu chí xuống **3 %**. Ví dụ ngưỡng trong file demo: PHPR 36/44 dB, PNPR 12 dB, IMSD 0,5/0,1 dB | Hands-free hiện có ~PNPR (peakiness), ~IPMP (persistence), rise; **thiếu PHPR theo nghĩa nội dung chương trình và IMSD** |
| Sabine US 5,245,665 [patent] | đỉnh ≥ 33 dB trên hài/hạ hài của nó trong nhiều phổ liên tiếp | PHPR nguyên bản |
| US 11,722,822 (ưu tiên 2020) [patent] | kiểm "thiếu hài" ở −20/−30 dB so với tone chính | cùng ý |
| Li et al., *Sensors* 21(23):8062, 2021 [paper] | notch FIR dạng đóng, pha tuyến tính, có phân đoạn | không cần: biquad IIR của Hands-free đã ≈0 ms trễ |
| Zhang/Yu/Yu (Tencent), **Deep AHS**, ICASSP 2023; **Hybrid AHS** (arXiv 2305.02583); **NeuralKalmanAHS** (arXiv 2309.16049); huấn luyện đệ quy (arXiv 2309.16048) [paper] | mạng hồi quy **tái tạo giọng đích** từ tín hiệu mic (tức là tách nguồn), hoặc Kalman + mạng | chỉ speech; tái tạo = đổi tín hiệu ⇒ **không hợp ràng buộc** |
| DeepPEM-AFC (Zhan et al., 2025) [paper]; *In-the-loop training of deep feedback cancellation for hearing aids* (arXiv 2606.03832, 2026) [paper] | PEM-AFC + DL; DFC huấn luyện vòng hở **mất ổn định khi gain cao**, phải huấn luyện trong vòng | củng cố bài học lane A: vòng kín khác vòng hở |
| CRN cho feedback đa kênh (arXiv 2505.15914, 2025) [paper] | mạng CRN giảm độ phức tạp | máy trợ thính/hội nghị |
| **Phase synthesizer** cho decorrelation (Linhard & Bulling, arXiv 2510.12377, 2025) + tổ hợp decorrelation (arXiv 2602.06921, 2026) [paper] | dịch tần + điều chế pha bằng **variable delay line** trong filter bank, tự nhận "perceptually transparent", đo bằng **PESQ** trên **speech trong xe** | chỉ đúng điều kiện mở lại lane A ghi trong roadmap ("khử tương quan không đổi tiếng") **về hình thức**, nhưng PESQ trên speech ≠ "không méo" trên nhạc/hát. Theo dõi, không làm |
| Graph-based temporal anomaly detection cho AHS (ScienceDirect, 2026) [paper] | phát hiện hú như bất thường thời gian, tự giám sát, ngưỡng động, lọc < 10 ms | ý tưởng cho lane C (không cần nhãn); chưa đọc toàn văn |
| IEEE SPS webinar *Neural AFC* (06/2025) | tổng quan | đã ghi 04/09 |

## 5. Mã nguồn mở

- **Không tìm thấy** một NHS (dò + notch) mã nguồn mở đáng dùng cho live sound qua tìm kiếm (tìm
  GitHub trực tiếp bị chặn ở phiên này). Thứ có thật là **AFC cho máy trợ thính**:
  - **openMHA** (HörTech, AGPL) — `examples/31-adaptive-feedback-canceller`, `08-feedback-suppression`
    (PEM/NLMS, có ví dụ live + calibrate) [code: đã liệt kê cây file].
  - **Tympan_Library** (MIT) — `AudioFeedbackCancelNLMS_F32`, `AudioFeedbackCancelNFXLMS_F32`
    (kèm white paper Neely 2022) [code: đã liệt kê cây file].
  - **rogaits/afc-nr** — MATLAB, GPL-3.0, đa mic.
  Cả ba chỉ phục vụ lane A nếu mở lại; không đổi kết luận dừng.
- Plugin thương mại nhỏ thấy trong kết quả: *Howly Shit* (bitscrew), *De-Feed DF-1* (≤ 8 notch),
  *FeedbackGuard* (AU, "PNPR + kiểm tăng trưởng + 24 notch") — chỉ mô tả marketing.

## 6. "REA" → `toanaz-ops/rta-tool` (clone đọc-chỉ, HEAD `17f3010`)

RTA dual-FFT của chính owner, `rta_core` là C++20 **không phụ thuộc JUCE**, test bằng golden vector
NumPy/SciPy. **Giấy phép AGPL-3.0-or-later** (186 file `SPDX: AGPL-3.0-or-later`); README nói
`rta_core` có thể relicense độc lập. Grep "borrowed/ported/derived from" không thấy file nào chép từ
Open Sound Meter (chỉ so sánh). Nếu owner là tác giả duy nhất thì relicense được; **phải xác nhận
trước khi chép sang Hands-free (sản phẩm thương mại)**.

Đáng dùng lại cho Hands-free:

| Module rta-tool | Hands-free đang có | Lợi ích |
|---|---|---|
| `dsp/TransferEstimator.h` (H1/H2/Hv + **coherence γ²**), `dsp/DualFftEngine.h` | `LoopGainEstimator` = **tỉ số năng lượng, chỉ biên độ, không pha, không coherence** | có **pha** vòng hở ⇒ dùng điều kiện Nyquist (|H| ≥ 1 **và** pha ≡ 0 mod 360°) để chỉ đặt notch phòng ngừa ở nơi thật sự sẽ hú; γ² loại bin bị noise phòng chi phối |
| `gen/Sweep.h` (Farina ESS) + `ir/Deconvolver.h` | sweep log, nhưng ước lượng theo năng lượng bin | IR thật ⇒ tách méo hài loa khỏi đáp tuyến tuyến tính; đo được đuôi vang dài (khắc lỗi "under-read" mà `LoopGainEstimator.h` tự ghi) |
| `dsp/DelayFinder.h` (GCC-PHAT, sub-sample) | không đo trễ vòng | trễ vòng τ ⇒ các tần số hú tiềm năng cách nhau 1/τ — ràng buộc hữu ích cho detector & cho lane C |
| `ir/Decay*.h` (RT60, Lundeby) | – | đặc trưng phòng cho "venue memory" |
| `dsp/MtwEngine.h` (multi-time-window) | FFT 2048 cố định, bin 23,4 Hz | độ phân giải tần số cao ở LF cho **soundcheck** (không phải detector live) |

Không đáng dùng: phần UI/platform (Hands-free đã có Sodium Rack + bridge riêng).

## 7. Phát hiện từ code Hands-free trong lúc đối chiếu (quan trọng)

**Tần số notch = tâm bin, không nội suy dưới-bin.** `PeakinessAnalyzer.cpp:144`:
`candidate.frequencyHz = bin * sampleRate / kFftSize`; `NotchController.cpp:1207` đặt notch đúng tần
số đó với Q mặc định 30. Bin @48k = 23,4 Hz ⇒ sai số tới ±11,7 Hz. Ghi chú ở
`NotchController.cpp:1234-1239` lập luận "Q 30 ở 1 kHz rộng ~33 Hz > 1 bin" — đúng ở 1 kHz, **sai ở
dải thấp-trung**, nơi hú monitor hay xảy ra. Tính bằng RBJ peaking −18 dB, Q 30, fs 48k (độ suy tại
tần số hú thật khi tâm notch lệch δ):

| f0 | δ = 0 | 3 Hz | 6 Hz | 11,7 Hz (nửa bin) |
|---|---|---|---|---|
| 150 Hz | −18,0 | −7,8 | −3,8 | **−1,4** |
| 300 Hz | −18,0 | −12,4 | −7,8 | **−3,9** |
| 600 Hz | −18,0 | −15,7 | −12,4 | −8,0 |
| 1,2 kHz | −18,0 | −17,3 | −15,7 | −12,5 |
| 3 kHz | −18,0 | −17,9 | −17,5 | −16,4 |

Hệ quả có thể có (chưa đo trên rig): dưới ~600 Hz thang lane G "đào" sâu hơn cần thiết hoặc hú tái
phát ngay cạnh, tức là **tốn tone nhiều hơn cho cùng một mức chặn** — đúng thứ owner cấm. Các
sản phẩm notch tốt (dbx 1/80 oct, Behringer 1/60) chỉ hẹp được vì đo tần số chính xác hơn 1 bin.

## 8. Khuyến nghị cho codebase này (xếp ưu tiên)

Tất cả đều nằm trong họ "notch + đo", không đổi tiếng khi không có hú. Mỗi mục đụng audio path đã
nêu mức thay đổi level dự kiến theo CLAUDE.md.

### P1 — Tần số dưới-bin cho notch (lane mới "F", hoặc gắn G) — **làm trước**
- **Việc:** ước lượng tần số đỉnh chính xác: nội suy parabol trên log-biên độ quanh bin (Hann: sai số
  còn vài % bin) và/hoặc **phase-vocoder** (chênh pha bin giữa hai hop 512 mẫu liên tiếp ⇒ sai số cỡ
  < 1 Hz với tone ổn định). Dùng giá trị này cho `setNotchImpl`, cho bộ nhớ phòng (`takeRememberedDepthLocked`
  so theo bin — giữ khóa bin, lưu Hz chính xác), và sửa ghi chú `NotchController.cpp:1234`.
- **Level dự kiến:** ngoài tần số hú: không đổi (cùng Q, cùng depth). Tại tần số hú: suy thật tiến về
  đúng depth đã đặt (ví dụ 300 Hz: từ −3,9…−18 tùy độ lệch → ≈ −18 dB). Có thể làm thang G dừng ở bậc
  nông hơn ⇒ ít tone bị cắt hơn.
- **Công sức:** S–M (1 task DSP + test headless với tone lệch bin, ví dụ 311,7 Hz; 1 task log
  `freq_hz` chính xác). **Dữ liệu/phần cứng:** không cần để làm; nghe xác nhận trên rig ở alpha.
- **Rủi ro:** tone đang trượt (vibrato) cho ước lượng nhiễu ⇒ chỉ dùng khi persistence đã xác nhận,
  kẹp |Δf| ≤ 0,5 bin, NaN-guard.

### P2 — Notch hẹp hơn, độ rộng thích nghi (sau P1)
- **Việc:** khi tần số đã chính xác, đặt Q theo phần-octave (ví dụ khởi đầu ~1/40–1/60 oct như
  Behringer/dbx, tức Q ≈ 58–87) thay vì Q 30 cố định; **chỉ nới rộng** khi hú tái phát cạnh tâm hoặc
  tần số trôi (kiểu "adaptive filter bandwidth" của dbx). Gộp hai notch kề nhau thành một.
- **Level dự kiến:** tại tâm không đổi; dải lân cận bị cắt **ít hơn** (băng thông −3 dB hẹp lại
  ~2–3 lần) ⇒ chi phí tone giảm.
- **Công sức:** M. Phải kiểm ổn định số của biquad Q cao ở LF trong float (rta-tool có
  `BiquadResponse.h` để kiểm đáp tuyến), và ramp 10 ms của lane G vẫn là tổ hợp lồi khi đổi Q
  (hiện ramp chỉ cho depth; đổi Q đang là "design mới" ⇒ reset — cần thiết kế).
- **Dữ liệu:** log lane D (tỉ lệ hú tái phát cạnh notch) để chọn Q khởi đầu.

### P3 — Đặc trưng phân biệt nhạc theo tài liệu NHS (detector, không đụng audio path)
- **Việc:** thêm hai đặc trưng vào `CandidateScorer`: **PHPR** = đỉnh so với năng lượng ở 2f, 3f,
  f/2 *của chính nó* (nốt nhạc/giọng có hài ⇒ phạt; hú gần sine ⇒ không phạt — nguyên lý Sabine) và
  **IMSD** = độ đều của dốc tăng dB qua các frame (hú tăng tuyến tính theo dB, nốt nhạc thì không).
  Lưu ý: phạt harmonic hiện tại chỉ áp khi ứng viên nằm ở 1,4–4,1× một **notch đã khóa** — khác PHPR.
  Ghi chú đơn vị (bài học lane M): PHPR/IMSD là dB, `kConfirmScore` là tích 0..1.
- **Lợi ích có bằng chứng:** van Waterschoot 2010 đo báo động giả trên nhạc 33 % → 3 % khi kết hợp
  nhiều tiêu chí [paper].
- **Rủi ro:** hú ở mức rất lớn qua loa bị méo sẽ có hài ⇒ PHPR phải dùng như **trọng số mềm**, không
  phải cổng; bỏ qua khi rise rất dốc (`kSteepRiseRatio`).
- **Công sức:** M. **Dữ liệu:** bộ ghi nhạc thật + replay harness offline trên log/ghi âm; nhãn
  GOOD/FALSE lane D để đo tỉ lệ báo động giả trước/sau. Không cần rig để phát triển.

### P4 — Soundcheck v2: hàm truyền phức + coherence + trễ vòng (mở rộng lane M, dùng lại rta-tool)
- **Việc:** thay ước lượng năng lượng bằng H1 + γ² (`TransferEstimator`, `DualFftEngine`), IR qua
  ESS + Farina (`Sweep`, `Deconvolver`), trễ vòng qua GCC-PHAT (`DelayFinder`). Đề xuất notch phòng
  ngừa chỉ ở bin có biên độ gần 0 dB **và** pha vòng gần 0° mod 360°, bin có γ² thấp thì không đề xuất.
  Xuất trễ vòng τ cho detector (ưu tiên ứng viên gần lưới k/τ).
- **Lợi ích:** ít notch phòng ngừa hơn và đặt đúng hơn ⇒ ít tone bị cắt; vá lỗi "đuôi dài bị
  under-read" mà `LoopGainEstimator.h` tự ghi.
- **Level dự kiến:** sweep giữ nguyên giới hạn −20…−40 dBFS (`kSoundcheckMaxPeak`); notch đề xuất
  ít/không sâu hơn hiện tại.
- **Công sức:** M–L. **Điều kiện:** owner xác nhận relicense phần `rta_core` dùng lại (AGPL) — nếu
  không, viết lại từ công thức (H1, γ², ESS đều là công thức công khai). **Phần cứng:** rig thật (pha
  vòng không mô phỏng được đáng tin). Đây cũng là nền đo nếu lane A mở lại.
- **Ranh giới thư viện:** H1/coherence/ESS/GCC-PHAT là ứng viên thư viện dùng chung (RTA và
  Hands-free cùng dùng — đúng điều kiện "có sản phẩm thứ hai thật" của CLAUDE.md).

### P5 — Fixed vs Live rõ ràng + xuất sang PEQ của console (không đụng audio path)
- **Việc:** chuẩn hóa ngữ nghĩa mọi đối thủ dùng: notch từ soundcheck = **Fixed** (không tự nhả),
  notch trong show = **Live** (nhả dần như lane G). Thêm **"Export fixed notches"** (CSV/text: Hz, Q,
  dB) để soundman chép sang PEQ console — workflow Biamp khuyên.
- **Công sức:** S. **Dữ liệu:** không.

### P6 — Minh bạch & "chi phí tone" (UI, không đụng audio path)
- **Việc:** hiện tổng "tone cost" đang áp (ví dụ Σ |depth| × băng thông theo octave) và đường đáp tuyến
  tổng của chuỗi notch trên spectrum; ở Dumb mode một chỉ số đơn. Đây là điểm De-Feedback bị phê (hộp
  đen) và là bằng chứng cho lời hứa "không đổi tiếng".
- **Thêm:** in **trễ thêm của app tính bằng sample** (bài học CEDAR 06/09, vẫn chưa làm).
- **Công sức:** S–M; cần ảnh `HandsFreeSnapshot`.

### P7 — Kiểu điều khiển học từ X-FDBK cho Dumb mode
- **Việc:** nút **"LEARN"/ADD giữ-để-học** và chế độ setup **tự thoát sau N giây** (X-FDBK tự thoát sau
  1 phút để không ai quên bật trong show). Không đổi DSP.
- **Công sức:** S.

### P8 — Lane C (classifier) giữ nguyên hướng, nạp đặc trưng mới
- **Việc:** đầu vào = peakiness, rise, persistence, **PHPR, IMSD (P3)**, bất đối xứng L/R (S), khoảng
  cách tới lưới 1/τ (P4). Mạng rất nhỏ (RTNeural) trên detector thread, chỉ ra quyết định.
  Hướng tự giám sát kiểu "anomaly detection" (paper 2026) đáng thử nếu nhãn lane D về chậm.
- **Công sức:** L. **Dữ liệu:** ≥ 300 verdict từ ≥ 3 session như roadmap; P3 làm trước để có baseline
  luật cứng tốt nhất trước khi so với ML.

### Không khuyến nghị (vi phạm ràng buộc hoặc sai tầng)
- **Tách nguồn ML kiểu De-Feedback / Clear Voice Live / Deep AHS** trong Hands-free: đổi tín hiệu kể cả
  khi không hú, hỏng nhạc cụ, CPU/1 instance mỗi kênh. Nếu làm thì là sản phẩm voice isolator riêng
  (quyết định owner 06/09 §7).
- **Frequency shift (Sennheiser TruVoicelift), Feedback Prevent Mute**: đổi tiếng / tắt tiếng nghe được.
- **Feedback Hunter-style EQ toàn dải** tự động: đổi tone toàn dải; chỉ chấp nhận được nếu là đề xuất
  người dùng tự áp, không mặc định.
- **Decorrelation bằng phase synthesizer** để mở lại lane A: chỉ có bằng chứng PESQ trên speech; cần bộ
  đo "không méo" trên ghi âm rig như roadmap đã nêu trước khi bàn.

## Nguồn

Sản phẩm
- De-Feedback: https://www.alphalabsaudio.com/defeedback/ · https://drewbrashler.com/2026/alpha-labs-defeedback-demo-latency-testing/ · https://newsletter.drewbrashler.com/p/alpha-labs-defeedback-testing-results · https://www.sonicstate.com/news/2026/01/21/de-feedback-plugin-for-live-sound-/ · https://www.prosoundweb.com/the-secret-sauce-an-in-depth-look-at-the-groundbreaking-de-feedback-plugin/ · https://digital.livesoundint.com/publication/?i=865372&article_id=5159541&view=articleBrowser · https://fourieraudio.com/news/alpha-labs-brings-de-feedback-to-fourier-audios-transform-engine · https://community.gigperformer.com/t/anyone-used-de-feedback-plugin-from-alpha-labs/26496 · https://github.com/skrinsky/live/blob/main/de-feedback-ai-live-sound.md (ghi chú bên thứ ba, đã đọc toàn văn)
- Clear Voice Live: https://forums.allen-heath.com/t/what-s-your-sq-de-feedback-clear-voice-live-setup-and-experience/32477 · https://reddit.sentinel-team.org/posts/1titbpg/snapshots/2026-05-21T19%3A21%3A40.10629Z
- Waves: https://www.waves.com/2-ways-to-fight-feedback · https://digital.livesoundint.com/articles/waves-audio-x-fdbk · https://fohonline.com/?p=127222 · https://sonicstate.com/news/2024/01/11/waves-ships-the-feedback-hunter/ · https://www.audiotechnology.com/by-brand/waves/waves-plugin-hunts-down-feedback · https://www.mixonline.com/technology/news-products/waves-clarity-vx-live-product-of-the-week
- dbx/Harman: https://www.soundonsound.com/reviews/dbx-afs2 · https://harman.helpjuice.com/afs2/afs-how-it-works · https://pro.harman.com/insights/education/what-is-feedback-suppression-and-how-can-it-help-your-performances/
- Sabine: https://9235.net/patent/US5245665A.html?ds=us&p=1 · https://www.fullcompass.com/common/files/4358-FBXSeriesBrochure.pdf · https://cf3.zzounds.com/media/FBX1224-specifications-040614-41ccb7518a92f52179f3356fc93df872.pdf
- Behringer: https://www.zzounds.com/item--BEHFBQ2496 · Klark Teknik: https://www.canford.co.uk/Products/93-8601_KLARK-TEKNIK-DF1000-FEEDBACK-SUPPRESSOR-2-channel-1-30-octave-filters-bal-unbalanced-line-level
- Shure: https://markertek.com/product/dfr-22/shure-dfr22-two-channel-audio-processor-feedback-reduction-system · https://old.lsionline.com/news/shure-ships-new-dfr22-audio-processor-bfb3bw
- Yamaha: https://au.yamaha.com/en/news_events/2016/20160622_Yamaha_Expands_the_EMX_Lineup_of_Powered_Mixers.html · https://www.markertek.com/product/ymh-dxr15mkiii/yamaha-dxr15mkiii-powered-15-inch-full-range-2000w-dsp-loudspeaker-with-134db-max-spl-black-abs
- Q-SYS: https://q-syshelp.qsc.com/Content/Schematic_Library/feedback_canceler_notch.htm · Biamp: https://tesira-help.biamp.com/Component_Objects/Audio/Equalizers/Feedback_Suppressor.htm · https://support.biamp.com/Tesira/Programming/Utilizing_the_Feedback_Suppressor_Block_to_Set_EQ
- Allen & Heath AHM: https://www.prosoundweb.com/tag/allen-heath/
- Sennheiser: https://newsroom.sennheiser.com/what-is-truvoicelift · https://newsroom.sennheiser.com/what-is-the-frequency-shifter · https://audioxpress.com/news/sennheiser-introduces-truvoicelift-algorithm-for-teamconnect-ceiling-2-mic-array
- GN ReSound: https://www.gnhearing.com/en/research/features-explained/dfs-digital-feedback-suppression · https://hearingreview.com/miscellaneous/resound-introduces-dfs-ultra-feedback-suppression-technology
- ISOVOX: https://isovoxbooth.com/products/isoplug · https://www.mixonline.com/technology/news-products/isovox-isoplug-voiceover-plug-in-debuts

Paper / patent
- van Waterschoot & Moonen 2010: https://aes2.org/publications/elibrary-page/?id=15738 · https://ftp.esat.kuleuven.be/pub/SISTA/vanwaterschoot/abstracts/09-207.html
- US 11,722,822: https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/11722822
- Sensors 2021: https://www.ncbi.nlm.nih.gov/pmc/articles/PMC8659983/
- Deep AHS: https://arxiv.org/pdf/2302.09252 · NeuralKalmanAHS: https://arxiv.org/pdf/2309.16049 · Recursive: https://arxiv.org/pdf/2309.16048
- In-the-loop DFC: https://arxiv.org/html/2606.03832 · Multichannel CRN: https://arxiv.org/pdf/2505.15914
- Phase synthesizer: https://arxiv.org/html/2510.12377v2 · Combination: https://arxiv.org/html/2602.06921v2
- Graph-based AHS 2026: https://www.sciencedirect.com/science/article/pii/S1110866526000095
- SPS webinar: https://signalprocessingsociety.org/professional-development/education-center/neural-acoustic-feedback-cancellation-signal-processing

Code
- https://github.com/toanaz-ops/rta-tool (HEAD 17f3010) · https://github.com/HoerTech-gGmbH/openMHA · https://github.com/Tympan/Tympan_Library · https://github.com/rogaits/afc-nr
