# Build verification: trust the clean configure, not the build dir

**Ngày:** 2026-08-21 (phát hiện trong Task 10)

## Bài học

Task 8 báo cáo "build verified on MSVC, 18/18 tests, 0 warnings". Thực tế
target `HandsFree` **không build được ở HEAD** suốt từ Task 3.

Nguyên nhân: `src/main.cpp`, `src/app/MainComponent.h`, `src/app/AudioEngine.h`
đều `#include <JuceHeader.h>`, nhưng file đó chỉ được sinh ra khi CMakeLists gọi
`juce_generate_juce_header(<target>)` — và lời gọi này chưa bao giờ được commit
(`git log -S juce_generate_juce_header -- CMakeLists.txt` trả về rỗng).

Thư mục `build-task8-msvc/HandsFree_artefacts/JuceLibraryCode/JuceHeader.h` có
tồn tại, sinh ra từ một chỉnh sửa cục bộ không commit. Người review chỉ nhìn
"build dir xanh" nên kết luận sai.

## Vì sao quan trọng

Bằng chứng build nằm trong thư mục build là bằng chứng về **máy của người đó**,
không phải về **repo**. Repo mới là thứ được bàn giao. Dự án này có tới 5 build
dir (`build/`, `build-review/`, `build-task8/`, `build-task8-msvc/`,
`build-verify/`), mỗi cái là một trạng thái lịch sử khác nhau — càng dễ nhầm.

## Áp dụng thế nào

- Khi một task tuyên bố "build passed", verify bằng cách **configure lại từ
  đầu** hoặc ít nhất build ở một dir khác với dir người implement đã dùng.
- Xác minh mọi thứ CMake phải sinh ra bằng `git log -S <tên hàm>` thay vì kiểm
  tra file đã tồn tại trên đĩa.
- Dọn bớt build dir: giữ `build/` là dir chuẩn, xoá các dir `build-*` cũ khi
  không còn cần, để không còn chỗ cho bằng chứng cũ trú ngụ.

Liên quan: [[brief-verification-2026-08-21]]
