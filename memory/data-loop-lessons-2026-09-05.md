# Bài học từ lane D (vòng dữ liệu: log session + nhãn GOOD/FALSE) — 2026-09-05

**Bối cảnh:** lane D thêm `SessionLogger` (JSONL, thread riêng), nút GOOD/FALSE
trên `NotchListPanel`, và `tools/logstats.py`. 8 task, suite 402 → 430, không
đụng audio path (0 dB). Spec: `docs/superpowers/specs/2026-09-05-data-loop-design.md`.
Plan + amendments A-1..A-10: `docs/superpowers/plans/2026-09-05-data-loop.md`.

## 1. `juce::JSON::toString` in double tới 18 chữ số — làm tròn TRƯỚC khi vào `var`

`serialiseDouble` của JUCE không giới hạn số chữ số: một magnitude 0.00123 có
thể in ra `0.001229999999999999`. Không sửa được ở bước serialise vì lúc đó
giá trị đã là double thật trong `var`. Phải làm tròn về 3 chữ số có nghĩa
(`roundSig3`) **trước khi** nhét vào `var`, không phải format lại chuỗi sau.

## 2. `SessionLogger::log(const var&)` phải clone trước khi stamp `t` — sửa `var` const vẫn mutate được caller

`getDynamicObject()` trên một `var` là non-const về mặt kỹ thuật của kiểu trả
về (con trỏ tới `DynamicObject` dùng chung), nên gọi nó trên một tham số
`const var&` rồi set thêm property `"t"` **sửa luôn object của caller** — dù
chữ ký hàm là const-correct trên giấy. Phải `var::clone()` (deep copy) trước
khi thêm `t`, nếu không caller giữ một `var` đã bị logger tự ý thêm field.

## 3. Không bao giờ start logger trong ctor của `MainComponent`

26 test hiện có cộng `HandsFreeSnapshot` đều dựng `MainComponent` để test
layout/logic, không phải để chạy một phiên thật. Nếu logger start trong ctor,
mỗi lần chạy test suite sẽ ghi — và **prune** — thư mục log thật ở
`%APPDATA%`. Logger chỉ start từ `startAudio()` (production) hoặc
`startSessionLog(dir)` test-only, không bao giờ từ constructor.

## 4. Test headless phải gọi `button->onClick()`, không phải `triggerClick()`

`triggerClick()` post một message async qua `MessageManager` để mô phỏng
tương tác chuột thật; test suite headless không bơm message loop đó nên
message không bao giờ chạy — test tưởng nút không làm gì. Gọi thẳng
`onClick()` (lambda) là cách duy nhất thấy hiệu ứng ngay trong cùng frame,
và cũng là lý do `tools/snapshot.cpp` dùng `goodButtonForTest(...)->onClick()`
thay vì mô phỏng click chuột.

## 5. Nửa cái reset còn tệ hơn không reset gì (widen 1→2, A-9)

Amendment A-9 định reset `Detector` của làn 1 khi slot widen 1→2 để dọn cửa
sổ phân tích cũ. Nhưng `CandidateScorer` giữ history riêng (EMA rise/novelty)
không bị đụng tới — làn vừa quay lại có `Detector` sạch nhưng `CandidateScorer`
bão hòa 0 trên hai trục rise/novelty trong ~200 ms, tức **dễ bắn notch hơn**
hành vi 1.1.1 đang chạy, ngược hẳn ý định "làm sạch trạng thái cũ". Phán
quyết điều phối viên: RÚT hành vi này khỏi lane D (owner constraint "0 dB,
không đụng audio/detection path" đã đủ lý do); có nên gate `riseReferenceMs`
cho làn tái nhập hay không là quyết định owner còn treo cho lane S. Bài học
chung: reset một phần trạng thái liên kết (Detector) mà bỏ qua phần còn lại
(CandidateScorer history) không "an toàn hơn" — nó đổi hành vi theo hướng
không ai chủ định.

## 6. Hai bẫy quy trình nhiều-agent trong ngày này

- **Một prompt lane D dán vào hai session** tạo ra hai session song song cùng
  nhận việc trên MỘT lane — `list_sessions` là cách phát hiện; handoff/ledger
  nên ghi rõ session nào đang sở hữu lane nào để tránh hai bên cùng sửa một
  file.
- **Một worktree app tái dùng tên thư mục cũ** (kiểu
  `chore-infra-prune-orphans-…`) bị một session khác coi là orphan và prune
  mất. Trước khi prune bất kỳ worktree nào: `git worktree list` **và** kiểm
  tra cwd của mọi session đang chạy, không chỉ suy từ tên thư mục.
