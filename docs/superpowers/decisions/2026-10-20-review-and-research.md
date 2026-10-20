# Sổ quyết định: review toàn dự án và nghiên cứu chống hú (2026-10-20)

Mỗi mục ghi một ngả rẽ: câu hỏi, các phương án, và lựa chọn.

**Trạng thái:** đang chạy. Owner (20/10/2026) yêu cầu: "Spawn 1 agent fable review toàn dự án. Spawn thêm sub agent nghiên cứu về defeedback nổi tiếng gần đây và các project tương tự (ví dụ isovoice…), cần thiết thì dùng REA repo để nghiên cứu. Làm cho tới khi xong không hỏi tôi, tự quyết định + ghi lại trong sổ quyết định của dự án."

Mọi mục dưới đây do điều phối **tự quyết** theo lệnh đó. Owner dò lại và đổi nếu cần.

Người chốt: Claude (điều phối). Người quyết cuối: owner (ToanAZ).

## Q1: "REA repo" là repo nào?

| # | Phương án | |
|---|---|---|
| 1 | **`toanaz-ops/rta-tool`**: tài khoản không có repo tên "REA"; `list_repos` trả 13 repo, gần nhất về tên và nội dung là `rta-tool` (real-time analyzer, public) | chọn |
| 2 | `project008-soundtech-assist` (trợ lý soundtech) | |
| 3 | Bỏ qua | |

**Chọn: 1.** Đọc **chỉ đọc**, chỉ khi nghiên cứu thấy cần. Nếu owner muốn nói repo khác thì lật lại mục này.

## Q2: Review chạy trên gì, có được sửa code không?

| # | Phương án | |
|---|---|---|
| 1 | **Review chỉ đọc trên `origin/main`** (worktree tách rời); báo cáo trước, sửa sau trong PR riêng do điều phối làm | chọn |
| 2 | Agent review tự sửa luôn | |

**Chọn: 1.** Đây là DSP âm thanh trực tiếp: một sửa chữa sai sẽ ra loa PA. Người review
và người sửa phải tách nhau. Nhánh `feat/lane-l-copilot` (PR #10) chưa lên main nên
nằm ngoài phạm vi.

## Q3: Xử lý phát hiện của review thế nào?

| # | Phương án | |
|---|---|---|
| 1 | **BLOCKER/IMPORTANT có đường lỗi cụ thể → sửa, mỗi nhóm một PR, kèm test và mức level dự đoán. MINOR → gom vào sổ việc treo, không sửa vội.** Sửa DSP thì vẫn theo luật CLAUDE.md: nêu mức thay đổi, chạy full build + ctest | chọn |
| 2 | Sửa tất cả trong một PR | |
| 3 | Chỉ báo cáo | |

**Chọn: 1.**

## Q4: Kết quả nghiên cứu để ở đâu?

| # | Phương án | |
|---|---|---|
| 1 | **`docs/research/2026-10-20-defeedback-landscape.md`** cùng một ghi chú `memory/`; khuyến nghị nào đổi roadmap thì ghi thêm ở đây (Q5+) | chọn |
| 2 | Chỉ trả lời trong chat | |

**Chọn: 1.**
