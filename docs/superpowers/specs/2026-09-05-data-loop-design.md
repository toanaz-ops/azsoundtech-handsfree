# Lane D — Vòng dữ liệu: nhãn oan/đúng và log session có cấu trúc

**Ngày:** 2026-09-05. **Roadmap:** [`2026-09-04-anti-feedback-v2-roadmap.md`](2026-09-04-anti-feedback-v2-roadmap.md).
**Trạng thái:** đã thực thi trên nhánh `feat/data-loop` (2026-09-05, 8 task,
suite 432/432 sau fix wave 2026-09-05); các chỗ lệch khỏi bản spec này — kể cả việc hợp nhất với
lane S đã merge trước — nằm trong
[`../plans/2026-09-05-data-loop.md`](../plans/2026-09-05-data-loop.md):
mục "Hợp nhất hai phiên" (đầu file) và bảng "Amendments to the spec" (A-1..A-10).
**Đụng audio path:** không.
**Mức thay đổi level dự kiến:** 0 dB. Lane này chỉ quan sát và ghi.

## 1. Vì sao lane này đi trước mọi thứ "AI"

Lane C (classifier), lane L (LLM copilot) và lane T (threshold sweep)
đều cần hai thứ mà app chưa sinh ra:

- **Nhãn**: notch này là hú thật hay cắt oan nốt nhạc. Chỉ người vận
  hành biết, và chỉ biết trong vài giây sau khi nó xảy ra.
- **Ngữ cảnh**: phổ tại lúc quyết định, phổ 250 ms trước đó (rise
  reference), điểm số, tuning đang dùng, slot, làn, mode.

Hiện `NotchController` ghi một dòng log chữ mỗi lần đặt notch. Dòng đó
không parse được, không có phổ, không có nhãn.

## 2. Mục tiêu và phi mục tiêu

Mục tiêu:

1. Hai nút trên mỗi dòng của ACTIVE NOTCHES: `GOOD` và `FALSE`. `FALSE`
   đồng thời **xóa notch đó** (đó là điều người vận hành muốn khi bấm)
   và ghi nhãn. `GOOD` chỉ ghi nhãn.
2. Log session JSONL tại `%APPDATA%/AZSoundtech/HandsFree/logs/`, một
   file mỗi lần chạy app, ghi từ **thread riêng**, không bao giờ từ
   audio thread.
3. Mọi sự kiện đặt/xóa notch mang ngữ cảnh phổ đủ cho huấn luyện.
4. Một script đọc log ra bảng tóm tắt để owner và tester nhìn được ngay.

Phi mục tiêu:

- Không ghi audio thô, không ghi tên tester, không gửi đi đâu. Log nằm
  trên máy; tester tự copy gửi khi được hỏi.
- Không xây pipeline huấn luyện. Lane C làm.
- Không hiển thị thống kê trong app.

## 3. Thiết kế

### 3.1 `SessionLogger` (mới, `src/app/SessionLogger.h/.cpp`)

```cpp
class SessionLogger
{
public:
    struct Config { juce::File directory; int keepFiles = 30; };

    explicit SessionLogger (Config c);
    ~SessionLogger();                 // flush + session_end

    void start (const juce::var& sessionHeader);   // mở file, ghi session_start
    void stop();                                   // ghi session_end, đóng

    // Thread-safe, gọi từ detector thread hoặc message thread. KHÔNG gọi
    // từ audio thread: có mutex và có thể alloc.
    void log (const juce::var& event);

    juce::File currentFile() const;
    std::uint64_t droppedEvents() const;
};
```

Bên trong: `std::deque<juce::String>` dưới mutex, cap 4096 dòng chờ;
quá cap thì bỏ và đếm `droppedEvents` (không bao giờ chặn caller).
Thread ghi (`juce::Thread`) thức mỗi 1 s hoặc khi `stop()`, ghi hết deque
ra file bằng `FileOutputStream` append, `flush()`. Tên file
`session-YYYYMMDD-HHMMSS.jsonl`. Sau khi mở file mới, xóa file cũ nhất
cho đến khi còn `keepFiles` (xóa **sau** khi file mới ghi thành công,
cùng nguyên tắc với prune của release script).

Mọi dòng là một object JSON, luôn có `"t"` (ms kể từ session_start,
đồng hồ steady) và `"ev"` (tên sự kiện). Encoding UTF-8, không BOM.

`start()` và `stop()` ghi `session_start` / `session_end` **trực tiếp
xuống file**, không qua deque và không bao giờ bị đếm vào
`droppedEvents`. Vì thế bất biến "số dòng trong file + droppedEvents =
số lần `log()` + 2" luôn đúng.

**Phiên bản app**: `JUCE_APPLICATION_VERSION_STRING` chỉ được định
nghĩa cho target `HandsFree` (`CMakeLists.txt:168-174`), còn
`MainComponent.cpp` được biên dịch cả vào `HandsFreeTests`. Vì vậy
`MainComponent` **không** được tham chiếu macro đó. `main.cpp` (chỉ
thuộc target app) gọi `mainComponent.setAppVersion
(JUCE_APPLICATION_VERSION_STRING)` trước khi start; mặc định trong
`MainComponent` là `"0.0.0-unset"`. Test thấy chuỗi mặc định.

**Thứ tự hủy**: `SessionLogger` là member của `MainComponent`, khai báo
**trước** `notchControllers_` (cùng lý do `systemClock_` đứng trước:
member khai báo trước bị hủy sau, nên logger còn sống khi các detector
thread được join trong `~NotchController`). Ngoài ra `~MainComponent`
gọi `setEventSink(nullptr)` cho mọi controller **trước** mọi việc khác,
rồi `sessionLogger_.stop()`.

### 3.2 Sự kiện

| `ev` | Từ đâu | Trường |
|---|---|---|
| `session_start` | MainComponent | `app_version`, `os`, `device`, `sample_rate`, `buffer_size`, `slots[]` (`SlotConfig` + `linked` nếu lane S đã merge) |
| `mode` | MainComponent | `mode`: `bypass` / `auto` / `soundcheck` |
| `tuning` | MainComponent | `slot` (−1 = global), `rise_ms`, `persist`, `q`, `depth_db`, `thr` |
| `notch_set` | NotchController (detector thread) | `slot`, `lane`, `index`, `hz`, `q`, `depth_db`, `origin`, `score`, `peakiness`, `rise`, `novelty`, `penalty`, `persist_needed`, `thr`, `ctx` |
| `notch_retune` | NotchController (detector thread) | `slot`, `lane`, `index`, `hz`, `q`, `depth_db` (mới), `from_db` (cũ), `origin`, `reason`: `deepen` / `release` / `reclamp` / `ceiling`, `age_ms` |
| `notch_clear` | NotchController | `slot`, `lane`, `index`, `hz`, `reason`: `auto_release` / `manual` / `clear_all` / `width_change` / `verdict_false`, `age_ms` |
| `verdict` | NotchListPanel → MainComponent | `slot`, `lane`, `index`, `hz`, `verdict`: `good` / `false`, `age_ms` |
| `preset_load` | MainComponent (fix round 2, 1.2.0) | `file` (chỉ tên), `adopted`, `skipped`, `ceiling_applied` (bool, luôn ghi), `q`/`depth_db` (trần vừa áp, chỉ khi `ceiling_applied`) |
| `session_end` | SessionLogger | `dropped_events` |

`notch_retune` (lane G, 1.2.0) là **cập nhật**, không phải đóng/mở: một notch
có thể retune nhiều lần giữa `notch_set` và `notch_clear` của nó.
`tools/logstats.py` xử lý nó bằng cách sửa bản ghi đang mở (`depth_db`,
`deepest_db`, `retunes`) và **không** đóng bản ghi — nếu đóng thì một lần đào
sâu ở mốc 300 ms sẽ biến mọi notch được đào thành "notch sống 300 ms", tức một
false positive giả trên mọi dòng thống kê. Cờ `--expect-retunes` kiểm chứng số
lần retune trong fixture ctest.

Reader là một chuỗi `if/elif` trên `ev` và **bỏ qua tên lạ**, nên một file log
cũ đọc bằng tool mới vẫn chạy đúng, và một `ev` thêm sau này cũng không làm
hỏng reader cũ. **Một file log ghi bởi build nhánh TRƯỚC `af5201e` thì hỏng đối
với logstats**: lúc đó `MainComponent` còn ghi mọi Retune thành `notch_clear`,
nên tool đóng bản ghi ở lần đào sâu đầu tiên. Không bản nào như vậy được phát
hành — chỉ cần biết khi đọc log nội bộ cũ.

`preset_load.ceiling_applied` (fix round 2, 1.2.0) không phải readout trung
tính: khi `true`, file vừa nạp có thể đã kéo mọi notch Detector đang sâu hơn
trần mới lên tới trần đó ngay ở tick detector kế tiếp (một trần NÔNG hơn trần
cũ là +14 dB có thể tại một bin đang hú; trần SÂU hơn là 0 dB). `q`/`depth_db`
trong sự kiện ghi trần đã áp SAU clamp, không phải số trong file.

`ctx` của `notch_set` là ngữ cảnh phổ:

```json
"ctx": {
  "bins": 1025, "bin_hz": 23.4375,
  "now":  [ ...1025 float, 3 chữ số có nghĩa... ],
  "ref":  [ ...frame gần nhất ≥ rise_ms trước, cùng định dạng... ],
  "other_lane_now": [ ... ]        // chỉ khi slot stereo và làn kia có phổ cùng vòng
}
```

~6 KB mỗi mảng; một sự kiện `notch_set` ~12–18 KB. **Đo lại 2026-09-05
(fix wave lane D): 19,5–19,7 KB/dòng** cho slot stereo có đủ `now` + `ref` +
`other_lane_now`; ở chế độ LINK một lần xác nhận phát hai Set (~40 KB). Một
show 3 giờ với ~300 lần đặt notch → ~6–14 MB. Chấp nhận được; `keepFiles = 30` chặn tích lũy.

Để có các thành phần điểm tách riêng, `CandidateScorer::scoreCandidate`
thêm một overload trả:

```cpp
struct ScoreBreakdown {
    float rawPeakiness;   // Candidate::peakiness, tỉ số thô không chặn trên
    float pNorm, rNorm, mNorm;   // ĐÚNG ba biến chuẩn hóa 0..1 hiện có trong scoreCandidate
    float penalty;        // 1.0 hoặc kHarmonicPenalty
    float score;          // pNorm × rNorm × mNorm × penalty, bit-exact với overload cũ
    const float* refFrame;    // con trỏ vào history_: frame scorer THỰC SỰ so sánh
    double refAgeMs;          // tuổi của frame đó; nullptr/0 khi chưa có history
};
```

Overload cũ gọi overload mới và trả `.score`. Không đổi số học.

`ctx.ref` trong `notch_set` là **`refFrame` của chính lần chấm điểm
đó**, không phải một frame tra lại theo `rise_ms`. Lý do: scorer hiện
chọn frame mới nhất có tuổi ≥ `0.45 × riseReferenceMs`
(`CandidateScorer.cpp:64-95`), tức với mặc định 250 ms frame so sánh có
thể chỉ ~112 ms tuổi. Log một frame 250 ms tuổi sẽ mô tả sai trục rise
mà scorer đã dùng. JSON ghi thêm `"ref_age_ms"`.

### 3.3 Móc vào `NotchController`

```cpp
struct NotchEvent { /* các trường của notch_set / notch_clear */ };
using EventSink = std::function<void (const NotchEvent&)>;
void setEventSink (EventSink sink);   // gọi trước start(); nullptr = tắt
```

**Sink không bao giờ được gọi khi đang giữ `modelMutex_`.** Cơ chế:
controller có `std::vector<NotchEvent> eventOutbox_` (reserve trước,
bảo vệ bởi `modelMutex_`). `pushClearLocked` và điểm đặt notch chỉ
**ghi** một `NotchEvent` vào outbox đó. `flushOutbox()` (detector
thread, đã chạy ngoài lock) swap outbox ra một vector cục bộ rồi gọi
sink cho từng event. `stop()` flush lần cuối trước khi join. Hệ quả:
mọi event đến sink trên detector thread, trễ tối đa một chu kỳ poll
(5 ms); `clearNotch` từ message thread không bị kéo dài bởi I/O của
logger. Với `notch_set`, mảng `ctx` (2–3 × 1025 float) được copy vào
`NotchEvent` ngay lúc ghi outbox vì `refFrame` trỏ vào history sẽ bị
ghi đè; copy này là memcpy, không alloc (mảng cố định trong struct;
`eventOutbox_` reserve 64 phần tử lúc khởi tạo, quá 64 thì drop và đếm
`droppedEvents` của controller).

Sink do MainComponent gắn, chuyển sang `juce::var` và gọi
`SessionLogger::log`. Chi phí: alloc mảng JSON 1025 số mỗi notch đặt
(không phải mỗi frame), trên thread không realtime. Dòng log chữ cũ
giữ nguyên.

`clearNotch` thêm tham số `ClearReason` (enum: `Manual`, `ClearAll`,
`AutoRelease`, `WidthChange`, `VerdictFalse`, `PartialApplyUnwind`),
mặc định `Manual`. Hai chỗ unwind nội bộ (`adoptPreset` và
`processSpectrumForDetection` khi áp được một phần) **phải** truyền
`PartialApplyUnwind` tường minh: chúng không phải hành động người vận
hành, nhãn `manual` ở đó làm bẩn dữ liệu huấn luyện.

**`setWidth` có logic mới**: hiện nó chỉ ghi `width_`
(`NotchController.cpp:41-46`), notch làn 1 mồ côi trong model khi thu
về mono. Lane này (hoặc lane S, tùy lane nào merge trước; lane sau
không làm lại) thêm: khi `lanes < width_`, mọi notch active trên làn
ngoài phạm vi mới được `pushClearLocked` với `WidthChange` **trước**
khi ghi `width_`. Tiền đề "thread đã dừng" giữ nguyên, nên các event
này nằm trong `eventOutbox_` đến khi thread chạy lại hoặc `stop()`
flush; trong thực tế `setWidth` luôn đi kèm restart nên event ra trong
vòng vài ms.

### 3.4 GUI

`NotchListPanel` hiện không có component con theo dòng: `rows_` là
`std::vector<RowText>` bị xóa và dựng lại 4 lần/giây, `paint()` chỉ vẽ
chữ (`NotchListPanel.cpp:90-106, 232-355`). Nút bấm phải là
`juce::Component` con thật để nhận click, nên lane này là **ngoại lệ có
chủ ý** của kỷ luật "paint chỉ vẽ member dựng sẵn", với cơ chế vòng đời
riêng:

- `std::map<std::uint64_t, RowButtons> buttons_` khóa theo đúng
  `identityKey(channel, index, hz)` mà `sightings_` đang dùng.
  `RowButtons { std::unique_ptr<juce::TextButton> good, bad; Verdict
  state; }`.
- `refreshFromSnapshot()`: identity mới thấy → tạo cặp nút
  (`addAndMakeVisible`), identity biến mất khỏi tracking → hủy cặp nút
  (`removeChildComponent` rồi reset). Không tạo lại nút cho identity đã
  có, nên click không bị "nuốt" bởi rebuild.
- Vị trí nút đặt trong `refreshFromSnapshot()` và `resized()` theo
  hàng của identity; bố cục cột thêm một cột `VERDICT` cố định
  `kColVerdictW` sau cột STATUS, cột STATUS co lại tương ứng.
- Nút là `TextButton` chữ `GOOD` / `FALSE` theo AzTheme (không dùng ký
  tự ✓ ✗: bài học mojibake middle-dot 2026-08-25). Sau khi bấm, cặp nút
  ẩn đi và ô VERDICT vẽ chữ trạng thái bằng `paint()` như các cột khác.
- Test `NoAllocationInPaint` hiện có (nếu áp cho panel này) phải vẫn
  xanh: alloc chỉ xảy ra trong `refreshFromSnapshot()`.

Callback:

```cpp
std::function<void (int slot, int lane, int index, float hz, bool good)> onVerdict;
```

Sau khi bấm, dòng hiện chữ `GOOD` hoặc `FALSE` thay cho hai nút cho đến
khi notch biến mất (tracking theo identity key đã có). Bấm `FALSE` →
MainComponent ghi `verdict` rồi gọi `controller.clearNotch(lane, index,
ClearReason::VerdictFalse)`. Panel vẫn không tự ra lệnh (giữ kỷ luật
"display only", lệnh đi qua MainComponent).

Lane S merge trước: cột LANE đã có, `slot`/`lane` lấy từ snapshot.
Nếu D đi trước S: `lane = channel` của `SnapshotNotch`.

Ảnh render bắt buộc: `console-live.png` có một dòng chưa chấm, một dòng
`GOOD`, một dòng `FALSE` vừa bấm (notch đang biến mất).

### 3.5 Tool tóm tắt (`tools/logstats.py`)

Python 3, stdlib only. `python tools/logstats.py <file.jsonl>` in:

- thời lượng session, device, sample rate, mode timeline;
- bảng notch: slot, lane, Hz, origin, age, verdict, reason clear;
- tần số tái phát: nhóm Hz ±1 bin, đếm số lần đặt;
- tỉ lệ `false` / tổng verdict, và tỉ lệ notch **không** được chấm.

Đây là "thứ người ta chạy được" của lane này (rule 12).

### 3.6 Docs

`docs/KY-THUAT-CHONG-HU.md` thêm §8 "Log session và nhãn" (đường dẫn,
sự kiện, cam kết không ghi audio). `docs/GIOI-THIEU.md` thêm mục "Chấm
notch để app học" với hai câu hướng dẫn tester.

## 4. Kiểm thử

SessionLogger (`test_sessionlogger.cpp`, mới):
1. `start` tạo file, `session_start` là dòng đầu, `stop` ghi
   `session_end` là dòng cuối; mỗi dòng parse được bằng `JSON::parse`.
2. 5000 `log()` liên tiếp không chặn caller quá 1 ms mỗi lần
   (đo thô); `droppedEvents` > 0 và tổng dòng + dropped = 5000 + 2.
3. `keepFiles = 3`: tạo 5 session → còn 3 file mới nhất.
4. Thư mục không tạo được → `start` trả về và `log` là no-op, không
   throw, không crash.

CandidateScorer:
5. `ScoreBreakdown.score == scoreCandidate(...)` cũ cho cùng đầu vào
   (bit-exact), và `pNorm × rNorm × mNorm × penalty == score`.
6. `refFrame` trỏ đúng frame mà trục rise đã dùng: với rise 250 ms và
   history có frame ở 100/120/300 ms tuổi, `refAgeMs == 120` (mới nhất
   ≥ 0.45 × 250 = 112.5).

NotchController:
7. Sink nhận `notch_set` với `ctx.now` bằng frame vừa phân tích và
   `ctx.ref` bằng `refFrame` của lần chấm điểm đó, `ref_age_ms` khớp.
8. Sink nhận `notch_clear` với đúng `reason` cho sáu đường: auto-release,
   `clearNotch` mặc định, `clearAll`, `setWidth(1)` với notch làn 1
   active, `VerdictFalse`, và unwind nội bộ (`PartialApplyUnwind`, ép
   bằng cách cho `setNotch` làn 1 thất bại qua sample rate).
9. Sink được gọi **ngoài** `modelMutex_`: sink thử `lock()` mutex qua
   test hook hoặc gọi `clearNotch` từ trong sink; không deadlock, không
   re-entrancy lỗi.
10. Không sink → không gọi; `eventOutbox_` không phình.
11. `stop()` flush event còn lại trước khi join: đặt notch rồi `stop()`
    ngay, sink vẫn nhận `notch_set`.

GUI:
12. Bấm `FALSE` → `onVerdict(good=false)` và ô VERDICT đổi trạng thái;
    bấm `GOOD` → `onVerdict(good=true)`, notch còn nguyên.
13. Nút tồn tại qua nhiều lần `refreshFromSnapshot()` với cùng identity
    (cùng con trỏ component), và bị hủy khi identity hết tracking.
14. `MainComponent`: `onVerdict(false)` gọi `clearNotch` với
    `VerdictFalse` và logger nhận `verdict` rồi `notch_clear`.
15. `MainComponent` hủy an toàn khi detector thread đang đặt notch:
    test dựng, start controller với tín hiệu hú, hủy ngay; không crash
    (chạy 20 lần).

Tool:
16. `tools/logstats.py` chạy trên fixture `tests/fixtures/session-sample.jsonl`
    ra đúng số notch và tỉ lệ; chạy trong ctest qua `add_test` gọi
    `python` nếu có, `SKIP` nếu không.

## 5. Việc ngoài code

- Release alpha `-Part patch`. Note tester: "Bấm FALSE khi app cắt oan
  nốt nhạc, GOOD khi cắt đúng hú. Log ở `%APPDATA%\AZSoundtech\HandsFree\logs`,
  gửi file khi được hỏi. Không có audio trong log."
- Ghi vào roadmap: lane C mở khi có ≥ 300 verdict từ ≥ 3 session khác
  nhau.

## 6. Quyết định chốt

| # | Quyết định | Lý do |
|---|---|---|
| D-1 | JSONL, không SQLite/CSV | Append an toàn khi crash; mỗi dòng độc lập; Python đọc không cần thư viện |
| D-2 | `FALSE` xóa notch luôn | Đó là hành động người vận hành muốn; tách hai nút là thêm một cú bấm giữa show |
| D-3 | Phổ lưu 3 chữ số có nghĩa, không base64 | Người đọc được; kích thước chấp nhận được |
| D-4 | Cap deque 4096, drop và đếm | Không bao giờ chặn detector thread |
| D-5 | Không ghi audio | Riêng tư của tester và khán giả; phổ đủ cho huấn luyện |
| D-6 | Sink chỉ gọi từ `flushOutbox()`, ngoài `modelMutex_` | Message thread gọi `clearNotch` không được chờ I/O của logger; không re-entrancy |
| D-7 | `ctx.ref` = frame scorer thực dùng, không tra lại theo `rise_ms` | Scorer dùng 0.45 × rise; log frame khác là mô tả sai dữ liệu huấn luyện |
| D-8 | Version app đi từ `main.cpp` vào `MainComponent`, không dùng macro trong file dùng chung | `HandsFreeTests` biên dịch `MainComponent.cpp` mà không có macro đó |
| D-9 | Nút verdict là component con theo identity, ngoại lệ có chủ ý của kỷ luật paint | Không có cách nào khác để nhận click; vòng đời gắn với `sightings_` có sẵn |

## 7. Phản biện đã xử lý

Reviewer độc lập (2026-09-05, agent chỉ đọc) tìm ra 3 BLOCKER, 3
SHOULD-FIX, 3 điểm mơ hồ; tất cả đã đưa vào các mục 3.1–3.4 và 4 ở
trên. Hai điều xác nhận: không có facility log nào sẵn có để tái dùng
(`juce::Logger` chưa có sink), và lane này không chạm audio callback
hay chính sách đặt notch.
