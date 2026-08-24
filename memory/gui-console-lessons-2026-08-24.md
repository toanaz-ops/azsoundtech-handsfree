# GUI console session lessons — 2026-08-24

Hai bẫy thật từ phase GUI Console redesign (Tasks 0–5, branch
feat/gui-console-redesign):

## 1. `git commit -- <path>` commit NGUYÊN nội dung working-tree của path

`git commit -m "..." -- CMakeLists.txt` không "commit mỗi thay đổi của tôi trong
file này" — nó commit toàn bộ working-tree content của file, kể cả hunk của
phiên song song đang sửa dở. Sự cố: hunk ASIO của lane khác lọt vào commit
`feat(gui): ModeRail...`; phải reset + tách lại thành 2 commit (owner duyệt).

**Quy tắc:** luôn `git add <explicit-paths>` rồi `git commit` KHÔNG pathspec.
Partial-staging một file nhiều hunk phải qua `git apply --cached` với patch
tự tạo — và verify bằng `git diff --cached`.

## 2. JUCE component headless: `setSize()` không có peer → `resized()` KHÔNG chạy

Component test ngoài desktop không có window peer nên resize không được deliver
— layout test có thể PASS RỖNG (đọc bounds cũ từ lần layout trước). L2 test cũ
đã pass kiểu này suốt Task 3.

**Quy tắc:** sau khi dựng component headless, gọi `app.resized()` (hoặc pattern
tương đương) TRƯỚC khi assert bounds; assert bounds SAU KHI đổi layout, đừng
tin bounds kế thừa. Xem `PerformanceLayoutHidesTheStripUntilToggled`
(test_notchlistpanel.cpp) làm mẫu.

## 3. (nhỏ) Confirm dialog async phải injectable

`JUCE_MODAL_LOOPS_PERMITTED` off → mọi hộp xác nhận cần hook injectable
(`std::function<void(std::function<void(bool)>)>`) để test headless, và callback
phải capture SafePointer chứ không `[this]` (UAF khi component chết giữa chừng).
Xem ModeRail::handleClearAllClicked.
