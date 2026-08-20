# MEMORY — AZ Soundtech Hands-free

Index các bài học/quyết định của dự án. Mỗi note: `memory/<topic>-<date>.md` (1 file mỗi chủ đề, ghi ngay trong session phát hiện ra).

## Notes

_(chưa có — thêm note đầu tiên khi có bài học/decision mới)_

## Conventions (tóm tắt)

- Commit: `feat:` / `fix:` / `docs:` / `chore:` / `refactor:` — nhỏ, một việc một commit
- Branch: `feat/<tên>` / `fix/<tên>` — 1 session = 1 branch = 1 worktree (`.worktrees/`)
- Merge: qua Pull Request + review, không commit thẳng vào main
- Secret-scan hook: chạy tự động mỗi commit — không commit `.env`, `*.key`, token
- Stack: C++ / JUCE / CMake / MSVC / ASIO — xem `docs/superpowers/specs/`