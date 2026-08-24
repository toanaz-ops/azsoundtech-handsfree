# Hướng dẫn build file `.exe` — AZ Soundtech Hands-free

Tài liệu này chỉ cách tạo file `AZ Soundtech Hands-free.exe` để kích chạy
(hoặc đóng gói bộ cài), sau mỗi lần code đổi / merge.

> ⚠️ **Merge KHÔNG tự build.** `git merge` chỉ đổi source trong git. Muốn có
> binary mới thì luôn phải chạy lệnh build bên dưới. Bỏ qua bước này là bạn
> đang kích chạy bản exe cũ.

---

## 1. Yêu cầu (chỉ cài một lần)

| Thứ | Yêu cầu | Ghi chú |
|---|---|---|
| Visual Studio Build Tools **2026** | generator là `"Visual Studio 18 2026"` | máy này chỉ có VS 18; KHÔNG phải 17 2022 |
| CMake | `cmake -B` / `--build` | `cmake` trên PATH là bản MinGW — **phải luôn truyền `-G`** (xem §2) |
| ASIO SDK | thư mục `external/asiosdk/common/asio.h` | gitignored, tải từ Steinberg; thiếu thì app build KHÔNG có ASIO |
| NSIS (tùy chọn) | `C:\Program Files (x86)\NSIS\makensis.exe` | chỉ cần khi đóng gói bộ cài (§4) |

Nếu worktree mới được tạo: `git submodule update --init --recursive` (cho
`external/JUCE`) và tạo junction `external/asiosdk` trỏ về bản đã tải.

---

## 2. Build (chuẩn, mỗi lần đổi code)

Chạy trong thư mục gốc dự án. Nếu đổi **header hoặc CMakeLists.txt** → bắt
đầu từ lệnh reconfigure; nếu chỉ đổi thân `.cpp` → bỏ qua lệnh 1.

```bash
cmake -B build -G "Visual Studio 18 2026" -A x64
```

```bash
cmake --build build --config Release
```

```bash
ctest -C Release
```

(Lệnh 3 chạy trong thư mục `build/`.) Build coi như đạt khi ra
`100% tests passed`. Riêng đường DSP/audio: **green build chưa đủ** — phải có
người nghe thử volume thấp trước khi ra show.

## 3. File exe để kích

```
build\HandsFree_artefacts\Release\AZ Soundtech Hands-free.exe
```

Đây chính là file kích đúp để chạy. Nếu build thành công, file này có thời
gian mới nhất.

## 4. Đóng gói bộ cài (tùy chọn — để cài máy khác)

```bash
& "C:\Program Files (x86)\NSIS\makensis.exe" "/DBUILD_DIR=D:\DEV CAVE EP3\PROJECT005-AZ-handsfree\build" installer\handsfree.nsi
```

→ `installer\AZSoundtech-Handsfree-Setup-1.0.0.exe`. Lần đầu cần
`pwsh -File installer\fetch-deps.ps1` để tải `vc_redist.x64.exe` vào
`installer\vendor\`.

## 5. Trước khi build: đứng đúng nhánh

```bash
git checkout main
```

Build nhánh nào thì exe là code nhánh đó. Đừng build xong mới nhận ra đang
đứng ở nhánh cũ đã bị merge lỗi thời.

## 6. Khi build bị "file locked"

`unable to unlink ... Invalid argument` thường là **cl.exe mồ côi** từ một
build bị ngắt. Xem `memory/cl-exe-orphan-file-lock-2026-08-24.md` — chẩn đoán
bằng Restart Manager, kill cây `cl.exe`.
