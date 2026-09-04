# Nghiên cứu: thế hệ chống hú kế tiếp — ngoài notch trừ gain

**Ngày:** 2026-09-04. **Người hỏi:** owner. **Người nghiên cứu:** Claude Fable 5.1.
**Trạng thái:** nghiên cứu đã chốt thành roadmap
[`../superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md`](../superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md).

Owner đặt bốn câu hỏi: (1) LLM tham gia được vào anti-feedback không,
(2) tác động polarity/pha thay vì trừ gain, (3) stereo independent,
(4) đề xuất thêm. Tài liệu này ghi lại câu trả lời, bằng chứng, và
những gì đã bị loại cùng lý do — để phiên sau không nghiên cứu lại.

## 0. Hiện trạng đo được từ code (27/08/2026, v1.0.4)

- Detector tap **chỉ làn 0** của mỗi slot (`AudioEngine.h` ~dòng 161,
  "post-notch lane-0 output"). Lệnh notch từ controller áp cho **cả hai
  làn** của slot. Bản chất: *dò mono, đè stereo*. Mic ở cánh trái → cắt
  cả cánh phải vô ích. Đây là điểm yếu thật, không phải giả định.
- Quyết định notch = luật cứng `peakiness × rise × novelty`, phạt
  harmonic 0.5 trong dải 1.4×–4.1×. Persistence 3 block, ngưỡng 10.0
  (chưa đo lại cho FFT 2048 — xem lane T trong plan trước).
- Depth cố định theo preset (−12/−18/−24 dB), nhả cụt sau 30 s im.
- Soundcheck = chờ phòng tự hú 15 s. Không có phép đo chủ động.
- Ring-risk readout có UI, không có data (`docs/spec-ring-risk.md`).

## 1. LLM trong anti-feedback

**Kết luận: không ở tầng realtime, có ở hai tầng chậm hơn.**

Ngân sách quyết định của detector là 10.7 ms/hop. LLM trả lời trong
hàng trăm ms đến giây và không xử lý tín hiệu. Mọi sản phẩm "AI
feedback" trên thị trường dùng **mạng nhỏ** (CNN/LSTM vài trăm KB,
< 1 ms trên CPU). Phân biệt rõ:

| Tầng | Phản ứng | Công cụ | Việc |
|---|---|---|---|
| ms | mỗi hop | mạng nhỏ (RTNeural / ONNX Runtime) trên detector thread | phân loại hú vs nốt nhạc giữ; thay/bổ trợ CandidateScorer |
| giây–phút | theo yêu cầu | LLM qua tool call đọc telemetry | copilot cho soundman: notch tái phát, notch chồng, gợi ý hạ monitor/xoay mic |
| ngày | offline | agent + pipeline log | học preset theo venue, tinh chỉnh ngưỡng, fine-tune mạng nhỏ |

Tiền đề cho cả tầng ms và tầng ngày là **vòng dữ liệu**: nút "oan"/"đúng"
trên NotchListPanel + log session (magnitudes quanh sự kiện, không lưu
audio). Không có nút đó thì không có nhãn, không có gì để học.

Bằng chứng: IEEE SPS webinar "Neural Acoustic Feedback Cancellation"
(06/2025) — hướng kết hợp adaptive filter + deep learning để xử lý phi
tuyến; bài graph-based temporal anomaly detection cho howling (2026) —
mô hình hóa cấu trúc băng hẹp và động học chậm của hú; LSTM 1 lớp với
ngữ cảnh ~1.1 s đủ cho realtime.

## 2. Pha / polarity thay vì trừ gain

Hú cần đồng thời: loop gain ≥ 1 **và** pha vòng lặp là bội 360°. Notch
tấn công gain; các kỹ thuật dưới tấn công pha.

| Kỹ thuật | GBF thêm | Speech | Music | Kết luận |
|---|---|---|---|---|
| Lật cực toàn dải | 0 | – | – | **Loại.** Dịch 180° ở mọi tần số chỉ chuyển hú sang tần số khác |
| Allpass băng hẹp tại f0 | ~0 một mình | – | – | Một mình chỉ đẩy hú sang f0±Δ. **Giữ** ở dạng cặp allpass + notch nông (−6 thay vì −18) — cần đo pha vòng lặp trước |
| Frequency shift 3–5 Hz (Schroeder 1962) | +5..6 dB | tốt | beat, harmonic sai | **Giữ** làm mode Speech tùy chọn, tắt mặc định |
| Phase/delay modulation | +3..6 dB | tốt | vibrato nhẹ ở mức cao | **Giữ** làm tùy chọn sau, ưu tiên thấp |
| AFC (NLMS + PEM) | +10..15 dB | tốt | tốt, không cắt tone | **Giữ**, lane lớn cuối cùng: filter 100–300 ms, phân kỳ khi mic di chuyển = phát tiếng lạ ra PA; bắt buộc watchdog + fallback notch |

Bằng chứng: Rane note 158 (tổng quan suppressor, GBF từng kỹ thuật);
Uni Stuttgart 2006 (frequency shifting cho PA); Hearing Review (kết hợp
phase cancellation + frequency shift + fingerprint, +7 dB transient
shift); PEM-AFC cho train PA (Signal Processing 2019); "PEM-based
howling cancellation: can we do better?" (IEEE 2022).

## 3. Stereo independent

Xác nhận từ code (mục 0). Hai cách:

- **A. Mỗi làn một detector.** Đơn giản, 16 thread, ~+4.4 MB heap,
  2× CPU FFT nền. Không có thông tin chéo.
- **B. Một detector/slot nhìn cả hai làn.** Hai FFT, scorer so sánh L
  với R. **Chọn B**: tỉ lệ L/R là đặc trưng mạnh — hú từ một loa vào
  mic bất đối xứng theo hình học, nhạc từ mixer stereo cân bằng hoặc
  pan ổn định. Mic giữa → cả hai làn thấy → notch cả hai, tự nhiên.

Tác dụng phụ: notch một bên lệch stereo image tại f0. Q=30 băng rất hẹp,
gần như không nghe; vẫn cần nút LINK cho người muốn giữ image.

## 4. Đề xuất thêm

1. **Soundcheck đo chủ động**: phát chirp/MLS qua từng output riêng,
   đo loa→mic, tính loop gain theo tần số → đặt notch trước khi có
   tiếng hú, biết cánh nào gây. Nền cho allpass+notch và AFC.
2. **Depth theo nhu cầu**: ringing tăng mũ với τ → ước lượng loop gain
   vượt bao nhiêu dB → depth = −(vượt + 3 dB). Tone tốt hơn đối thủ.
3. **Nhả dần**: −18 → −12 → −6 → clear, peakiness quay lại thì kẹp lại.
4. **Ring-risk**: nối data theo spec có sẵn, cùng tín hiệu rise.
5. **Venue memory**: fingerprint phòng → gợi ý preset; việc cho LLM
   tầng ngày.

## 5. Đã loại

- LLM trong vòng lặp realtime: sai công cụ, không phải vấn đề tốc độ
  phần cứng.
- Lật cực toàn dải hoặc băng hẹp đứng một mình: chỉ dời hú.
- Frequency shift bật mặc định: hỏng music.
- Tăng số detector thread lên 16 (cách A): mất feature L/R.

## Nguồn

- https://www.ranecommercial.com/legacy/note158.html
- https://www.iss.uni-stuttgart.de/en/research/publications/scheuing_eders2006.pdf
- https://hearingreview.com/practice-building/practice-management/combining-phase-cancellation-frequency-shifting-and-acoustic-fingerprint-for-improved-feedback-suppression
- https://www.sciencedirect.com/science/article/abs/pii/S0165168419303330
- https://ieeexplore.ieee.org/abstract/document/9999433/
- https://signalprocessingsociety.org/blog/sps-sltcaasp-tc-webinar-neural-acoustic-feedback-cancellation-signal-processing-deep-learning
- https://www.sciencedirect.com/science/article/pii/S1110866526000095
- https://www.sciencedirect.com/science/article/pii/S221201731630281X/pdf
