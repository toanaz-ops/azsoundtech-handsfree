# Lane L2a — copilot sidecar + bảng COPILOT (2026-11-09)

**Bối cảnh:** owner chốt 09/11/2026: LLM tới soundman qua tiến trình phụ (Q1 B),
người dùng tự nhập khoá (Q2 1), luật cố định lên GUI trước (Q3 1). L2a = sidecar
`HandsFreeCopilot.exe` + bảng `COPILOT` trong Geek mode; chưa có LLM, chưa có mạng.
Spec: `docs/superpowers/specs/2026-11-09-copilot-panel-design.md`. PR #37.

## Bài học

1. **Python đóng băng ghi stdout bằng code page ANSI khi bị pipe trên Windows.**
   Chữ Việt hỏng hoặc ném `UnicodeEncodeError`. Ép `reconfigure(encoding="utf-8")`
   ngay đầu `main()`, trước cả `parse_args`. Test phải gỡ `PYTHONIOENCODING` và
   `PYTHONUTF8` khỏi môi trường thì mới bắt được.
2. **`juce::ChildProcess::kill()` để lại exit code 0, và `readProcessOutput` chặn tới
   khi tiến trình con thoát.** Không suy ra "quá hạn" hay "bị huỷ" từ exit code; phải
   có watchdog riêng và cờ riêng.
3. **Cờ huỷ đặt ở backend và được `run()` reset là một cái cờ mất được.** Lệnh huỷ đến
   ngay trước `run()` bị xoá, destructor chờ `stopThread` rồi JUCE giết luồng. Sửa
   bằng token huỷ **theo từng lượt chạy**, tạo và công bố cùng một khoá với bộ đếm thế hệ.
4. **PyInstaller `onedir` khởi động nguội 1,5–7,5 s** trên máy dev (bundle 19,4 MB,
   nghi Defender quét). Hạn chờ 10 s là quá sát; đặt 30 s. `onefile` còn chậm hơn.
5. **Log ghi `slot` đếm từ 0, console in `SLOT 01`.** Mọi chữ tới tay người dùng phải
   cộng 1; lỗi này chỉ lộ khi nhìn ảnh render cạnh danh sách notch.
6. **MSVC ở repo này không có `/utf-8`.** Chuỗi Việt trong nguồn và trong test viết
   bằng byte escape (`\xe1\xba\xa1`) hoặc `\u` trong JSON; raw string chứa `)"` cần
   delimiter riêng (`R"json(...)json"`).
7. **Bộ test headless không bơm message loop.** Lớp trả kết quả về luồng message phải
   nhận một hàm "post" tiêm được; test tự rút hàng đợi bằng tay.
8. **Test tích hợp với exe thật phải tự bỏ qua khi exe vắng** (`GTEST_SKIP`), vì CI
   không dựng sidecar. Đường `ChildProcess` → Python đóng băng → stdout UTF-8 chạy
   đúng ngay lần đầu, kể cả khi stderr không được nối.
9. **Ảnh render bắt 2 lỗi 849 test bỏ qua:** dòng nhận xét thứ ba bị cắt ở 1440×920
   (bảng 40 % cột trái là thiếu, 50 % thì vừa), và nhãn "slot 0".

## Bẫy quy trình

- Hook `SPAWN GATE` chặn `Agent(model: opus)` cho agent có quyền sửa code; agent viết
  code phải là `sonnet` hoặc `haiku`.
- `python -I` tắt user site-packages: gói cài bằng `pip --user` (kể cả `anthropic`)
  biến mất. Chỉ dùng `-I` khi đọc file tải về, và cài thư viện phụ bằng `--target`.
- Phiên khác merge vào `main` giữa lúc lane đang chạy (PR #35 đụng `MainComponent`):
  số `ctest` đo trước khi gộp không còn là bằng chứng cho cây sau khi gộp.
- `makensis` ghi file cài vào `installer/` với số version hiện tại. Biên dịch thử thì
  chuyển file đó đi ngay, kẻo để lại một bản "1.3.0" không phải bản đã phát hành.
