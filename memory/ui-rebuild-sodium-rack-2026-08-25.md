# UI rebuild — "Sodium Rack" (2026-08-25)

Bài học từ lần đập đi xây lại toàn bộ giao diện console. Chỉ ghi những thứ
**không suy ra được** từ code hay git log.

## 1. Chụp GUI JUCE: dùng `createComponentSnapshot()`, KHÔNG chụp màn hình

`juce::Component::createComponentSnapshot (bounds, /*includeChildren*/ true)`
render thẳng cây component ra `juce::Image` — không cần window, không cần
desktop peer. Đây là cách DUY NHẤT đáng tin để review một thay đổi GUI.

Chụp màn hình app đang chạy hỏng vì những lý do **không liên quan gì đến app**:

- cửa sổ khác (overlay screen-share, Snipping Tool) đè lên vùng chụp;
- PowerShell không per-monitor DPI aware → `GetWindowRect` trả pixel VẬT LÝ
  còn `Graphics.CopyFromScreen` đọc pixel ẢO HÓA, ảnh lệch đúng bằng hệ số
  scaling (máy này 4K + 150%);
- `PrintWindow` ngay sau khi resize trả về frame CHƯA layout xong;
- **app đang chạy giữ lock trên chính `.exe` của nó** → build kế tiếp fail
  `LNK1104: cannot open file ... .exe`. Mỗi vòng lặp review phải kill app.

Công cụ: `tools/snapshot.cpp` → target `HandsFreeSnapshot`. Skill hướng dẫn:
`.claude/skills/juce-component-snapshot/SKILL.md`.

Ba cái bẫy khi dùng nó (đã dính đủ cả ba):

- `setSize()` KHÔNG gọi `resized()` khi không có peer → phải gọi tay, nếu
  không ảnh ra rỗng (đây là lỗi hay gặp nhất);
- timer không chạy → panel đọc snapshot phải được gọi `refreshFromSnapshot()`
  bằng tay;
- MSVC cần `/STACK:8388608` cho bất kỳ target nào dựng `MainComponent`.

## 2. Ảnh render bắt được lỗi mà test xanh không bắt được

Bốn lỗi thật, chỉ lộ ra khi NHÌN, toàn bộ test vẫn xanh:

- `juce::TextButton { {}, "+ Add slot" }` — constructor 2 tham số của
  TextButton là **(buttonName, TOOLTIP)**, không phải (name, text) như
  `juce::Label`. Nút "+ Add slot" chưa bao giờ có chữ; styling phẳng cũ giấu
  nó, viền mới làm lộ ra ngay.
- ModeRail chỉ phản ánh cú click của user, không bao giờ đọc mode của engine
  → cửa sổ vừa mở hiện badge BYPASSED mà không đèn nào sáng. Sửa bằng
  `ModeRail::setDisplayedMode()` gọi từ cùng vòng poll với badge.
- `drawFittedText(..., minimumHorizontalScale = 1.0f)` cắt chữ thành
  "CLEAR A…" thay vì bóp lại. Nút phá hủy mà bị cắt chữ là không chấp nhận
  được → dùng 0.8f.
- Marker notch vẽ theo brief "depth → 40..100% chiều cao plot" ra ba **bức
  tường** amber che hết đường tín hiệu. Brief đúng về mặt chữ, sai về mặt
  nhìn: marker chỉ cần nói Ở ĐÂU và SÂU BAO NHIÊU, dữ liệu mới là đường vẽ.
  Đổi sang wedge ngắn 18–46 px.

## 3. Nội suy màu RGB giữa hai màu đối nghịch cho ra màu bùn

Ramp "notch mới = amber sodium → notch đã ổn định = xanh băng" nội suy thẳng
RGB đi qua đúng trung điểm component-wise = **olive khaki**. Trên nền graphite
nó đọc như lỗi render chứ không như "đang nguội dần".

Sửa: ramp 3 chặng, đi qua một màu thép nhạt (`cooling` #C9D1D9). Giữa đường
thì **giảm bão hòa**, không đổi hue thẳng. Test khẳng định điều đó bằng phép
đo: `notchColour(halfway).getSaturation()` phải nhỏ hơn cả hai đầu.

## 4. Test transcribe giá trị thiết kế thì phải sửa cùng thiết kế

`tests/test_aztheme.cpp` chép nguyên bảng palette/metric. Đổi thiết kế =
4 test đỏ ngay, đúng như thiết kế của nó ("một thay đổi ở đây nghĩa là chủ
dự án đã duyệt lại palette"). Đừng coi là test hỏng.

Ngược lại `NotchListPanelWiring.StripIsAlwaysPinnedAtTheWindowBottom` khẳng
định `height == 120` và `bottom == kMinimumHeight` — nó mã hóa **layout cũ**
(strip full-width đáy cửa sổ). Layout mới cho bảng notch thành CỘT TRÁI của
floor. Viết lại theo quan hệ (không đè lên analyser, nằm trong cửa sổ,
idempotent) thay vì theo pixel, để lần đổi layout sau chỉ đỏ khi thật sự hỏng.

## 5. `melatonin_blur` cache theo OBJECT, không theo lời gọi

Submodule `external/melatonin_blur` (MIT) cho drop/inner shadow gaussian thật.
`melatonin::DropShadow` cache blur gắn với **instance**, nên tạo mới trong
`paint()` là vứt cache mỗi frame. Phải là **member** của component vẽ nó, và
`juce::Path` truyền vào cũng nên dựng trong `resized()` chứ không phải
`paint()`.

Hệ quả kiến trúc: KHÔNG dùng shadow trong `LookAndFeel` — một L&F phục vụ mọi
button nên cache sẽ đập nhau liên tục. Trong repo này shadow chỉ nằm ở
`StatusBadge` (quầng LED) và `SpectrumView` (quầng notch vừa bắn).

## 6. Layout: cột cố định luôn thắng phần co giãn nếu không chặn

Cột rig (device + detection + bảng routing) có chiều cao CỐ ĐỊNH. Không chặn
thì ở cửa sổ cỡ vừa nó chiếm nhiều pixel hơn cả analyser — lật ngược toàn bộ
ý đồ layout. Hai trần trong `MainComponent::floorHeightFor()`: trần tuyệt đối
(`kMinSpectrumHeight`) và **trần tỉ lệ** (`kMaxFloorShare = 0.55`). Trần tỉ lệ
mới là cái có tác dụng thật.

Kèm theo: `MainComponent` trước đây không gọi `setSize()` → `setContentOwned`
mở cửa sổ đúng bằng `setResizeLimits` tối thiểu, tức là app luôn khởi động ở
đúng cỡ layout tệ nhất của nó. Giờ có `kDefaultWidth/kDefaultHeight`.

## 7. Literal UTF-8 trong source C++ bị MSVC nuốt (rule 6, ở phía compiler)

`juce::String ("Active notches  \u00b7  Slot ")` render ra màn hình thành
**"ACTIVE NOTCHES Â· SLOT 01"**. MSVC đọc literal qua execution charset; hai
byte UTF-8 của dấu chấm giữa (0xC2 0xB7) bị hiểu là hai ký tự Latin-1.

Repo đã có sẵn cách đúng và có ghi lý do — `NotchListPanel::minusSign()` dựng
U+2212 từ **code point**:

```cpp
juce::String::charToString ((juce::juce_wchar) 0x00b7)
```

Rule 6 lâu nay chỉ nói về script đọc-ghi file. Nó áp dụng cả cho **lần đọc của
compiler**. Không viết ký tự ngoài ASCII vào string literal C++ trong repo này.

Cách bắt: chỉ nhìn ảnh render mới thấy. Build xanh, 354 test xanh.

## 8. Danh sách source của test target là bản chép tay thứ hai

`tests/CMakeLists.txt` liệt kê tay từng `../src/...`. Thêm
`src/gui/SlotTabs.cpp` vào root CMakeLists thì app + tool build được, còn test
target **link fail** vì file đó đơn giản là không tồn tại với nó.

Đã sửa tận gốc: cả ba target (app, test, tool) giờ dùng chung
`HANDSFREE_CORE_SOURCES` khai báo ở root, đường dẫn **tuyệt đối**
(`${CMAKE_SOURCE_DIR}/...`) — vì `tests/` và `tools/` giải đường dẫn tương đối
theo thư mục của CHÍNH NÓ.

## 9. Đổi hướng thiết kế: đường tín hiệu màu sodium, không phải xám

Bản đầu tôi vẽ đường phổ đơn sắc để dành riêng màu cho notch. Chủ dự án chọn
theo mockup: đường sodium + gradient fill. Hệ quả phải xử lý:

- notch **mới** (cũng sodium) chìm vào đường tín hiệu → mọi marker giờ vẽ trên
  một **keyline tối** (`background`, dày 3 px) rồi mới tô màu lên. Đây là cách
  máy phân tích phần cứng tách cursor khỏi trace, và nó đúng với mọi màu mà
  ramp đang ở.
- ngược lại notch **đã ổn định** (ice) nổi bật hơn hẳn so với nền xám cũ —
  tương phản mạnh nhất màn hình. Hướng của chủ dự án hoá ra tốt hơn.
- lưới phải có token riêng (`grid` #1D2128). Dùng `shade` (#060709) thì lưới
  gần như tàng hình — mà lưới sinh ra để ĐỌC.
