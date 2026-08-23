# MEMORY — AZ Soundtech Hands-free

Index các bài học/quyết định của dự án. Mỗi note: `memory/<topic>-<date>.md` (1 file mỗi chủ đề, ghi ngay trong session phát hiện ra).

## Notes

- [Build verification](build-verification-2026-08-21.md) — "build passed" trong build dir không chứng minh gì về repo; app target hỏng ở HEAD suốt 6 task mà không ai thấy.
- [Brief verification](brief-verification-2026-08-21.md) — brief v2 "đã sửa" vẫn còn 3 lỗi; kiểm chứng cả tên API, hợp đồng ngữ nghĩa VÀ số học của assertion.
- [Worktree junction incident](worktree-junction-incident-2026-08-23.md) — `git worktree remove` đi theo junction và xóa JUCE dùng chung (lần 3); kiểm tra `LinkType` và rmdir junction TRƯỚC khi remove.

## Conventions (tóm tắt)

- Commit: `feat:` / `fix:` / `docs:` / `chore:` / `refactor:` — nhỏ, một việc một commit
- Branch: `feat/<tên>` / `fix/<tên>` — 1 session = 1 branch = 1 worktree (`.worktrees/`)
- Merge: qua Pull Request + review, không commit thẳng vào main
- Secret-scan hook: chạy tự động mỗi commit — không commit `.env`, `*.key`, token
- Stack: C++ / JUCE / CMake / MSVC / ASIO — xem `docs/superpowers/specs/`