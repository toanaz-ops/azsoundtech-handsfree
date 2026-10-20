# Lane L1 — Copilot cho soundman: phát hiện từ log session + hỏi Claude (offline)

**Ngày:** 2026-10-20. **Roadmap:** [`2026-09-04-anti-feedback-v2-roadmap.md`](2026-09-04-anti-feedback-v2-roadmap.md) (lane L, cần D — đã hạ cánh 1.1.2).
**Sổ quyết định:** [`../decisions/2026-10-20-lane-l-copilot.md`](../decisions/2026-10-20-lane-l-copilot.md) (Q1–Q5 điều phối chốt tạm).
**Đụng audio path:** **không.** App không đổi một dòng nào; `git diff origin/main -- src` rỗng. Không có thay đổi mức.
**Release:** không cần. Công cụ nằm trong repo, không vào installer.

## 1. Vấn đề

Log session của lane D ghi đủ để thấy một buổi diễn có vấn đề: cùng một tần số hú lại,
notch chạm trần, notch dồn dập. `logstats.py` in ra bảng, nhưng soundman phải tự đọc
bảng và tự suy ra nên làm gì. Copilot biến bảng đó thành **lời khuyên bằng câu
người**, và cho phép hỏi thêm.

## 2. Ranh giới

- Copilot là công cụ dòng lệnh, chạy **sau buổi diễn hoặc trong giờ nghỉ**, trên file
  `%APPDATA%/AZSoundtech/HandsFree/logs/session-*.jsonl`.
- Không chạy trong app, không có trên đường âm thanh, không ghi gì ra ngoài stdout.
- Phần phát hiện chỉ dùng stdlib và không cần mạng. Chỉ `--ask` mới gọi mạng, tới
  Claude API.

## 3. Cách dùng

```
python tools/copilot/copilot.py <session.jsonl>                 # phát hiện
python tools/copilot/copilot.py <session.jsonl> --json          # máy đọc
python tools/copilot/copilot.py <session.jsonl> --ask "Vì sao 1 kHz hú hoài?"
```

`--ask` cần gói `anthropic` cùng thông tin đăng nhập (`ANTHROPIC_API_KEY` hoặc
`ant auth login`). Nếu thiếu, công cụ in lý do ra stderr và trả mã 3; phần phát hiện
vẫn được in trước đó.

## 4. Luật phát hiện (`findings()`)

Mỗi phát hiện có dạng `{code, severity, title, advice, evidence}`, sắp theo mức
`alert` → `warn` → `info`. Ngưỡng là hằng số đặt tên ở đầu file và được test ghim.

| code | Khi nào | Mức | Lời khuyên (tóm tắt) |
|---|---|---|---|
| `recurrence` | cùng tần số (±1 bin) bị đặt ≥ 2 lần | info; ≥ 3 lần thì warn | kiểm tra hướng mic so với monitor, giảm dải đó trên EQ monitor, lưu vào preset |
| `ceiling` | notch của detector có độ sâu sâu nhất ≤ trần + 0,5 dB. Trần là `tuning.depth_db` sâu nhất của slot; nếu slot không có thì lấy giá trị toàn cục (slot −1). Notch bị đánh dấu oan thì bỏ qua | alert | hạ gain monitor hoặc kênh đó; **đừng tăng trần** |
| `stacked` | hai notch cùng slot và cùng làn, cùng sống một lúc, cách nhau ≤ 1/6 octave | warn | một nhát EQ rộng trên mixer |
| `burst` | ≥ 4 notch do detector đặt trong 60 s | alert | hạ tổng gain hoặc monitor 2–3 dB, chạy ĐO PHÒNG lại |
| `false_rate` | ≥ 3 notch đã đánh giá và tỉ lệ oan ≥ 30 % | warn | thử preset Safe hoặc Music, gửi log |
| `unjudged` | ≥ 3 notch và ≥ 50 % chưa có nhãn | info | bấm oan/đúng (dữ liệu cho lane C) |
| `dropped` | `session_end.dropped_events` > 0 | warn | log thiếu sự kiện nên kết luận có thể thiếu |

### 4.1 Đọc log

Copilot dùng lại `logstats.summarise()`, nên chỉ có một bộ đọc log cho cả hai công cụ.
Các tính chất của bộ đọc đó vẫn giữ: `notch_retune` không đóng notch, và tên sự kiện
lạ thì bị bỏ qua.

### 4.2 Ràng buộc an toàn

Đây là âm thanh trực tiếp ra loa PA. Không luật nào, và không câu trả lời nào của
`--ask`, được khuyên **tăng gain, tăng trần, tắt chống hú hay bỏ một giới hạn bảo
vệ**. Ràng buộc nằm ở hai chỗ:

- test `test_advice_never_says_raise_gain` kích hoạt cùng lúc mọi luật và kiểm từng
  câu;
- system prompt của `--ask`.

## 5. `--ask`

- Dùng Tool Runner của SDK Python với mô hình `claude-opus-5-5`, adaptive thinking,
  effort `medium`, `max_iterations=12`. Refusal fallback phía server bật bằng
  `betas=["server-side-fallback-2026-07-01"]` và `fallbacks="default"`.
- Có bốn tool, đều **chỉ đọc**, cùng đóng trên một log đã parse:

| Tool | Trả về |
|---|---|
| `session_summary` | header, thời lượng, mode, các tổng |
| `get_findings` | đúng danh sách của §4 |
| `list_notches(slot)` | các notch, đánh số từ 1 |
| `notch_context(number)` | score, peakiness và các bin phổ quanh notch đó |

- Lỗi thì in ra stderr và trả mã 3. Các trường hợp:
  - thiếu SDK;
  - thiếu thông tin đăng nhập: SDK ném `TypeError` khi không tìm thấy nguồn nào, còn
    401 là `AuthenticationError`;
  - mất kết nối;
  - API trả lỗi status;
  - model từ chối (`stop_reason == "refusal"`).

## 6. Test

- `tests/test_copilot.py` (stdlib `unittest`) chạy qua ctest với tên
  `copilot_findings`. Nếu máy không có Python 3 thì test bị SKIP.
- Có 21 test cho các luật, ngưỡng, thứ tự mức, fixture lane D và CLI `--json`. Thêm 2
  test gọi trực tiếp 4 tool khi SDK có mặt; các test này không gửi request nào.
- **Chưa kiểm:** một lượt `--ask` thật, vì phiên làm việc không có khoá API. Với khoá
  giả, request tới được API và nhận 401 đúng như mong đợi.

## 7. Bước sau (không thuộc L1)

- Chạy trên log alpha thật. Chỉnh ngưỡng §4 khi có số liệu (`p005-hw`).
- Cân nhắc đưa vào GUI (Q1-2) chỉ khi L1 cho thấy lời khuyên có ích. Việc đó cần lời
  owner về khoá API và mạng.
- Phân tích nhiều session theo venue: nối với tầng "ngày" của nghiên cứu 04/09.
