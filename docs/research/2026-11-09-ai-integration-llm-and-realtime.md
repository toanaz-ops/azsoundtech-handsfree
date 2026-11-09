# Nhúng AI vào phần mềm: LLM call thật và AI chạy realtime — 2026-11-09

**Câu hỏi của owner:** cách nhúng AI vào phần mềm, gồm (1) lời gọi LLM thật và
(2) loại AI chạy được realtime, không bị trễ.
**Đụng audio path:** không. Đây là tài liệu nghiên cứu; không có thay đổi mức.
**Model viết:** `claude-fable-5-1`. **Sổ quyết định:**
[`../superpowers/decisions/2026-11-09-ai-integration.md`](../superpowers/decisions/2026-11-09-ai-integration.md).

Tài liệu này là **phần thêm** so với ba bản đã có, không lặp lại chúng:
[04/09](2026-09-04-anti-feedback-next-gen.md) (ba tầng ms / giây / ngày),
[06/09](2026-09-06-cedar-realtime-plugins.md) (Voxis, DNN < 10 ms),
[20/10](2026-10-20-defeedback-landscape.md) (Deep AHS, P8).

## 0. Kết luận ngắn

1. **"AI" ở đây là hai thứ khác hẳn nhau**, không dùng chung một kỹ thuật:
   LLM (trả lời trong vài giây, cần mạng) và mạng nơ-ron nhỏ (trả lời dưới
   1 ms, chạy ngay trong app). Không có LLM nào chạy realtime trên đường âm
   thanh, và không cần.
2. **LLM call thật: mã đã có, nhưng chưa từng chạy thật.** `copilot.py --ask`
   (lane L1) chưa có một lượt gọi nào thành công. Phiên này cũng không chạy
   được: máy không có `ANTHROPIC_API_KEY`, không có `ant`. Việc còn thiếu là
   **một khoá API và một quyết định của owner về chỗ đặt khoá**, không phải
   công nghệ.
3. **AI realtime: công nghệ sẵn, dữ liệu chưa có.** Thư viện RTNeural chạy
   mạng nhỏ dưới 1 ms và hợp với lane C. Thứ chặn lane C là nhãn: cần ≥ 300
   lần bấm oan/đúng từ ≥ 3 buổi, và tính đến 21/10 chưa có phản hồi alpha nào
   được ghi lại.
4. **"Không trễ" chỉ đúng với AI ra quyết định.** Mạng chỉ *quyết định* (hú
   hay nốt nhạc) thì không thêm một mẫu trễ nào vào tiếng. Mạng *xử lý tín
   hiệu* (khử ồn, tách giọng) luôn thêm trễ và đổi tiếng — thứ owner đã cấm
   cho Hands-free ở lane A.
5. **Thứ tự đề xuất:** buổi nghe 1.4.0 trước (đề xuất 21/10 vẫn đứng), rồi
   mới tới AI. Chi tiết ở §4.

## 1. Hiện trạng trong repo

| Thứ | Trạng thái | Nguồn |
|---|---|---|
| Luật phát hiện từ log (`findings()`) | xong, 21 test, không cần mạng | `tools/copilot/copilot.py`, spec 20/10 §4 |
| `--ask` gọi Claude qua Tool Runner, 4 tool chỉ đọc | viết xong, **chưa có lượt gọi thật nào** | spec 20/10 §6 "Chưa kiểm" |
| Lời gọi mạng trong app | không có lúc chạy. `LicenseManager` có lớp `ActivationTransport` (HTTPS POST qua `juce::URL`, có seam để tiêm fake) nhưng GUI không gọi tới | `src/app/LicenseManager.h:14-25, 101`; grep `src/gui` = 0 |
| Mạng nhỏ trên detector thread (lane C) | chưa viết spec; chờ nhãn | roadmap v2 dòng 25 |
| Suy luận nơ-ron trong build | không có (không RTNeural, không ONNX) | grep `CMakeLists.txt` |

Hai điểm lệch phát hiện trong phiên này:

- Note `memory/lane-l-copilot-offline-2026-10-20.md` ghi SDK `anthropic` 1.15
  (lane L chạy trên máy khác). Máy này cài **1.0.0**. Đã kiểm bằng
  `inspect.signature`: `tool_runner` của 1.0.0 có đủ `fallbacks`, `betas`,
  `thinking`, `output_config`, `max_iterations`, nên bản SDK không chặn lượt
  gọi thật đầu tiên. Thứ duy nhất còn thiếu là khoá.
- `LicenseManager` cho thấy app đã có sẵn một mẫu "gọi mạng ngoài audio thread,
  có seam để test". Một cầu nối LLM trong app nên dùng lại đúng mẫu đó.

## 2. LLM call thật

### 2.1 Vì sao LLM không bao giờ ở tầng realtime

Detector có 10,7 ms cho mỗi hop (512 mẫu ở 48 kHz). Một lượt LLM qua mạng
mất từ dưới một giây tới nhiều giây, và thời gian đó không có trần: mạng
chậm, API quá tải, model từ chối. Streaming chỉ làm chữ đầu tiên hiện sớm
hơn, không làm câu trả lời xong sớm hơn. Chưa có số đo độ trễ nào từ repo
này — phải đo khi có khoá.

Vì vậy LLM chỉ làm việc **người chờ được**: giải thích sau buổi diễn, trả lời
câu hỏi trong giờ nghỉ, phân tích nhiều buổi theo venue.

### 2.2 Gọi Claude từ đâu, bằng gì

- **C++ không có SDK chính thức.** Hai đường hợp lệ: (a) HTTP thô
  `POST https://api.anthropic.com/v1/messages` với header `x-api-key` và
  `anthropic-version: 2026-10-21`; (b) một tiến trình phụ viết bằng Python
  dùng SDK chính thức. Đường (b) đã có sẵn: `tools/copilot/`.
- **Model:** `claude-opus-5-5` (đã chốt tạm ở Q5 lane L). Giá theo bảng của
  Anthropic (bản lưu 06/10/2026): Opus 5.5 $4 / $20 mỗi triệu token vào / ra;
  Haiku 5.5 $0,10 / $0,50.
- **Ước lượng chi phí (chưa đo):** một câu hỏi trên một log vài trăm dòng cỡ
  6.000 token vào và 1.500 token ra ≈ $0,05 với Opus 5.5. Đó là **mức sàn**:
  vòng tool gửi lại toàn bộ lịch sử ở mỗi vòng, và token suy nghĩ tính như
  token ra, nên một câu hỏi dùng cả 4 tool thực tế cỡ **$0,10–0,30**. Với
  Haiku 5.5 thì dưới $0,01. Số thật lấy từ `response.usage` khi chạy.
- **Bắt buộc trong mã gọi:** kiểm `stop_reason` trước khi đọc nội dung
  (`refusal` trả HTTP 200), bật `fallbacks="default"`, timeout ngắn, và mọi
  lỗi đều là "không có câu trả lời" chứ không phải lỗi của app.

### 2.3 Bốn cách đưa LLM tới soundman

| | Cách | Được | Mất | Đảo ngược |
|---|---|---|---|---|
| A | **Giữ CLI offline như hôm nay** | không rủi ro; app không đổi | soundman không mở dòng lệnh; gần như không ai dùng | — |
| B | **Tiến trình phụ** (`HandsFreeCopilot.exe`, đóng gói từ `tools/copilot`), app bấm nút thì khởi chạy; nó đọc đúng file log JSONL mà `SessionLogger` đang ghi | tách hẳn khỏi tiến trình âm thanh: nó treo hay chết thì tiếng không sao; dùng lại 100 % luật và 4 tool đã test; file log là kênh trao đổi sẵn có, không cần IPC mới | installer lớn thêm vài chục MB; thêm một thứ phải ký và cập nhật | dễ: bỏ nút |
| C | **Trong tiến trình app**, luồng nền, HTTP thô qua `juce::URL` theo mẫu `ActivationTransport` | một file exe; giao diện chat mượt | lời gọi mạng nằm cùng tiến trình với audio callback: lỗi ở tầng TLS, antivirus hay cấp phát bộ nhớ dồn dập đều chung nhà với tiếng đang ra PA; phải viết lại vòng tool bằng C++ | vừa: phải gỡ mã |
| D | **LLM chạy tại máy** (llama.cpp / Ollama) | không cần mạng ở venue; không gửi log đi đâu | model nhỏ trả lời kém hơn rõ; tải vài GB; ăn CPU đúng lúc app cần CPU cho âm thanh | dễ |

**Khuyên: B**, và làm theo hai bước. Bước đầu chỉ đưa **luật cố định** lên GUI
(không LLM, không mạng) — bước này đã cho soundman phần lớn giá trị. Bước sau
mới thêm "hỏi Claude" qua tiến trình phụ. Lý do: đây là âm thanh trực tiếp;
thứ gì có thể treo thì ở ngoài tiến trình đang ra loa.

### 2.4 Khoá API đặt ở đâu — quyết định khó nhất

| | Cách | Được | Mất |
|---|---|---|---|
| 1 | **Người dùng tự nhập khoá** của họ | AZ không tốn tiền, không vận hành gì | soundman phải có tài khoản Anthropic; gần như không ai làm |
| 2 | **Máy chủ trung gian của AZ** giữ khoá; app gọi máy chủ, máy chủ gọi Claude | người dùng không cần gì; AZ đặt được hạn mức theo máy, đổi model không cần ra bản mới; khoá không rời máy chủ | AZ trả tiền và phải chạy một dịch vụ; log buổi diễn đi qua máy chủ AZ nên cần nói rõ với người dùng |
| 3 | Nhúng khoá vào file cài | — | **không được làm**: khoá trong file exe ai cũng lấy ra được |

**Khuyên: 2** khi sản phẩm tới tay người ngoài; **1** là đủ cho vòng thử nội bộ.
Cách 2 là quyết định có chi phí thường xuyên và liên quan dữ liệu người dùng,
nên phải do owner chốt.

### 2.5 Ràng buộc an toàn giữ nguyên

Bốn điều đã chốt ở lane L vẫn áp dụng cho mọi bước sau: LLM chỉ **đọc**; không
bao giờ khuyên tăng gain, tăng trần, tắt chống hú; phát hiện do luật tính, LLM
chỉ giải thích; LLM không ghi vào cấu hình âm thanh. "LLM đề xuất preset để
app nạp" là một lane riêng và cần phép của owner.

## 3. AI realtime

### 3.1 Hai loại, hai chi phí

| | Mạng chỉ ra quyết định | Mạng xử lý tín hiệu |
|---|---|---|
| Ví dụ | phân loại "hú hay nốt nhạc giữ" | khử ồn, tách giọng (RNNoise, DeepFilterNet, CEDAR Voxis) |
| Tiếng đi qua mạng? | không | có |
| Trễ thêm vào tiếng | **0** | luôn có: một khung + phần nhìn trước |
| Đổi tiếng? | không | có |
| Chỗ chạy | detector thread, ngân sách < 1 ms mỗi hop | audio thread hoặc luồng riêng có bù trễ |
| Hợp với | Hands-free (lane C) | sản phẩm thứ hai (voice isolator) |

Số liệu của loại thứ hai: DeepFilterNet trễ thuật toán 40 ms (cửa sổ 20 ms,
hop 10 ms, nhìn trước 2 khung), hệ số thời gian thực 0,19 trên một nhân
i5-8250U; RNNoise 0,03 trên cùng CPU; Voxis công bố < 10 ms. 40 ms là quá
nhiều cho monitor sân khấu; < 10 ms thì dùng được nhưng vẫn đổi tiếng.

### 3.2 Bằng chứng mạng nhỏ làm được việc phát hiện hú

Một mạng CRNN phát hiện hú cho cuộc gọi thoại (ICASSP 2022, NetEase) nặng
**121 kB**, đạt 89,46 % phát hiện và 0,40 % báo nhầm, chạy realtime trên điện
thoại. Lưu ý: đó là giọng nói trong cuộc gọi, không phải nhạc sống. Nốt nhạc
giữ dài là ca khó của ta và không có trong bài đó, nên con số không chuyển
thẳng sang được.

### 3.3 Thư viện suy luận

| Thư viện | Bản chất | Hợp với |
|---|---|---|
| **RTNeural** (BSD-3) | C++ thuần, làm riêng cho audio realtime; Dense, GRU, LSTM, Conv1D/2D, BatchNorm; trọng số nạp từ JSON xuất từ Keras/PyTorch; API `ModelT` cố định kiến trúc lúc biên dịch; nền Eigen, xsimd hoặc STL; thêm bằng `add_subdirectory` | **lane C** — mạng vài chục tới vài trăm kB |
| **ONNX Runtime** | engine đa dụng; nhanh nhất với mạng không trạng thái; không được thiết kế cho luồng realtime | mạng lớn, chạy ngoài audio thread |
| **anira** | lớp bọc ONNX Runtime / LibTorch / TFLite: đẩy suy luận sang một nhóm luồng riêng và báo cho host độ trễ phải bù | **sản phẩm thứ hai** (mạng xử lý tín hiệu) |
| LibTorch | nặng, cấp phát bộ nhớ lúc chạy | chỉ để huấn luyện/thử nghiệm |

Với lane C, RTNeural là đủ và không kéo theo thư viện nặng nào. Câu "không
cấp phát bộ nhớ sau khi dựng" của `ModelT` đến từ nguồn bên thứ ba; phải tự
kiểm trên mã nguồn trước khi tin.

Bài DAFx 2026 (Maynooth) so các khung này trong một phiên DAW có tải thật và
kết luận: cặp model–khung trông dư sức realtime khi đo riêng vẫn có thể cho
trễ đuôi lớn và rớt tiếng khi chạy chung CPU với thứ khác. Bài đo trên Apple
Silicon nên bảng xếp hạng không áp cho Windows; **phương pháp thì áp được**:
chọn theo trễ đuôi (p99) dưới tải, không theo trung bình đo riêng.

### 3.4 Luật tích hợp bắt buộc (live-sound)

1. Mạng chạy trên **detector thread**, không trên audio callback. Audio
   callback tiếp tục không heap, không khoá, không IO.
2. Nạp model ngoài luồng realtime; bộ đệm suy luận cấp phát trước một lần.
3. Đầu vào và đầu ra kiểm `isfinite`; đầu ra kẹp về [0, 1]; bật chống
   denormal quanh lời gọi suy luận.
4. **Model thiếu, hỏng hay trả NaN thì quay về `CandidateScorer` hôm nay**,
   không im lặng tắt chống hú.
5. Ra mắt ở **chế độ bóng** trước: mạng chấm điểm và ghi vào log bên cạnh
   quyết định của scorer, nhưng không điều khiển notch. Chỉ cho mạng quyền
   quyết định sau khi số liệu bóng tốt hơn scorer trên log thật.
6. Trọng số là file có phiên bản, test ghim kết quả trên fixture cố định.
   Ngân sách < 1 ms nghiệm thu bằng **p99 đo trong app đang chạy đủ 8 slot**,
   không bằng trung bình của một vòng lặp đo riêng.
7. Mạng là phần sẽ dùng chung với sản phẩm thứ hai, nên lớp nạp model và chạy
   suy luận phải là **thư viện riêng** (luật trong `CLAUDE.md`), không lẫn vào
   mã detector.

### 3.5 Thứ thật sự chặn lane C

Không có nhãn thì không có gì để học. Ba lối ra, không loại trừ nhau:

- **Chờ alpha** sinh ≥ 300 nhãn. Đúng nhất, nhưng phụ thuộc người bấm nút.
- **Dữ liệu tổng hợp** để huấn luyện trước: mô phỏng vòng hú (đáp ứng phòng +
  gain tăng dần) đối nghịch với nốt nhạc giữ. Rủi ro đã ghi trong
  `memory/peakiness-sweep-2048-2026-09-04.md`: tín hiệu tổng hợp đơn giản
  không đại diện được cho nhạc. Chỉ dùng để khởi động, không dùng để nghiệm thu.
- **Chế độ bóng trong alpha**: mỗi lần scorer và mạng bất đồng là một mẫu đáng
  xem, kể cả khi không ai bấm nút.

## 4. Lộ trình đề xuất

Đề xuất 21/10 khuyên đóng băng lane mới tới sau một buổi nghe 1.4.0. Các bước
dưới đây xếp **sau** buổi nghe đó, trừ bước 1 không đụng app.

| # | Việc | Cỡ | Cần owner |
|---|---|---|---|
| 1 | Chạy `--ask` thật một lần trên `tests/fixtures/session-sample.jsonl`, ghi độ trễ và `usage` | S | một khoá API |
| 2 | Đưa luật cố định lên Geek mode (không LLM, không mạng) | S–M | ảnh render theo luật GUI |
| 3 | "Hỏi Claude" qua tiến trình phụ | M | chốt §2.3 và §2.4 |
| 4 | Lane C0: ghi đặc trưng ở chế độ bóng, chưa có mạng | S | — |
| 5 | Lane C1–C2: huấn luyện offline, RTNeural chạy bóng | M–L | nhãn, hoặc phép dùng dữ liệu tổng hợp |
| 6 | Lane C3: mạng được quyền quyết định | M | buổi nghe riêng |
| 7 | Sản phẩm thứ hai: anira + ONNX | L | quyết định sản phẩm |

## 5. Chưa kiểm và giới hạn

- **Chưa có lượt gọi LLM thật nào** — phiên này không có thông tin đăng nhập.
- Chi phí mỗi câu hỏi ở §2.2 là ước lượng, không phải số đo.
- Bài DAFx 2026 chỉ đọc phần tóm tắt và kết luận; chưa đọc bảng số liệu.
- "App không có lời gọi mạng lúc chạy" dựa trên grep toàn `src/`:
  `LicenseManager` / `LicenseViewModel` chỉ xuất hiện trong chính 4 file của
  chúng. Chưa kiểm bằng cách theo dõi mạng của app đang chạy.
- Tuyên bố "không cấp phát" của RTNeural chưa kiểm trên mã nguồn.
- "0 phản hồi alpha" là số của bản rà soát 21/10; chưa kiểm lại hôm nay.
- Chưa đo gì trên phần cứng. Mọi con số ở §3.1–3.2 là của tác giả các bài báo.

## Nguồn

- RTNeural: https://github.com/jatinchowdhury18/RTNeural · https://arxiv.org/abs/2106.03037
- anira: https://arxiv.org/abs/2506.12665 · https://github.com/anira-project/anira
- DAFx 2026, benchmark khung suy luận dưới tải DAW (đọc tóm tắt + kết luận): https://www.dafx.de/paper-archive/2026/papers/DAFx26_paper_16.pdf
- DeepFilterNet: https://arxiv.org/abs/2305.08227 · DeepFilterNet2: https://arxiv.org/abs/2205.05474
- CRNN phát hiện hú 121 kB (ICASSP 2022): https://rc.signalprocessingsociety.org/conferences/icassp-2022/spsicassp22vid0284
- Deep AHS: https://arxiv.org/abs/2302.09252 · NeuralKalmanAHS: https://arxiv.org/abs/2309.16049
- Claude API (HTTP thô, model, giá): skill `claude-api` của Claude Code, bảng model lưu 06/10/2026.
