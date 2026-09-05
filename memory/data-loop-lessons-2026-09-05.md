---
name: data-loop-lessons
description: lane D (data loop — SessionLogger, GOOD/FALSE verdicts, tools/logstats.py) pitfalls — JUCE double serialisation rounding, var const-mutation via getDynamicObject, logger start timing vs tests, headless onClick() vs triggerClick(), the withdrawn widen-reset (A-9), multi-agent lane ownership traps
metadata:
  type: project
---

# Bài học từ lane D (vòng dữ liệu: log session + nhãn GOOD/FALSE) — 2026-09-05

**Bối cảnh:** lane D thêm `SessionLogger` (JSONL, thread riêng), nút GOOD/FALSE
trên `NotchListPanel`, và `tools/logstats.py`. 8 task + một fix wave sau verifier, suite 402 → 432, không
đụng audio path (0 dB). Spec: `docs/superpowers/specs/2026-09-05-data-loop-design.md`.
Plan + amendments A-1..A-10: `docs/superpowers/plans/2026-09-05-data-loop.md`.

## 1. `juce::JSON::toString` in double đủ dài để round-trip — làm tròn TRƯỚC khi vào `var`

Cạm bẫy KHÔNG phải là một double "tròn" như `0.0123`: cái đó in ra đúng
`0.0123`. Cạm bẫy là một magnitude **float chưa làm tròn** bị nới lên double.
`(double) 0.0123456789f` là `0.012345679104328156` — 20 ký tự cho MỘT bin, và
`serialiseDouble` của JUCE phải in đủ chừng đó để round-trip lại đúng giá trị.
Nhân với 1025 bin × 3 mảng thì một dòng `notch_set` phình ra vài chục KB thừa.
Không sửa được ở bước serialise vì lúc đó giá trị đã là double thật trong
`var`. Phải làm tròn về 3 chữ số có nghĩa (`roundSig3`) **trước khi** nhét vào
`var`, không phải format lại chuỗi sau.
(Kiểm chứng lại 2026-09-05: dòng `notch_set` stereo thật đo được 19,5–19,7 KB
sau khi đã `roundSig3`.)

## 2. `SessionLogger::log(const var&)` phải COPY object trước khi stamp `t` — sửa `var` const vẫn mutate được caller

`getDynamicObject()` trên một `var` là non-const về mặt kỹ thuật của kiểu trả
về (con trỏ tới `DynamicObject` dùng chung), nên gọi nó trên một tham số
`const var&` rồi set thêm property `"t"` **sửa luôn object của caller** — dù
chữ ký hàm là const-correct trên giấy.

Cách sửa thực tế trong code là một bản sao **một tầng**:

```cpp
juce::var stamped (new juce::DynamicObject (*obj));
```

`DynamicObject`'s copy ctor sao chép `NamedValueSet` của nó, đủ để `setProperty
("t", ...)` không chạm vào object của caller. Đây **không** phải `var::clone()`:
`clone()` là deep copy, nó sẽ nhân bản cả ba mảng 1025 phần tử trong `ctx` chỉ
để thêm một field — tốn kém vô ích. Các mảng con được chia sẻ, và không ai sửa
chúng sau khi dựng.

## 3. Không bao giờ start logger trong ctor của `MainComponent`

26 test hiện có cộng `HandsFreeSnapshot` đều dựng `MainComponent` để test
layout/logic, không phải để chạy một phiên thật. Nếu logger start trong ctor,
mỗi lần chạy test suite sẽ ghi — và **prune** — thư mục log thật ở
`%APPDATA%`. Logger chỉ start qua `MainComponent::startSessionLog(dir)`, và
lời gọi production duy nhất nằm trong **`main.cpp`**, ngay SAU `startAudio()`
(để header ghi đúng device đang mở) — không phải bên trong `startAudio()`, và
không bao giờ từ constructor. Test gọi cùng hàm đó với một thư mục temp.

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
