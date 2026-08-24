# File "Invalid argument" khi merge = compiler cl.exe mồ côi giữ handle (2026-08-24)

## Triệu chứng

`git merge` fail với `error: unable to unlink old 'src/app/AudioEngine.h':
Invalid argument` — luôn đúng 2 file `.h`, kể cả `git checkout -- <file>` cũng
không ghi đè được. Rename/write/append đều fail "being used by another process".

## Root cause

Một build bị ngắt giữa chừng để lại **cặp cl.exe mồ côi** (driver + worker của
`/MP`) vẫn còn sống, giữ handle vào đúng 2 header đang compile. CPU ~0 suốt 3
giờ = treo thật, không phải đang build.

## Cách chẩn đoán (không cần Sysinternals)

Restart Manager API, xác định PID giữ file:

```powershell
# rstrtmgr.dll: RmStartSession -> RmRegisterResources(<files>) -> RmGetList
# => LOCKER pid=75868 name=Microsoft® C/C++ Compiler Driver
```
Sau đó `Get-CimInstance Win32_Process -Filter "ProcessId=<pid>"` lộ ra
`cl.exe` + cha cũng là `cl.exe` (cùng .rsp) = cặp /MP treo.

## Fix

`Stop-Process -Id <cha>,<con> -Force` → file mở khóa ngay. Kiểm tra lại bằng
Rename-Item probe. Cũng quét `cl.exe/MSBuild.exe/mspdbsrv.exe` xem còn mồ côi
khác cùng đợt không.

## Phòng tránh

- Nếu `cmake --build` bị ngắt (Ctrl+C / kill), kiểm tra orphan cl.exe trước khi
  đổ lỗi cho "file bị khóa". Kill cây cl.exe là an toàn (compiler vô trạng thái).
- KHÔNG phải clangd/editor trong lần này — đừng giả định thủ phạm, dùng
  Restart Manager để chỉ đích danh.
