# Thiếu ASIO SDK KHÔNG làm fail configure — nó tắt ASIO im lặng (2026-08-24)

## Triệu chứng

User báo "chưa thấy ASIO drivers trong cấu hình" của app, dù driver Audient
id14 đã cài trên máy (`HKLM:\SOFTWARE\ASIO` có key).

## Root cause

`CMakeLists.txt` chỉ **warn** khi không tìm thấy `external/asiosdk/common/asio.h`
— configure vẫn chạy thành công và build tiếp mà KHÔNG có `JUCE_ASIO`. Binary
không chứa `ASIOAudioIODeviceType`, nên danh sách thiết bị chỉ hiện WASAPI/
DirectSound bất kể driver cài hay chưa.

`CLAUDE.md` ghi *"If configure fails on a missing asio.h"* — **sai một nửa**:
configure KHÔNG fail, nó warn rồi bỏ qua. Ai chỉ nhìn "build xanh" sẽ không thấy.

## Cách chẩn đoán nhanh

1. `Test-Path external/asiosdk/common/asio.h`
2. `Select-String build\CMakeCache.txt "JUCE_ASIO:"` — phải ra
   `JUCE_ASIO:BOOL=ON`. Nếu chỉ có `JUCE_ASIO_SDK_PATH` là SDK chưa thấy.
3. Driver thật: `Get-ChildItem HKLM:\SOFTWARE\ASIO` (view native cho app x64;
   `WOW6432Node\ASIO` chỉ dành cho app 32-bit — ví dụ Realtek ASIO chỉ ở đó).

## Fix

Tải ASIO SDK 2.3.3 từ Steinberg (`download.steinberg.net/sdk_downloads/
asiosdk_2.3.3_2019-06-14.zip` — link trực tiếp, không cần login), giải nén vào
`external/asiosdk/` sao cho `common/asio.h` tồn tại. Full reconfigure + build.
Đã được user cho phép tự tải (2026-08-24) dù CLAUDE.md trước đó ghi "manual".

Lưu ý: copy bằng `Copy-Item src\* dst\ -Recurse` trên PS 5.1 lỗi
"Container cannot be copied onto existing leaf item" với SDK này — dùng robocopy.

## Phạm vi hỗ trợ thiết bị

Khi `JUCE_ASIO=ON`, JUCE enumerate mọi driver đăng ký trong
`HKLM:\SOFTWARE\ASIO` lúc runtime — Audient id14, Behringer Wing USB,
Midas… tự xuất hiện khi cài driver, không cần sửa code.

## ADDENDUM (cùng ngày): thiếu SDK chỉ là lớp 1 — cache var là no-op

Cài SDK xong app VẪN không có ASIO. Lớp 2 của root cause:
`set(JUCE_ASIO ON CACHE BOOL ... FORCE)` **không có tác dụng gì** với compile.
JUCE CMake không đọc biến đó (grep toàn `extras/Build/CMake`: không có
`JUCE_ASIO`). Cách đúng — như DemoRunner/AudioPluginHost của chính JUCE:

```cmake
target_compile_definitions(HandsFree PRIVATE JUCE_ASIO=1)
```

(đặt SAU `juce_add_gui_app`; module code compile trong target của app nên
define PRIVATE trên app là đủ).

### Verify ASIO thật sự vào binary

Chuỗi registry `"software\asio"` (UTF-16) chỉ tồn tại khi
`ASIOAudioIODeviceType` được compile:

```powershell
$b = [System.IO.File]::ReadAllBytes("<exe>")
[System.Text.Encoding]::Unicode.GetString($b).Contains("software\asio")
```

Cảnh báo: `Select-String -Pattern "ASIO"` trên binary KHÔNG phải bằng chứng —
không phân biệt hoa-thường và match được thứ khác (đã lỡ lần 1).
