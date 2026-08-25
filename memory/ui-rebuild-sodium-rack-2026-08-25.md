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
