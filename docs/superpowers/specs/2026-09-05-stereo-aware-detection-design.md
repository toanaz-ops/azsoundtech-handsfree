# Lane S — Stereo-aware detection: dò theo làn, notch theo làn, nút LINK

**Ngày:** 2026-09-05. **Roadmap:** [`2026-09-04-anti-feedback-v2-roadmap.md`](2026-09-04-anti-feedback-v2-roadmap.md).
**Trạng thái:** spec, chờ owner duyệt. **Đụng audio path:** có.

## 1. Vấn đề

Đọc code 2026-09-05 (`AudioEngine.cpp:642-665`, `NotchController.cpp:420-445`):

- Tap chỉ chép **làn 0** sau notch của mỗi slot vào ring. Làn 1 không
  bao giờ được nghe.
- Mỗi `NotchController` có **một** `Detector` + `PeakinessAnalyzer` +
  `CandidateScorer`, ăn ring làn 0.
- Khi confirm một candidate, controller gọi `setNotch(lane, ...)` cho
  **mọi** `lane < width_` với cùng tần số, Q, depth, cùng index (KD-6).
  `adoptPreset` làm y hệt.

Hệ quả trên sân khấu: mic ở cánh trái, loa trái hú, app cắt cả cánh
phải. Hú xảy ra ở loa phải thì app mù hoàn toàn cho đến khi nó lọt sang
loa trái đủ mạnh.

Điều **đã** đúng và giữ nguyên: `NotchCommand.channel`,
`model_[slotOf(channel, index)]`, `SnapshotNotch.channel`, và lưới
`notchChains_[slot][lane]` đều phân biệt làn. Thay đổi tập trung ở tap,
ở bộ phân tích, và ở chính sách đặt notch.

## 2. Mục tiêu và phi mục tiêu

Mục tiêu:

1. Mỗi làn của slot stereo có phổ riêng, điểm số riêng, persistence
   riêng, và notch đặt **chỉ trên làn đã confirm**.
2. Nút **LINK** mỗi slot: bật thì trở về hành vi hiện tại (một làn
   confirm → cả hai làn nhận notch cùng index, cùng tham số).
3. Preset ghi làn của từng notch; file cũ nạp được, nghĩa cũ.
4. GUI: cột LANE trong danh sách notch, marker phân biệt L/R trên phổ,
   chọn làn hiển thị phổ, nút LINK trong SlotPanel.
5. Một tham số **asymmetry bonus** cho scorer (mặc định trung tính) để
   lane C và lane T (threshold sweep) dùng sau; không có núm GUI.

Phi mục tiêu:

- Không đổi thuật toán peakiness, rise, novelty, harmonic penalty.
- Không thêm thread: vẫn một detector thread mỗi slot (MC-1).
- Không xử lý cross-slot.
- Không đổi format version preset (chỉ thêm key tùy chọn).

## 3. Mức thay đổi level dự kiến (bắt buộc theo CLAUDE.md)

- Slot **mono**: không đổi.
- Slot stereo, **LINK bật**: không đổi so với 1.0.4.
- Slot stereo, **LINK tắt** (mặc định mới):
  - Hú bất đối xứng (một loa vào mic): làn hú nhận notch depth như cũ
    (−12/−18/−24 dB theo preset); **làn kia 0 dB thay đổi** thay vì bị
    cắt cùng depth như 1.0.4. Đây là lợi ích.
  - Hú đối xứng (mic giữa hai loa): cả hai làn tự confirm, cả hai nhận
    notch. Tương đương 1.0.4 nhưng có thể lệch nhau vài block vì
    persistence đếm riêng, tức là một làn có thể bị cắt sớm hơn làn kia
    tối đa `persistence × 10.7 ms`.
  - Hú xuất hiện ở làn 1 trước: 1.0.4 **không** phát hiện; bản này cắt
    làn 1. Đây là trường hợp level *giảm* ở nơi trước đây không giảm.
- Rủi ro đáng nêu với tester: một làn đạt ngưỡng, làn kia lơ lửng ngay
  dưới ngưỡng. Làn kia vẫn ring nhẹ cho đến khi tự vượt ngưỡng. Nếu
  tester thấy ring dai ở một bên trong khi bên kia đã cắt, LINK là nút
  thoát; lane G (nhả dần, depth theo nhu cầu) mới giải quyết gốc.

## 4. Thiết kế

### 4.1 Tap hai làn (`AudioEngine`)

```cpp
// AudioEngine.h
std::array<std::array<LockFreeRingBuffer<float>, kMaxSlotLanes>, kMaxSlots> tapBuffers_;
std::array<std::array<std::atomic<std::uint64_t>, kMaxSlotLanes>, kMaxSlots> tapDropCounts_;

LockFreeRingBuffer<float>& getTapBuffer (int slot, int lane);
std::uint64_t getTapDropCount (int slot, int lane) const;
// Overload cũ getTapBuffer(slot) / getTapDropCount(slot) = lane 0, giữ cho test.
```

Callback: `tapSource[slot]` thành `tapSource[slot][lane]`; vòng ghi tap
lặp cả hai làn; làn không bật (`width == 1`) để `nullptr`, không ghi.
Ghi làn 1 xảy ra **sau** clamp/sanitize như làn 0 hiện nay: detector
nghe đúng thứ ra driver. Bộ nhớ thêm: 8 × 8192 float = 256 KB. Callback
thêm một `write()` mỗi slot stereo, không lock, không alloc.

SPSC giữ nguyên: audio thread là producer duy nhất của mọi ring; detector
thread của slot i là consumer duy nhất của cả hai ring của slot i.

### 4.2 Bộ phân tích theo làn (`NotchController`)

```cpp
NotchController (LockFreeRingBuffer<float>& tapLane0,
                 LockFreeRingBuffer<float>* tapLane1,   // nullptr = không có
                 LockFreeRingBuffer<NotchCommand>& commands,
                 ClockSource& clock, int slotId = 0);
// Ctor cũ 4 tham số giữ nguyên, ủy quyền với tapLane1 = nullptr.

struct LaneAnalysis {
    Detector          detector;
    PeakinessAnalyzer analyzer;
    CandidateScorer   scorer;
    std::array<std::uint32_t, Detector::kNumBins> persistence {};   // thay vector<uint32_t> persistence_ lazy-size hiện nay; guard resize thành dead code, bỏ
};
std::array<LaneAnalysis, kMaxSlotLanes> lanes_;
```

Mỗi controller nay ~1.1 MB (hai history 128×1025). Vẫn heap qua
`unique_ptr` trong `MainComponent`; test rig đã có `/STACK:8388608`,
`Harness` trong test đặt controller by-value trên stack nên phải kiểm
lại: nếu tràn, chuyển `Harness` sang `unique_ptr`. Ghi rõ trong plan.

Mọi setter tuning (`setRiseReferenceMs`, `setPersistenceBlocks`,
`setNotchDefaults`, `setPeakinessThreshold`, `setSampleRate`) áp cho **cả
hai** `LaneAnalysis`. Getter đọc làn 0.

`runOnce()` **giữ vòng drain** hiện có (`NotchController.cpp:158-200`:
gọi `processLatestBlock` lặp cho đến khi hết block, vì một callback
ASIO ≥ 1024 mẫu đổ nhiều hop cùng lúc và poll 5 ms không được để
detector tụt lại). Vòng drain nay chạy hai làn **lockstep**:

```
loop:
    for mỗi làn l < width_ có tap:
        spec[l] = lanes_[l].detector.processLatestBlock(tap[l])   // ≤ 1 hop
    if không làn nào có readCount > 0: break
    for mỗi làn l có spec mới:
        publish magnitudes[l] vào snapshot
        processSpectrumForDetection(l, spec[l], spec của làn kia nếu cũng mới trong LẦN LẶP NÀY)
live clock: "tap còn sống" = BẤT KỲ làn nào có mẫu mới trong 250 ms
auto-release, soundcheck expiry, flushOutbox: như cũ
```

Hai ring nhận cùng số mẫu trong cùng callback nên mỗi lần lặp lấy được
cùng một hop trên cả hai làn; so sánh chéo (4.4) vì thế so hai phổ cùng
thời điểm. Nếu một làn hết block trước (chỉ xảy ra khi drop count hai
làn khác nhau, tức không xảy ra với capacity bằng nhau), làn còn lại
vẫn được xử lý độc lập và so sánh chéo bỏ qua cho lần lặp đó.

### 4.3 Chính sách đặt notch

Ba chế độ theo `width_` và cờ `linked_` (atomic bool, mặc định
**false**):

| width | tap làn 1 | linked | Confirm trên làn l → |
|---|---|---|---|
| 1 | bất kỳ | bất kỳ | set làn 0 tại `firstFreeIndex(0)` (như cũ) |
| 2 | **không có** (ctor cũ) | bất kỳ | như LINKED, chỉ dùng phổ làn 0 (S-6; test cũ giữ nghĩa) |
| 2 | có | false | set **làn l** tại `firstFreeIndex(l)`; làn kia không đụng |
| 2 | có | true | set **cả hai** làn tại `firstFreeIndexBothLanes()`, cùng tham số |

`firstFreeIndexBothLanes()` là **thay đổi so với 1.0.4**: hàm hiện tại
`firstFreeSlotLocked()` chỉ kiểm làn 0 (`NotchController.cpp:342-348`),
an toàn hôm nay vì hai làn luôn được ghi đồng bộ. Khi INDEP tồn tại,
hai làn phân kỳ; một confirm LINKED dùng hàm cũ có thể chọn index đang
active trên làn 1 và `setNotch` (không kiểm chỗ trống) sẽ ghi đè notch
đó mà không có `Clear`. Vì vậy LINKED chọn index đầu tiên rảnh ở **cả
hai** làn; không có thì bỏ qua confirm này (như hôm nay khi đầy 16).
Với slot chưa bao giờ ở INDEP, kết quả trùng 1.0.4.

Persistence đếm theo (làn, bin). Khi linked, confirm trên một làn là đủ
(union), vì mục tiêu của LINK là "cắt cả hai bên khi bất kỳ bên nào hú".

Auto-release theo (làn, index) như cũ: `lastDetectedMs` của một notch
được cập nhật khi **phổ của chính làn đó** thấy peakiness tại tần số đó.
Khi linked, cập nhật `lastDetectedMs` cho cả cặp khi bất kỳ làn nào
thấy; cặp nhả cùng lúc.

Harmonic penalty dùng danh sách tần số đã khóa **của làn đang xét**
(hiện gộp cả hai làn; khi linked hai danh sách trùng nhau nên hành vi
cũ giữ nguyên).

`setLinked(bool)`: gọi được khi thread đang chạy (atomic). Chuyển
true→false không đụng notch đang có; chuyển false→true cũng không đồng
bộ hóa notch cũ (không tự "chép" notch L sang R). Người vận hành muốn
đối xứng thì CLEAR ALL rồi để detector đặt lại. Ghi trong tooltip.

`setWidth(1)` khi đang có notch làn 1: push `Clear` cho mọi notch làn 1
(hiện nay `setWidth` chỉ ghi `width_`, notch làn 1 mồ côi trong model).
Tiền đề thread đã dừng giữ nguyên.

`adoptPreset` **luôn dùng `p.index` của file** cho mọi làn, như hiện
nay (không tìm index rảnh: identity của GUI khóa theo index, round-trip
phải giữ index). Notch có `lane` 0/1 → `setNotch(lane, p.index, ...)`
đúng làn đó. Notch `lane == -1` (file cũ) → mọi làn `< width_` cùng
index, "áp được hết hoặc không gì" như cũ, **bất kể** `linked_`. Chế độ
linked chỉ ảnh hưởng cách detector đặt notch mới, không ảnh hưởng cách
nạp preset.

### 4.4 Asymmetry bonus (tùy chọn, mặc định trung tính)

```cpp
void  setLaneAsymmetryBonus (float b);   // [1.0, 2.0], mặc định 1.0
float getLaneAsymmetryBonus() const;
```

Khi cả hai làn có phổ mới trong cùng vòng và `bonus > 1.0`: với
candidate ở bin b trên làn l, nếu `peakinessAt(other, b) < 0.5 ×
peakiness(l, b)` thì điểm cuối nhân `bonus`. Lý do: hú từ một loa vào
mic bất đối xứng theo hình học, nhạc từ mixer stereo thì không. Mặc định
1.0 nghĩa là **không đổi hành vi đặt notch** ngoài việc tách làn; lane T
sweep con số này bằng dữ liệu lane D trước khi bật.

### 4.5 Snapshot

```cpp
struct SnapshotBuffer {
    std::array<std::array<float, Detector::kNumBins>, kMaxSlotLanes> magnitudes {};
    std::uint32_t magnitudeCount = 0;     // bins hợp lệ, chung cho các làn
    std::uint32_t laneCount = 1;          // 1 hoặc 2
    bool          linked = false;
    double sampleRate = 0.0;
    std::array<SnapshotNotch, kTotalSlots> notches {};
    std::uint32_t notchCount = 0;
    std::uint64_t sequence = 0;
};
```

`magnitudes[0]` giữ nghĩa cũ. Kích thước ~8 KB, vẫn copy dưới mutex
như cũ. Mọi consumer (`SpectrumView`, `NotchListPanel`, test) sửa theo
compile error, không có đường dẫn ngầm.

### 4.6 Preset

`PresetNotch` thêm `int lane = -1;` (−1 = không ghi, nghĩa "mọi làn").
JSON: key `"lane"` tùy chọn, hợp lệ 0 hoặc 1. Kiểm tra trong **SHAPE
pass** (`readNotch`, `PresetManager.cpp:136-167`), giống nhau cho cả
hai overload `fromJSON`: `lane` không phải số nguyên hoặc ngoài {0, 1}
→ **từ chối cả file** kèm thông báo trích giá trị lạ, cùng lớp với
"notch trên Nyquist của file" (file tự mâu thuẫn, không phải lệch
thiết bị). Không dùng cơ chế skip của `slot`, vì cơ chế đó chỉ chạy
trong overload channel-aware và sẽ cho hai overload hành vi khác nhau.
`PresetSlot` thêm `bool linked = false`, key `"linked"` tùy chọn, đọc
trong `readSlotEntry`. Version giữ `"1.0"`: parser dùng `hasProperty`
cho key tùy chọn, file cũ không có key vẫn nạp đúng nghĩa cũ.

Serializer (`toJSON`, dùng bởi test round-trip) ghi `lane` và `linked`
luôn. **Hợp nhất với lane P ngày 05/09/2026**: `savePreset` ghi **một
notch cho mỗi (slot, làn, index)** kèm key `lane`, và một section
`"slots"` mang routing cùng cờ `linked` của từng slot đang bật; quy tắc
duy nhất của validator cũng tính theo **(slot, làn, index)** (`lane` =
−1 đụng với bất kỳ làn nào ở cùng index). `linked` vì thế đi được cả hai
chiều — từ file vào app và từ app ra file.

### 4.7 GUI

- **SlotPanel**: mỗi dòng thêm toggle `LINK` (SegmentedControl hai
  đoạn `LINK` / `INDEP`), chỉ hiện khi width = Stereo. Callback
  `onSlotLinkChanged(slot, bool)`. Tooltip: "LINK: một bên hú, cắt cả
  hai. INDEP: cắt đúng bên hú. Đổi chế độ không chép notch đang có."
- **NotchListPanel**: thêm cột `LANE` (`L` / `R`; slot mono luôn `L`).
  `RowText` thêm `juce::String lane`. Cột `#` giữ số thứ tự dòng.
- **SpectrumView**: marker notch làn 1 vẽ kiểu khác làn 0 (ví dụ tick
  đứt nét, cùng màu) và có nhãn `R` nhỏ; thanh công cụ analyser thêm
  SegmentedControl `L` / `R` chọn `magnitudes[lane]` hiển thị, mặc định
  `L`, disabled khi `laneCount == 1`. Trace vẫn một đường: không vẽ hai
  phổ chồng nhau (đọc không nổi ở 30 fps).
- Ảnh render bắt buộc: `console-live.png` với một slot stereo INDEP có
  notch L và R khác tần số, và một slot LINK.

### 4.8 Wiring (`MainComponent`)

Ctor tạo controller với `engine_.getTapBuffer(i, 0)` và
`&engine_.getTapBuffer(i, 1)`. `onSlotTuningChanged` không đổi.
`onSlotLinkChanged(slot, b)` → `controller.setLinked(b)` và
`slotLinked_[slot] = b` (mảng bool trong `MainComponent`, nguồn sự thật
runtime; lane P sẽ đọc mảng này khi có SAVE). `loadPreset` áp
`slots[i].linked` vào controller **và** vào `slotLinked_`/SlotPanel
trước khi `adoptPreset`.

### 4.9 Docs

`docs/KY-THUAT-CHONG-HU.md` §1 (sơ đồ: tap ×2 làn), §2 bước 3, §3.4
(chính sách theo làn, LINK), §4 (key `lane`, `linked`), §5 bảng, §6 bỏ
dòng nào không còn đúng. `docs/GIOI-THIEU.md` mục tính năng: stereo
independent + LINK. Cùng commit với code.

## 5. Kiểm thử

Mọi test ghi rõ thay đổi production nào làm nó đỏ (quy ước repo).

AudioEngine (`test_audioengine.cpp`):
1. Slot stereo: cả hai tap nhận đúng số mẫu, nội dung làn 1 = output
   làn 1 sau notch, không phải làn 0.
2. Slot mono: tap làn 1 không nhận gì, drop count làn 1 = 0.
3. Overload cũ `getTapBuffer(slot)` trả đúng làn 0.

NotchController (`test_notchcontroller.cpp`):
4. INDEP: sine 1 kHz chỉ trên tap làn 1, im lặng làn 0 → `Set` với
   `channel == 1`, **không** có `Set` `channel == 0`.
5. INDEP: sine trên cả hai → hai `Set`, index có thể khác nhau, mỗi
   `channel` một.
6. LINKED: sine chỉ làn 1 → hai `Set` cùng index cùng tham số (hành vi
   `PersistentHowlSetsNotchOnBothChannels` cũ, nay đổi tên và chạy dưới
   `setLinked(true)`).
7. Ctor cũ (không tap làn 1) + width 2 → hành vi LINKED dù `linked_ ==
   false` (không có phổ làn 1 thì không thể độc lập). Ghi rõ trong
   header.
8. Auto-release INDEP: notch làn 0 nhả sau 30 s dù làn 1 vẫn thấy tần
   số đó.
9. Auto-release LINKED: cặp nhả cùng lúc, chỉ khi cả hai làn im.
10. `setWidth(1)` với notch làn 1 đang active → `Clear channel 1`.
11. Tuning setter áp cả hai làn (đổi threshold, làn 1 phản ứng).
12. Asymmetry bonus = 1.0 không đổi điểm; = 2.0 nhân đôi điểm khi làn
    kia < 0.5×; không nhân khi làn kia ≥ 0.5×.
13. Snapshot: `laneCount`, `linked`, `magnitudes[1]` đúng.
14. Harmonic penalty chỉ tính notch cùng làn khi INDEP.

Preset (`test_presetmanager.cpp`):
15. Round-trip `lane` 0/1 và `linked` qua `toJSON`/`fromJSON`, cả hai
    overload.
16. File không có `lane` → `lane == -1`; `adoptPreset` áp mọi làn tại
    đúng `p.index` của file, bất kể `linked_`; notch `lane == 1` chỉ
    lên làn 1 tại `p.index`.
17. `lane` = 2, hoặc `"lane": "L"` → cả file bị từ chối, thông báo có
    giá trị lạ, giống nhau ở cả hai overload.
17b. LINKED sau khi INDEP đã đặt notch làn 1 tại index 0: confirm mới
    chọn index 1 cho cả hai làn, notch làn 1 index 0 còn nguyên (không
    có `Set` ghi đè, không có `Clear`).

GUI (`test_slotpanel.cpp`, `test_notchlistpanel.cpp`,
`test_spectrumview.cpp`):
18. LINK toggle ẩn khi Mono, hiện khi Stereo, phát callback.
19. Cột LANE đúng `L`/`R`.
20. Marker làn 1 có kiểu riêng; chọn `R` hiển thị `magnitudes[1]`.

Không có test nào cần card âm thanh.

## 6. Việc phải làm ngoài code

- `pwsh -File installer\release-alpha.ps1 -Part minor` (1.1.0: hành vi
  mặc định đổi trên slot stereo).
- Note gửi tester: mục 3 nguyên văn, kèm hướng dẫn "thấy ring dai một
  bên → bật LINK, báo lại".
- Memory note nếu quá trình làm phát hiện điều không hiển nhiên.

## 7. Quyết định đã chốt trong spec (để plan không hỏi lại)

| # | Quyết định | Lý do |
|---|---|---|
| S-1 | Mặc định INDEP, không LINKED | Owner yêu cầu tính năng này; LINK là đường lui |
| S-2 | Một thread/slot, hai bộ phân tích | MC-1; L/R cùng thread mới so sánh chéo được |
| S-3 | Bonus mặc định 1.0 | Không đổi hành vi đặt notch ngoài tách làn; sweep sau bằng dữ liệu |
| S-4 | Preset version giữ "1.0" | Key tùy chọn, parser bỏ qua khi thiếu |
| S-5 | Không chép notch khi đổi LINK | Chép là đoán; detector đặt lại chính xác hơn |
| S-6 | Ctor cũ = LINKED | Test cũ giữ nghĩa; không có phổ làn 1 thì không độc lập được |
| S-7 | LINKED chọn index rảnh ở cả hai làn | Hàm cũ chỉ nhìn làn 0 sẽ ghi đè notch INDEP của làn 1 |
| S-8 | `adoptPreset` giữ `p.index`, không tìm index rảnh | Identity GUI khóa theo index; round-trip phải giữ |
| S-9 | `lane` sai → từ chối file ở SHAPE pass | Hai overload `fromJSON` phải hành xử giống nhau; cơ chế skip của `slot` chỉ có ở một overload |
| S-10 | ~~Không có SAVE preset trong lane này~~ → hợp nhất với lane P 05/09/2026 | `savePreset` ghi một notch cho mỗi (slot, làn, index) kèm `lane`, và section `"slots"` mang `linked`; validator tính duy nhất theo (slot, làn, index) |

## 8. Phản biện đã xử lý

Reviewer độc lập (2026-09-05, agent chỉ đọc) tìm ra 2 BLOCKER
(`savePreset` không tồn tại; LINKED ghi đè notch INDEP), 2 SHOULD-FIX
(vòng drain bị bỏ trong pseudocode; `adoptPreset` INDEP đổi index), 1
NIT, 2 mơ hồ. Tất cả đã đưa vào 4.2, 4.3, 4.6, 4.8, 5 và bảng quyết
định. Reviewer xác nhận: SPSC không bị vi phạm; `setWidth` push Clear
khi thread dừng chỉ xếp vào outbox, không vi phạm producer duy nhất;
stack 8 MB đủ cho `Harness` by-value ~1.1 MB; mono không đổi.
