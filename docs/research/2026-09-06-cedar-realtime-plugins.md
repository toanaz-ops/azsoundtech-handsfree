# CEDAR Audio "Live Sound" plug-ins — nghiên cứu 2026-09-06

**Câu hỏi của owner:** nghiên cứu https://cedaraudio.com/plugins/realtime, trong
bối cảnh câu hỏi trước đó "có nên nhúng AI vào Hands-free". Note này trả lời:
CEDAR làm gì, làm bằng cách nào, và Hands-free học được gì.

## 1. CEDAR bán gì trên trang đó

| Sản phẩm | Việc | Thuật toán | Trễ | Giá |
|---|---|---|---|---|
| DNS One | dialogue noise suppression (LEARN noise print, chọn dải) | thích nghi, không ML — "CEDAR Quantum" | 14 sample (≈0.29 ms @48k) | £249 |
| ScreenVox | noise suppression cho lời nói | thích nghi, không ML — Quantum | 14 sample | £249 |
| StageVox | noise suppression + bớt reverb cho ca sĩ | Quantum (không nói rõ) | "< 1 ms" | £249 |
| Voxis (27/08/2026) | voice isolator (tách giọng khỏi noise, reverb, khán giả) | **deep neural network** | **< 10 ms** | £199 |

Định dạng: VST3 / AU / AAX Native, Mac + Windows, CPU-only, chạy nhiều
instance một lúc (SOS: "mất đếm" trên M1 @96k với ScreenVox). Không có
SoundGrid/Venue native; phần cứng DNS 8D có Dante.

**CEDAR không có sản phẩm chống hú.** Trang "Applications / Live sound" viết
"prevent feedback" nhưng không sản phẩm nào, không cơ chế nào được nêu. Hiểu
đúng: bớt noise/reverb/bleed trên mic ca sĩ → mixer đẩy gain được thêm vài dB
trước khi hú. Đó là tăng GBF gián tiếp, không phải detector.

## 2. Điều đáng học nhất: hai tầng trễ, hai công nghệ

CEDAR chọn thuật toán theo ngân sách trễ, không theo mốt:

- **Quantum (thích nghi, không ML): 14 sample.** Cùng tầng với notch biquad
  của Hands-free (spec §4.4: đường audio chỉ qua IIR, trễ thêm ≈0). Ba năm
  liền họ giữ dòng này và mới ra thêm Voxis, không thay thế.
- **Voxis (DNN): < 10 ms.** Được coi là "thấp bất thường với mạng nơ-ron", và
  vẫn gấp ~30 lần Quantum. CEDAR chấp nhận 10 ms cho một việc mà DSP cổ điển
  không làm được (tách giọng khỏi khán giả/reverb). Họ không dùng DNN cho
  việc DSP cổ điển đã làm tốt.

Ánh xạ sang Hands-free (hop 512 @48k = 10.7 ms, FFT 1024 hop 50%):

| Việc | CEDAR làm bằng | Hands-free tương đương | Kết luận |
|---|---|---|---|
| xử lý tín hiệu trên đường audio | Quantum, 14 sample | biquad notch, ≈0 | giữ nguyên, không nhét mạng vào audio thread |
| quyết định "cái gì là noise/giọng" | DNN 10 ms, chỉ khi cần | detector thread 10.7 ms/hop | ngân sách của lane C (RTNeural < 1 ms) khớp đúng tầng này |

Tức là roadmap v2 (lane C = classifier nhỏ trên detector thread, không đụng
audio path) đã cùng kiến trúc với cách CEDAR tách Quantum/Voxis. Không cần đổi.

## 3. Ba thứ Hands-free có thể lấy về ngay

1. **Ba nút, không hơn.** ScreenVox: Attenuation / Focus / Bias. StageVox:
   Attenuation / Focus / Ambience. Voxis: Low / Mid / High + Link + Voice.
   Sản phẩm £249 của hãng 30 năm chỉ lộ 3 tham số. Hands-free đang lộ nhiều
   hơn thế trên console; lane G (depth theo nhu cầu) là cơ hội gom về
   "Aggression / Music-safe / Release" hoặc tương đương.
2. **Trễ là con số được in trên hộp.** "14 samples" là câu bán hàng. Hands-free
   có cùng lợi thế (biquad, ≈0 ms) nhưng `docs/GIOI-THIEU.md` chưa nói thành
   một con số đo được. Nên đo và in: trễ thêm của app tính bằng sample.
3. **"Music-safe" là trục cạnh tranh, không phải "mạnh hơn".** SOS phê ScreenVox
   ở chỗ giọng nền và ambience bị ăn — cùng loại lỗi với notch oan trên nốt
   nhạc dài. Nhãn GOOD/FALSE (lane D) chính là dữ liệu để đo trục này.

## 4. Không nên lấy

- **Không nhúng DNN vào audio thread.** CEDAR có 30 năm và vẫn để DNN ở 10 ms.
- **Không thêm noise suppression vào Hands-free.** Khác bài toán, khác dữ liệu
  huấn luyện, và CEDAR/Waves Clarity/iZotope đã chiếm chỗ. Nếu soundman cần,
  họ cắm StageVox trước Hands-free trong chuỗi insert.
- **Không đợi Voxis-style model cho lane C.** Lane C là classifier
  (hú / không hú), vài chục nghìn tham số, không phải separator.

## 5. Ảnh hưởng lên roadmap v2

Không đổi thứ tự. Củng cố hai điểm:
- Lane C giữ ràng buộc "< 1 ms trên detector thread, không đụng audio path".
- Lane G nên thêm mục tiêu UI "≤ 3 tham số lộ ra" vào phần "Mỗi lane phải đạt".

## 6. Quyết định owner (06/09/2026, sau khi đọc note này)

1. **Hai chế độ giao diện, chuyển qua lại được: "Dumb mode" (≤ 3 nút) và
   "Geek mode" (console đầy đủ như hiện tại).** Không bỏ console hiện tại.
   Chưa có spec; cần brainstorm trước khi lên plan. Ghi vào roadmap v2 là
   lane U (UI), phụ thuộc G vì G quyết định 3 nút đó là gì.
2. **Voice isolator kiểu Voxis — làm riêng, không nhét vào Hands-free.**
   Lý do và bước đầu ở §7. Owner nêu bối cảnh: Hands-free chủ yếu nằm trên
   channel mic vox, nên hai thứ cùng một điểm insert.

## 7. Voice isolator: dự án riêng, nền tảng chung

Khuyến nghị: **DSP core và pipeline ML là dự án riêng; vỏ ứng dụng (JUCE
host, ASIO bridge, ring buffer, SessionLogger, theme Sodium Rack, installer,
release-alpha) tách thành thư viện chung để cả hai dùng.** Không đưa DNN vào
repo Hands-free vì:

- **Khác ngân sách trễ và CPU.** Hands-free bán "≈ 0 ms, biquad". Một DNN
  10 ms, một lõi CPU/instance, đặt cạnh nó trong cùng app là đặt gate
  `ctest`, budget audio thread và lời hứa trễ của Hands-free vào rủi ro.
- **Khác dữ liệu và cách đo.** Hands-free đo bằng peakiness, nhãn GOOD/FALSE,
  hú/không hú. Isolator đo bằng PESQ/STOI/SI-SDR trên bộ mix giọng + noise +
  reverb + khán giả; cần GPU, cần bộ dữ liệu ca hát (không chỉ speech).
- **Khác hình thái sản phẩm.** Isolator sống tốt nhất dạng VST3/AAX (DAW,
  SoundGrid, Venue) — CEDAR bán đúng dạng đó. Hands-free là standalone ASIO
  insert. Cùng một core, hai vỏ.
- **Bán riêng, giá riêng.** CEDAR bán 4 plug-in rời và bundle. Gộp sau dễ
  hơn tách sau.

Bước đầu rẻ (vài tuần, không cần huấn luyện): dựng prototype JUCE nhúng một
model mã nguồn mở đã có sẵn realtime — RNNoise (BSD, frame 10 ms, ~48 kB
model) hoặc DeepFilterNet (MIT/Apache, vài ms) — chạy trên mic vox thật qua
Audient, đo trễ + CPU + nghe artefact trên giọng hát (không chỉ nói). Kết
quả trả lời câu "có đáng đầu tư 6–12 tháng huấn luyện model riêng không"
trước khi tiêu tiền GPU.

## Nguồn

- https://cedaraudio.com/plugins/realtime · /plugins/voxis · /plugins/stagevox · /plugins/screenvox
- https://cedaraudio.com/applications/livesound
- https://www.soundonsound.com/reviews/cedar-screenvox (14 sample, CPU, artefact)
- https://www.kvraudio.com/news/cedar-audio-releases-voxis---sub-10ms-neural-network-voice-isolator-68268
- https://www.soundonsound.com/news/voxis-cedar-audio
- https://fohonline.com/blogs/new-gear/cedar-icons-plug-ins-for-live-sound/
