---
name: worktree-prune-killed-live-session
description: prune/xóa worktree "stale" đã phá worktree đang SỐNG của một phiên khác vì desktop app tái dùng tên thư mục cũ cho nhánh mới
metadata:
  type: feedback
---

Ngày 2026-09-05, phiên điều phối (main) dọn worktree "stale": `rm -rf` +
`git worktree prune` trên `.claude/worktrees/chore-infra-prune-orphans-948d0e`.
Thư mục đó **đang là worktree sống** của phiên "Lane D: vòng dữ liệu" (nhánh
`claude_desk/lane-d-data-loop-239597`, tạo 10:51 cùng ngày). Desktop app đã
**tái dùng tên thư mục worktree cũ** (chore-infra-prune-orphans) cho một nhánh
lane-D hoàn toàn mới — tên nhìn stale nhưng ruột thì live.

Hậu quả: mất `build/` (~10 phút build lại), submodule checkout, và 3 sửa chưa
commit của phiên đó. Commit đã tạo (`0889084`, plan lane D) sống sót vì nằm trên
nhánh; `git worktree add` lại được vào đúng thư mục.

Hai dấu hiệu đã bị bỏ qua ngay lúc làm:
- `rm` báo **"Device or resource busy"** trên đúng thư mục đó — đó là một tiến
  trình đang giữ file, tức phiên còn sống. Phải DỪNG ngay, không retry, không prune.
- Ảnh chụp `rev-list --left-right` trước đó nói nhánh "đã merged" (branch-only=0)
  nhưng sau đã lệch — trạng thái git là ảnh tức thời, không đứng yên khi có phiên
  song song đang commit (global rule 1 + 7). Xem [[docs-drift-audit]].

**Why:** phán "stale" theo TÊN thư mục và một phép đo git đã cũ, trong khi tên
thư mục worktree KHÔNG đáng tin (desktop app tái dùng) và phiên khác đang chạy
trong đó. Đây đúng là kịch bản global rule 7 (một session một shared resource)
cảnh báo.

**How to apply:** trước khi `prune`/`rm`/`git worktree remove` BẤT KỲ worktree nào:
1. `git worktree list` — lấy path + nhánh + commit thật, không tin cái tên.
2. `mcp ccd_session_mgmt list_sessions` — đối chiếu `cwd` của mọi phiên
   `isRunning`; nếu một phiên sống trỏ vào worktree đó → KHÔNG đụng.
3. `git log main..<branch>` — đọc commit riêng trước khi xoá nhánh; luôn dùng
   `git branch -d` (nó từ chối nhánh chưa merged) trước, chỉ `-D` khi đã đọc log.
4. `rm` báo "Device or resource busy" = tiến trình sống đang giữ → DỪNG, hỏi owner.
5. Muốn chắc thì hỏi owner "worktree X còn phiên nào dùng không?" trước khi dọn.

Khác với [[worktree-junction-incident]] (remove đi theo junction xoá JUCE dùng
chung): lần này lỗi là dọn nhầm worktree của phiên đang chạy, không phải junction.
