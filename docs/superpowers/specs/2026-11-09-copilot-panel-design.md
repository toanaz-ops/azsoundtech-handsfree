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
- Thành phần: tiêu đề `COPILOT`, nút `PHÂN TÍCH` và `CÀI AI`, dòng trạng thái, danh sách cuộn,
  và (L2b) một hàng hỏi ở đáy: ô nhập một dòng + nút `HỎI` (thành `HUỶ` khi đang hỏi). Mỗi
  phát hiện: chấm màu theo mức (`danger` / `warn` / `dim` của `az::theme`), tiêu đề, lời
  khuyên. Không viết `toUpperCase()` lên chữ Việt (bài học lane U).
- Trạng thái của bảng: `idle` (chưa bấm) · `running` · `findings(n)` · `clean` (0 phát
  hiện) · `noLog` · `sidecarMissing` · `timedOut` · `failed` · và, L2b, `asking` ·
  `answer` · `blocked` · `askFailed` · `needsSetup` · `cancelled`. Khu cuộn hiện HOẶC các dòng
  phát hiện HOẶC một khối chữ (câu trả lời kèm dòng chân `model · N in / M out`, câu chặn cố
  định, hoặc lý do lỗi). Mỗi trạng thái một câu
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

## 7. L2b — "hỏi AI" (đã làm ở L2b)

Phía giao diện (bước 2b) đã cài: hàng hỏi + khối câu trả lời ở `gui::CopilotPanel`; lớp phủ
`gui::CopilotSetupPanel` (Q12) phủ đúng cột trái (danh sách notch + bảng COPILOT) khi bấm
`CÀI AI`, không phủ cột phải nên nút BYPASS vẫn bấm được; ẩn khi rời Geek hoặc khi cột quá thấp
để chứa đủ các hàng. `MainComponent` giữ kho khoá DPAPI, nối `onValuesChanged` vào cùng đường
lưu settings với chế độ UI (`AppSettings`: nhà cung cấp, model, địa chỉ, đồng ý — không bao giờ
khoá), và chạy câu hỏi trên cùng `SidecarRunner` (một lượt một lúc, hạn 120 s, `HUỶ` gọi
`cancel()`). Chưa có lượt gọi nhà cung cấp thật nào được kiểm.

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


## L2b sidecar contract

Đây là hợp đồng mà phía C++ cài đặt theo. Nguồn thật là `tools/copilot/copilot.py` và
`tests/test_copilot.py`.

### Cờ dòng lệnh

| Cờ | Ý nghĩa |
|---|---|
| `<file>` | File log. Bắt buộc, trừ khi dùng `--store-key` / `--delete-key` / `--has-key`. |
| `--json` | Không `--ask`: danh sách findings (không đổi). Có `--ask`: MỘT object JSON (xem dưới). |
| `--ask QUESTION` | Hỏi mô hình. Sidecar nhận cả dạng cách (`--ask Q`) lẫn dạng `=` (`--ask=Q`); app luôn gửi dạng `=`, MỘT phần tử argv cho mỗi cờ có giá trị, để câu hỏi bắt đầu bằng `-` (vd "-3 dB có đủ không") không bị argparse đọc thành cờ. Áp tương tự cho `--provider`, `--model`, `--base-url`, `--timeout`. |
| `--provider {anthropic,openai_compatible}` | Mặc định `anthropic`. |
| `--model ID` | Mặc định `claude-opus-5-5` cho `anthropic`; **bắt buộc** cho `openai_compatible`. |
| `--base-url URL` | **Bắt buộc** cho `openai_compatible` (chỉ `http://` hoặc `https://`; ví dụ `https://api.openai.com/v1`, `http://localhost:11434/v1`); bị từ chối với `anthropic`. Sidecar gọi `POST {base-url}/chat/completions`. |
| `--timeout SECONDS` | Mặc định 120; áp cho **từng request HTTP**, không phải cả lượt hỏi: vòng công cụ tối đa 12 lượt nên tổng có thể lên tới 12 × timeout; giới hạn trên thật là lệnh kill 120 s của app. |
| `--store-key --provider P` | Đọc MỘT dòng từ **stdin**, mã hoá DPAPI, lưu. Không bao giờ nhận khoá qua argv. In một câu xác nhận không chứa khoá. |
| `--delete-key --provider P` | Xoá khoá của `P`. |
| `--has-key --provider P` | Thoát 0 nếu có khoá lưu cho `P`, 1 nếu không (blob hỏng tính là không, kèm một dòng stderr). |
| `--low-priority` | Như L2a. |

`--store-key`, `--delete-key`, `--has-key`, `--ask` loại trừ nhau.

### Mã thoát

| Mã | Nghĩa |
|---|---|
| 0 | Xong. |
| 1 | Chỉ `--has-key`: không có khoá. Không đường nào khác trả 1. |
| 2 | Log rỗng, không đọc được, sai UTF-8, hoặc có dòng JSON không phải object. |
| 3 | `--ask` thất bại (thiếu SDK, thiếu thông tin đăng nhập, kết nối, quá hạn, HTTP không 2xx, từ chối/`content_filter`, không có trả lời, vòng công cụ quá 12 lượt) hoặc thao tác kho khoá thất bại, hoặc bất kỳ ngoại lệ không lường trước nào (stderr chỉ có TÊN kiểu ngoại lệ, không bao giờ nội dung hay traceback). Lý do ở stderr, đã gỡ khoá. |
| 4 | Câu trả lời bị bộ lọc an toàn chặn. Câu cố định "Câu trả lời của mô hình đã bị chặn vì nó khuyên một việc có thể gây hú. Hãy hỏi lại, hoặc đổi mô hình." ra stdout (không `--json`: chữ thường; có `--json`: object bên dưới). Câu bị chặn không bao giờ được in. |
| 5 | Tham số sai (mọi lỗi cú pháp, thiếu/thừa `--model`/`--base-url`, URL không phải http/https, `--timeout` ≤ 0, khoá rỗng trên stdin). Cố ý dùng 5 thay cho mã 2 của argparse để không đụng "log rỗng". |

### Kết quả `--ask --json` (stdout, một object, UTF-8)

```json
{"answer": "…", "provider": "openai_compatible", "model": "llama3",
 "usage": {"input_tokens": 123, "output_tokens": 45},
 "findings": [ {"code": "…", "severity": "…", "title": "…", "advice": "…", "evidence": {}} ]}
```

`usage.*` là `null` khi nhà cung cấp không báo. Với `--ask --json`, stdout LUÔN là đúng MỘT object
JSON khi mã thoát là 0, 3 hoặc 4, và rỗng với mã 2 và 5:

| Mã | stdout |
|---|---|
| 0 | object ở trên, thêm `"blocked": false` |
| 3 | `{"error": "<lý do một dòng, đã gỡ khoá, y hệt dòng stderr>"}`; ngoại lệ không lường trước: `{"error": "Lỗi không lường trước trong trợ lý (<TênKiểu>)"}` |
| 4 | `{"blocked": true, "message": "<câu cố định ở bảng mã thoát>"}` — không bao giờ có câu trả lời của mô hình |
| 2, 5 | rỗng |

Chuỗi `error` đi qua cùng bộ gỡ khoá với stderr (khoá đã lưu, `ANTHROPIC_API_KEY`,
`ANTHROPIC_AUTH_TOKEN`, `OPENAI_API_KEY`). Không `--json`, `--ask` in findings, một dòng trống,
rồi câu trả lời; mọi thứ khác không đổi. (Chế độ findings `--json` không `--ask` vẫn in một mảng
JSON như cũ.)

### Giới hạn và an toàn gửi khoá

- Thân phản hồi HTTP tối đa 4 MiB (quá = lỗi, mã 3). Mỗi kết quả công cụ trả cho mô hình tối đa
  64 KiB: danh sách bị cắt còn tiền tố dài nhất vừa khung, bọc thành
  `{"truncated": true, "total": N, "items": [...]}` (luôn là JSON hợp lệ).
- `--base-url` `http://` tới máy KHÔNG phải loopback (`localhost`, `127.0.0.0/8`, `::1`) chỉ được
  phép khi không có khoá nào sẽ được gửi; nếu có khoá thì từ chối trước mọi I/O mạng, mã 3.
  `https://` không đổi.
- Bộ lọc an toàn chuẩn hoá (NFKC, bỏ ký tự định dạng, casefold, bỏ dấu, đ→d) rồi chặn động từ
  "tăng" + đối tượng mức (gain, âm lượng, fader, ...) trong cửa sổ 4 từ, và "+N dB"; lời phủ định
  ngay trước động từ (đừng, không (nên), do not, never, ...) thì cho qua. Nghi ngờ thì chặn.
- Giới hạn đã biết: vùng nhớ do app sở hữu được xoá sau khi mã hoá DPAPI, nhưng các bản sao khoá nằm thoáng qua trong `juce::String` / `juce::TextEditor` được giải phóng mà không bị ghi đè số 0.

### Kho khoá

- File: `%APPDATA%\AZSoundtech\HandsFree\copilot-credentials.json`, UTF-8:
  `{"version": 1, "profiles": {"<provider>": {"blob": "<base64>"}}}`, ghi nguyên tử (file tạm rồi thay). `--store-key` gặp file không đọc được thì `os.replace` nó sang `copilot-credentials.json.corrupt` (đè bản `.corrupt` cũ, không bao giờ xoá), in một dòng stderr, rồi mới ghi file mới.
  `<provider>` là `anthropic` hoặc `openai_compatible`.
- `blob` = base64 của đầu ra `CryptProtectData` (theo tài khoản Windows, cờ
  `CRYPTPROTECT_UI_FORBIDDEN`) trên các byte UTF-8 của khoá, với **optional entropy** là
  đúng các byte ASCII `AZSoundtech.HandsFree.Copilot.v1` (hằng `DPAPI_ENTROPY` trong
  `copilot.py`; bên C++ dùng đúng chuỗi này).
- Biến môi trường `HANDSFREE_COPILOT_CREDENTIALS_DIR` thay thư mục (dành cho test).
- Thứ tự tìm khoá: khoá lưu cho provider đó; rồi `anthropic` → mặc định của SDK (biến môi
  trường hoặc profile `ant`), `openai_compatible` → biến `OPENAI_API_KEY`; riêng
  `openai_compatible` được phép không có khoá (Ollama tại máy) và khi đó không gửi header
  `Authorization`. Blob hỏng = "không có khoá" + một dòng stderr.
- Khoá không bao giờ xuất hiện ở stdout, stderr, thông báo lỗi hay log. Đây là bộ lọc cụm từ
  chứ không phải bảo đảm tuyệt đối: `violates_safety` bắt các cách nói phổ biến (tiếng Việt
  và tiếng Anh), không bắt mọi cách diễn đạt.
- `openai_compatible` không theo chuyển hướng HTTP (3xx là lỗi, mã 3) để header
  `Authorization` không bay sang máy chủ khác.
