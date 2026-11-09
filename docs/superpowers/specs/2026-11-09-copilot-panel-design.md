# Lane L2 — Copilot trong Geek mode: tiến trình phụ + bảng phát hiện, rồi "hỏi AI"

**Ngày:** 2026-11-09. **Roadmap:** lane L, tiếp nối L1 ([`2026-10-20-llm-copilot-design.md`](2026-10-20-llm-copilot-design.md)).
**Sổ quyết định:** [`../decisions/2026-11-09-ai-integration.md`](../decisions/2026-11-09-ai-integration.md) (Q1 B, Q2 1, Q3 1, Q7–Q9).
**Nghiên cứu nền:** [`../../research/2026-11-09-ai-integration-llm-and-realtime.md`](../../research/2026-11-09-ai-integration-llm-and-realtime.md).
**Đụng audio path:** **không.** Không file nào trong `src/dsp/`, `AudioEngine` hay
`NotchController` đổi. Mức ra không đổi (0 dB). Giả định "đã qua soundcheck" của
Q6 vì vậy không ảnh hưởng lane này.
**Vì sao là thư viện/tiến trình riêng:** copilot sẽ dùng chung với sản phẩm thứ hai
(cùng định dạng log session), nên nó là một exe độc lập, không phụ thuộc ngược vào app.

All identifiers, file/dir names and code-like string values in this lane are English.

## 1. Hai bước

| Bước | Nội dung | Mạng | Khoá API |
|---|---|---|---|
| **L2a** | Nút `PHÂN TÍCH` trong Geek mode chạy tiến trình phụ, hiện danh sách phát hiện của luật cố định | không | không |
| **L2b** | Ô "hỏi AI" trên cùng bảng, người dùng tự nhập khoá, chọn nhà cung cấp và model | có, chỉ khi bấm hỏi | người dùng tự nhập |

L2a ship trước và đứng một mình được. L2b không bắt đầu trước khi L2a merge.

## 2. Kiến trúc

```
HandsFree (app)                         HandsFreeCopilot.exe (tiến trình phụ)
  CopilotPanel  ── bấm ──► SidecarRunner ── juce::ChildProcess ──► copilot.py đóng băng
       ▲                        │   argv: <log.jsonl> --json --low-priority
       └── callAsync ◄── stdout (UTF-8 JSON) + mã thoát ◄──────────┘
```

- App **không** đọc log, **không** tính luật, **không** gọi mạng. Chỉ có một bộ đọc
  log: `logstats.summarise()` (bài học 3 của L1).
- Tiến trình phụ treo, chết hay bị antivirus chặn thì app chỉ hiện một dòng trạng
  thái. Không hộp thoại lỗi, không ảnh hưởng tiếng.

## 3. Tiến trình phụ (`tools/copilot/`)

### 3.1 Thay đổi trong `copilot.py`

1. `main()` ép `sys.stdout` và `sys.stderr` sang UTF-8
   (`reconfigure(encoding="utf-8")`). Python đóng băng trên Windows mặc định ghi
   stdout bằng code page của máy khi bị pipe; chữ Việt sẽ hỏng hoặc ném
   `UnicodeEncodeError`.
2. Cờ `--low-priority`: hạ độ ưu tiên tiến trình xuống `BELOW_NORMAL_PRIORITY_CLASS`
   bằng `ctypes` (chỉ Windows, lỗi thì bỏ qua). App luôn truyền cờ này để tiến trình
   phụ không tranh CPU với detector.
3. `import logstats` phải chạy được cả khi đóng băng: bỏ phép chèn `sys.path` dựa trên
   `__file__` làm đường duy nhất; đóng gói `tools/logstats.py` vào bundle.
4. Hợp đồng `--json` **giữ nguyên**: một danh sách JSON, mỗi phần tử có
   `code, severity, title, advice, evidence`. Mã thoát giữ nguyên: 0 xong, 2 log rỗng
   hoặc không đọc được, 3 `--ask` thất bại.

### 3.2 Đóng gói

- **PyInstaller, kiểu `onedir`**, tên `HandsFreeCopilot`. Không dùng `onefile`: nó giải
  nén ra thư mục tạm mỗi lần chạy (khởi động chậm) và hay bị antivirus báo nhầm.
- L2a đóng gói **không** kèm gói `anthropic` (`--exclude-module anthropic`); bundle chỉ
  có stdlib. L2b mới thêm.
- Script `tools/copilot/build_copilot.ps1` dựng bundle vào `build/copilot/HandsFreeCopilot/`.
  Phiên bản PyInstaller ghim trong `tools/copilot/requirements-build.txt`.
- **Cổng khói:** script chạy `HandsFreeCopilot.exe tests/fixtures/session-sample.jsonl --json`
  và so byte với `python tools/copilot/copilot.py ... --json`. Khác nhau là fail.
- `installer/release-alpha.ps1` gọi `build_copilot.ps1` sau khi `ctest` xanh. Dựng
  hoặc cổng khói đỏ thì **không phát hành** và trả version về, như một suite đỏ.
- `installer/handsfree.nsi`: cài cả thư mục vào `$INSTDIR\copilot\`; phần gỡ cài xoá
  đúng thư mục đó.

## 4. Phía app

### 4.1 `CopilotFindings` (`src/app/CopilotFindings.{h,cpp}`)

```cpp
struct CopilotFinding { juce::String code, severity, title, advice; };
// Trả về nullopt khi không phải một mảng JSON hợp lệ.
std::optional<std::vector<CopilotFinding>> parseCopilotFindings (const juce::String& utf8Json);
```

Phần tử thiếu trường thì bỏ phần tử đó, không bỏ cả danh sách. `severity` lạ coi như
`info`. `evidence` không cần cho GUI ở L2a.

### 4.2 `SidecarRunner` (`src/app/SidecarRunner.{h,cpp}`)

- Giao diện `CopilotBackend` (seam, theo mẫu `ActivationTransport` của `LicenseManager`):
  `virtual CopilotRunResult run (const CopilotRequest&) = 0;` — chạy **đồng bộ**, gọi
  từ luồng nền. `CopilotRunResult { int exitCode; juce::String stdoutText; Status status; }`
  với `Status { ok, sidecarMissing, timedOut, failed }`.
- `ChildProcessBackend`: bản thật, dùng `juce::ChildProcess`, đọc stdout dạng UTF-8.
  Quá hạn (**30 s** cho phát hiện) thì `kill()` và trả `timedOut`. Con số 30 s đến từ
  phép đo: bundle `onedir` 19,4 MB khởi động nguội mất 1,5–7,5 s trên máy dev (ba lần
  chạy, nghi do Defender quét lần đầu); 10 s là quá sát. File exe không tồn
  tại thì trả `sidecarMissing` mà không thử chạy.
- `SidecarRunner`: sở hữu một `juce::Thread`; `request()` trả về ngay; kết quả về luồng
  message qua `MessageManager::callAsync` với `SafePointer`. Có **bộ đếm thế hệ**: kết
  quả của lượt cũ bị bỏ khi đã có lượt mới hoặc bảng đã huỷ (bài học lane M). Chỉ một
  lượt chạy tại một thời điểm; bấm lần hai khi đang chạy thì bỏ qua.
- Đường dẫn exe: `<thư mục của app exe>/copilot/HandsFreeCopilot/HandsFreeCopilot.exe`.
- Đường dẫn log: getter mới `MainComponent::currentSessionLogFile()`. Không đoán "file
  mới nhất trong thư mục" — sai khi có hai app cùng chạy. Chưa có session log thì bảng
  hiện trạng thái "chưa có log", không chạy tiến trình.

### 4.3 `gui::CopilotPanel` (`src/gui/CopilotPanel.{h,cpp}`)

- **Vị trí (Q9):** nửa dưới cột trái của sàn Geek, ngay dưới `NotchListPanel`. Phát hiện
  nói về notch nên nằm cạnh danh sách notch; vùng này hiện trống. `NotchListPanel` giữ
  tối thiểu chỗ cho tiêu đề + 6 dòng; phần còn lại cho `CopilotPanel`. Cửa sổ thấp không
  đủ chỗ thì `CopilotPanel` ẩn, danh sách notch giữ nguyên như hôm nay.
- Thành phần: tiêu đề `COPILOT`, nút `PHÂN TÍCH`, dòng trạng thái, danh sách cuộn. Mỗi
  phát hiện: chấm màu theo mức (`danger` / `warn` / `dim` của `az::theme`), tiêu đề, lời
  khuyên. Không viết `toUpperCase()` lên chữ Việt (bài học lane U).
- Trạng thái của bảng: `idle` (chưa bấm) · `running` · `findings(n)` · `clean` (0 phát
  hiện) · `noLog` · `sidecarMissing` · `timedOut` · `failed`. Mỗi trạng thái một câu
  tiếng Việt; không trạng thái nào mở hộp thoại.
- Không tự chạy lại theo giờ. Chỉ chạy khi bấm.
- Dumb mode không đổi: `DumbView` vẫn che toàn bộ.

## 5. An toàn

- Tiến trình phụ chỉ **đọc** file log. Nó không có đường nào ghi vào cấu hình âm thanh.
- Lời khuyên vẫn bị ghim bởi `test_advice_never_says_raise_gain`.
- Không có lời gọi mạng nào ở L2a — kiểm bằng cách bundle không chứa `anthropic` và
  `findings()` chỉ dùng stdlib.

## 6. Test

| Tầng | Test | Ở đâu |
|---|---|---|
| Python | stdout UTF-8 khi bị pipe với code page không phải UTF-8; `--low-priority` không đổi kết quả; `--json` không đổi | `tests/test_copilot.py` |
| C++ | `parseCopilotFindings`: fixture thật, mảng rỗng, JSON hỏng, phần tử thiếu trường, mức lạ, chữ Việt nguyên vẹn | `tests/test_copilotfindings.cpp` |
| C++ | `SidecarRunner` với backend giả: thành công, mã 2, quá hạn, exe thiếu, kết quả cũ bị bỏ (thế hệ), huỷ bảng khi đang chạy không crash | `tests/test_sidecarrunner.cpp` |
| C++ | `CopilotPanel`: mỗi trạng thái hiện đúng câu; gọi `onClick()` chứ không `triggerClick()` | `tests/test_copilotpanel.cpp` |
| Ảnh | `HandsFreeSnapshot` thêm cảnh có 3 phát hiện (alert, warn, info) và cảnh `clean`; đọc lại ảnh trước khi báo | `tools/snapshot.cpp` |
| Gói | cổng khói §3.2 | `build_copilot.ps1` |

File nguồn mới phải vào `HANDSFREE_CORE_SOURCES`, nếu không app, test và snapshot đều thiếu.

## 7. L2b — "hỏi AI" (thiết kế, chưa làm)

- **Hai bộ nối, không trộn:** `anthropic` dùng `tool_runner` của SDK chính thức như L1;
  `openai_compatible` là vòng tool viết tay qua HTTP với `urllib` (địa chỉ + model + khoá),
  phủ OpenAI, OpenRouter, Gemini và Ollama tại máy. Claude luôn đi qua SDK chính thức.
- Model không còn viết cứng: `--provider`, `--model`, `--base-url`. Mặc định
  `anthropic` + `claude-opus-5-5`. Lưu trong `settings.json` (phần `product`), **không**
  kèm khoá.
- **Khoá (Q8):** app nhận khoá ở một ô nhập kiểu mật khẩu, mã hoá bằng DPAPI
  (`CryptProtectData`, theo tài khoản Windows) và ghi vào
  `%APPDATA%/AZSoundtech/HandsFree/copilot-credentials.json`. Tiến trình phụ tự giải mã
  bằng `CryptUnprotectData`. Khoá không bao giờ đi qua dòng lệnh, biến môi trường, log
  hay `settings.json`. DPAPI chặn người dùng khác và việc đọc ổ đĩa rời; nó không chặn
  phần mềm độc hại chạy dưới chính tài khoản đó — ghi rõ trong ghi chú cho tester.
- **Lọc câu trả lời bằng mã** cho mọi nhà cung cấp: câu trả lời khuyên tăng gain, tăng
  trần, tắt chống hú hay bỏ bảo vệ thì bị thay bằng một câu từ chối cố định, mã thoát 4.
  Prompt một mình không đủ với model lạ.
- Lần đầu hỏi phải tích ô đồng ý: "câu hỏi và dữ liệu log buổi diễn (không có âm thanh)
  sẽ gửi tới nhà cung cấp bạn chọn".
- Hạn 120 s, có nút huỷ (kill tiến trình con). Không streaming ở bản đầu.
- Test vòng tool `openai_compatible` bằng máy chủ HTTP giả (`http.server`), không cần mạng.
- **Chưa kiểm được cho tới khi có khoá:** một lượt hỏi thật, độ trễ, `usage`.

## 8. Release

L2a là thay đổi nhìn thấy được, không đụng audio path. Theo `CLAUDE.md` nó đi kèm một
bản alpha. **Nhưng `main` đang ở 1.3.0 và 1.4.0 chưa từng được đóng gói**
(`docs/release-notes/1.4.0-alpha.md` ghi "chưa đóng gói"): bản alpha kế tiếp sẽ mang theo
lane U và bốn bản vá audio path trong `docs/release-notes/unreleased/`. Việc phát hành vì
vậy chờ owner (Q11), không tự chạy khi L2a xong.

Tài liệu đi cùng: `docs/GIOI-THIEU.md` (hành vi nhìn thấy được), `installer/TESTER-NOTES.md`.
