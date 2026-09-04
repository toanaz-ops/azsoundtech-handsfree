# Docs drift audit — 2026-08-27

## Bối cảnh

Review toàn dự án (branch `claude_desk/project-overview-update-44a2ea`) sau ~15
commit dồn dập (multi-slot, Sodium Rack, tuning runtime, release pipeline).
Code sạch — 0 TODO/FIXME, guard an toàn đầy đủ — nhưng **docs và plan lệch
thực tế trên diện rộng**, và đó mới là rủi ro số một: một session mới đọc
roadmap sẽ đi làm lại việc đã xong từ một tuần trước.

## Bài học

1. **Plan checkbox không tự tick.** Plan multi-slot 368 dòng toàn `[ ]` dù
   Tasks 1–8 đã ship; plan bridge chỉ tick Task 1; hai plan mâu thuẫn nhau về
   cùng một việc. Ngược lại, plan gui-console tick `[x]` cho L1/L2 — feature đã
   bị GỠ ở `3a04200`. Cách xử lý: banner STATUS có ngày ở đầu file + tick đúng
   những mục có bằng chứng; KHÔNG bỏ tick lịch sử — plan là biên bản, thêm chú
   thích "đã revert" thay vì viết lại.

2. **Số test chép tay luôn stale — kể cả trong cùng một wave.** Docs ghi
   244/283/284 khi thật là 362; rồi chính wave sửa docs này lại ghi "362/362"
   trong khi lane code song song thêm 4 test → 366. Verifier độc lập bắt được.
   Quy tắc: số suite chỉ chép vào docs Ở BƯỚC CUỐI, sau khi mọi lane code đã
   land, lấy từ output ctest thật.

3. **Lỗ hổng an toàn tồn tại từ đầu không hiện trong diff nào.** Thiếu output
   clamp + NaN guard trên summing bus không phải regression — nó chưa bao giờ
   tồn tại, nên review-theo-commit không thấy. Chỉ audit "guard nào PHẢI có mà
   không có" mới lòi ra. Đã vá 27/08: sanitize input/output per-lane +
   `chain.reset()` tự hồi khi nhiễm NaN + clamp ±1.0 trước tap (test:
   `tests/test_audioengine.cpp`, section "Output safety guard").

4. **Fix đã vào CLAUDE.md**: Definition of done giờ có mục — thay đổi hằng số
   DSP/topology/hành vi user-visible phải cập nhật `docs/GIOI-THIEU.md` +
   `docs/KY-THUAT-CHONG-HU.md` trong cùng change. Xem [[gui-console-lessons]]
   và [[build-verification]] cho các bài học kiểm chứng liên quan.

## Còn treo (quyết định của người)

- Ring-risk chip vẫn "N/A" — provider chưa nối (`docs/spec-ring-risk.md`).
- Preset: GUI nạp/lưu chưa có, seed first-run viết+test nhưng app không gọi,
  installer không chép `presets/`.
- Ngưỡng peakiness 10.0 đo trên FFT 1024, chưa sweep lại với 2048.
- ~~`skills-lock.json` khóa 3 skill không tồn tại trong repo.~~ Chủ dự án gật
  2026-09-04 — đã `git rm`. Điều tra trước khi xóa: 3 skill nó khóa
  (design-taste-frontend, juce-best-practices, redesign-existing-projects)
  không nằm ở `.claude/skills/` (chỉ có `juce-component-snapshot`) lẫn
  `.opencode/skills/` (chỉ có các skill `openspec-*`) — không tool nào trong
  hai tool (Claude Code / OpenCode) của dự án đang dùng nó.
- ~~Clamp output cần một lần human listen ở volume thấp trước release kế
  tiếp.~~ Chủ dự án bỏ gate listen-trước-release (27/08/2026) — nghe trong
  alpha; clamp đã ship cùng 1.0.4.

## Phát hiện thêm 2026-09-04 — `openspec/` KHÔNG mồ côi (dự án dùng 2 tool)

CLAUDE.md từng ghi "OpenSpec is not in use" — đúng cho phía Claude Code,
SAI cho phía OpenCode: `.opencode/skills/openspec-{apply-change,
archive-change, explore, propose, sync-specs, update-change}/SKILL.md` nối
6 lệnh `/opsx-*` (`.opencode/commands/opsx-*.md`) vào workflow OpenSpec, và
các skill đó đọc `openspec/config.yaml` (`schema: spec-driven`). Xém xóa
nhầm ở Task C2 (docs/superpowers/plans/2026-08-27-next-wave.md) — chủ dự
án chặn lại, yêu cầu kiểm tra trước. Đã sửa CLAUDE.md để không lặp lại
nhầm lẫn này. **Bài học chung: một file "mồ côi" theo góc nhìn Claude Code
có thể đang sống ở phía OpenCode (hoặc ngược lại) — dự án này chạy qua lại
giữa hai tool — luôn `grep` cả `.opencode/` trước khi đề xuất xóa bất cứ
gì ở gốc repo.**
