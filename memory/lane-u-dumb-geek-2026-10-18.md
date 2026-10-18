# Lane U — Dumb/Geek mode (2026-10-18)

Bài học không hiển nhiên từ lane U. Spec: `docs/superpowers/specs/2026-10-18-dumb-geek-mode-design.md`.

1. **Số kênh của engine = 0 cho tới callback đầu tiên** (`AudioEngine::getNumInputChannels`).
   Mọi phép "slot này có hợp lệ không" chạy ngay sau `startAudio()` đều nói "không"
   → tắt hết slot rồi ghi xuống đĩa. Kiểm ở tick timer đầu tiên có số kênh > 0.
   Phản biện vòng 1 bắt được (B-1); test headless KHÔNG bắt được vì headless luôn 0
   kênh — test đó pass vì lý do sai. Phải có seam đưa số kênh vào.
2. **JUCE `initialise(..., selectDefaultDeviceOnFailure = true, name)` lặng lẽ mở thiết
   bị KHÁC** khi thiết bị đã lưu mất. Routing đã lưu chạy trên card khác = mic ra
   ngõ không ai chọn trên PA. So `getCurrentDeviceType/Name()` sau `start()`.
3. **`startAudio()` tự đè loại driver** bằng `chooseDefaultDeviceType` — settings phải
   đi QUA nó, không đặt trước nó.
4. **`juce::String::toUpperCase()` hỏng chữ Việt trên Linux** (towupper ngoài locale
   Việt để nguyên ặ/ể/đ…): "ĐANG CHặN 3 điểM". L&F của repo uppercase mọi legend
   switch → viết HOA sẵn trong mã nguồn, hoặc dùng sentence case. Trên Windows chưa
   kiểm — đừng dựa vào nó.
5. **Fixture `Rig` của `test_soundcheckcontroller.cpp` có use-after-free** (sc khai báo
   trên `log`, dtor của sc ghi abort vào vector đã hủy). MSVC/macOS không lộ; Linux
   glibc báo "malloc(): unaligned tcache chunk"; ASan chỉ đúng chỗ. Build Linux
   (clang + `-DJUCE_USE_CURL=0`, cần libasound2-dev libx*-dev libfreetype-dev
   libfontconfig1-dev libgl1-mesa-dev libcurl4-openssl-dev) là một máy kiểm thứ ba
   rẻ; ASan dùng GCC vì clang ở đây thiếu runtime.
6. **Thư viện dùng chung với JUCE phải là INTERFACE** (nguồn biên dịch bởi người
   dùng, như module JUCE). STATIC link `juce::juce_core` = hai bản mọi symbol JUCE.
7. **Ảnh render bắt 4 lỗi 769 test bỏ qua**: headline viết hoa hỏng, đèn "cyan" ra
   cam (`trace` là amber), nút `ĐƠN GIẢN` đè chip NO DEVICE, nút chọn kênh style
   `segment` trông như chữ trơn.
8. Quy trình: `git commit -m "...$T"` với `$'\n'` trong chuỗi nháy kép KHÔNG xuống
   dòng — trailer dính vào subject. Dùng `git commit -F file`.
