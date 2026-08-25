# JUCE 9 — API đổi nghĩa mà compiler không kêu (2026-08-25)

Cứu từ nhánh `wip/ui-screenshot-loop-2026-08-25` trước khi xoá nhánh đó. Phần
"phải NHÌN thấy UI" của note gốc đã nằm ở [[ui-rebuild-sodium-rack-2026-08-25]]
và `.claude/skills/juce-component-snapshot/`; đây là phần còn lại — hai cái bẫy
API thuần tuý, vẫn đúng.

## 1. `TextButton` đổi ý nghĩa tham số thứ hai

```
JUCE 8:  TextButton (name, buttonText)
JUCE 9:  TextButton (buttonName, toolTip)   <-- tham số 2 giờ là TOOLTIP
```

Nên `juce::TextButton addButton_ { {}, "+ Add slot" }` tạo ra một nút **rỗng
hoàn toàn**: name = "", tooltip = "+ Add slot". **Compile sạch. Test không
bắt** (không có test nào assert chữ trên nút đó). Chỉ nhìn ảnh render mới thấy.

Sửa: dùng dạng một tham số `TextButton { "+ Add slot" }` — nó set cả name lẫn
text. Hoặc gọi `setButtonText()` rõ ràng.

**Quy tắc rút ra:** khi nâng JUCE lên major mới, **grep mọi brace-init hai tham
số của widget**. Những API "tiện lợi" kiểu này đổi nghĩa mà không hề có
deprecation warning — chữ ký cũ vẫn hợp lệ, chỉ là nó làm việc khác.

Chú ý cái bẫy song sinh: `juce::Label { componentName, labelText }` **vẫn là
(name, text)**. Hai constructor trông y hệt nhau, nghĩa ngược nhau.

## 2. `juce::Rectangle<float>` không có `reduced`/`reduce` bốn tham số

Chỉ có `reduced (dx, dy)` và `reduce (dx, dy)`. Muốn cắt mỗi cạnh một lượng
khác nhau thì phải nối chuỗi:

```cpp
bounds.withTrimmedLeft (l).withTrimmedRight (r)
      .withTrimmedTop (t).withTrimmedBottom (b)
```

## Nguồn

Ghi lại bởi một phiên agent song song cùng ngày, phiên đó redesign UI theo một
hướng khác và cuối cùng bị thay bằng "Sodium Rack". Hai bài học trên là thứ duy
nhất trong nhánh đó không tái tạo được từ nơi khác — phần còn lại là skill cài
từ GitHub (xem `skills-lock.json`) hoặc code đã bị thay thế.
