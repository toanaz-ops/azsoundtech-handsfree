# MEMORY — AZ Soundtech Hands-free

Index các bài học/quyết định của dự án. Mỗi note: `memory/<topic>-<date>.md` (1 file mỗi chủ đề, ghi ngay trong session phát hiện ra).

## Notes

- [Build verification](build-verification-2026-08-21.md) — "build passed" trong build dir không chứng minh gì về repo; app target hỏng ở HEAD suốt 6 task mà không ai thấy.
- [Brief verification](brief-verification-2026-08-21.md) — brief v2 "đã sửa" vẫn còn 3 lỗi; kiểm chứng cả tên API, hợp đồng ngữ nghĩa VÀ số học của assertion.
- [Worktree junction incident](worktree-junction-incident-2026-08-23.md) — `git worktree remove` đi theo junction và xóa JUCE dùng chung (lần 3); kiểm tra `LinkType` và rmdir junction TRƯỚC khi remove.
- [Bridge lifecycle](bridge-lifecycle-devicepanel-2026-08-23.md) — đường restart thiết bị thật nằm ở DevicePanel; mọi đường restart mới phải bọc hook `onBeforeRestart/onAfterRestart` để giữ §6.5 (stop controller trước khi clear ring).
- [Multi-slot routing lessons](multi-slot-routing-lessons-2026-08-24.md) — by-value member array segfault; "legacy preserved" phải kèm điều kiện device; drain budget là per-callback không per-ring; width semantics đồng bộ một điểm.
- [ASIO SDK silent disable](asio-sdk-silent-disable-2026-08-24.md) — thiếu `external/asiosdk` KHÔNG fail configure, chỉ warn rồi tắt ASIO im lặng; check `JUCE_ASIO:BOOL=ON` trong CMakeCache trước khi nghi driver.
- [GUI console lessons](gui-console-lessons-2026-08-24.md) — `git commit -- <path>` commit nguyên working-tree file (hunk lane khác lọt vào); JUCE headless `setSize()` không peer → `resized()` không chạy, layout test pass rỗng; confirm dialog async phải injectable + SafePointer.
- [UI rebuild — Sodium Rack](ui-rebuild-sodium-rack-2026-08-25.md) — chụp GUI bằng `createComponentSnapshot()` chứ không chụp màn hình; ảnh render bắt được 4 lỗi test xanh không bắt; nội suy RGB giữa amber và cyan ra màu bùn; melatonin_blur cache theo object.
- [cl.exe orphan file lock](cl-exe-orphan-file-lock-2026-08-24.md) — merge fail "unable to unlink ... Invalid argument" = compiler cl.exe mồ côi từ build bị ngắt giữ handle; chẩn đoán bằng Restart Manager, kill cây cl.exe.

## Conventions (tóm tắt)

- Commit: `feat:` / `fix:` / `docs:` / `chore:` / `refactor:` — nhỏ, một việc một commit
- Branch: `feat/<tên>` / `fix/<tên>` — 1 session = 1 branch = 1 worktree (`.worktrees/`)
- Merge: qua Pull Request + review, không commit thẳng vào main
- Secret-scan hook: chạy tự động mỗi commit — không commit `.env`, `*.key`, token
- Stack: C++ / JUCE / CMake / MSVC / ASIO — xem `docs/superpowers/specs/`
