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

## Kết quả nghiên cứu (2026-10-20)

Tài liệu: `docs/research/2026-10-20-defeedback-landscape.md`. Proxy của phiên chặn mọi
trang ngoài GitHub, nên số liệu web lấy từ bản tóm tắt của công cụ tìm kiếm và đã gắn
nhãn mức bằng chứng. Phải kiểm lại trên toàn văn trước khi đưa vào spec. "isovoice"
**không tìm thấy**; owner gửi link nếu muốn so sánh.

Điều phối đã tự kiểm phát hiện §7 của tài liệu: `PeakinessAnalyzer.cpp:144` đặt tần số
notch đúng tâm bin (23,4 Hz/bin ở 48 kHz). Bảng sau là độ cắt thực tế tại tần số hú khi
tâm notch lệch nửa bin (11,7 Hz), cùng thiết kế RBJ đó, −18 dB, Q 30:

| Tần số | Độ cắt thực tế |
|---|---|
| 150 Hz | −1,4 dB |
| 300 Hz | −3,9 dB |
| 600 Hz | −8,0 dB |
| 1 kHz | −11,4 dB |
| 3 kHz | −16,4 dB |

## Q5: Làm khuyến nghị nào trước?

| # | Phương án | |
|---|---|---|
| 1 | **P1 trước: ước lượng tần số dưới-bin cho notch (lane F).** Không cần phần cứng; giảm chi phí tone dưới ~1 kHz, đúng hướng ràng buộc "không méo" | chọn |
| 2 | P2 (notch hẹp, Q thích nghi) trước | |
| 3 | P3 (đặc trưng phân biệt nhạc) trước | |

**Chọn: 1.** P2 chỉ an toàn khi tần số đã chính xác, nên phải đi sau P1. P3 cần nhạc
thu sẵn và nhãn D để đo tỉ lệ báo nhầm. P4 cần rig. P4 còn cần owner xác nhận cấp lại
giấy phép AGPL-3.0 cho phần dùng lại của `rta-tool`. Vì vậy P4 **chờ owner**, điều
phối không tự quyết.

Mức level dự kiến của P1:
- Ngoài tần số hú: không đổi (cùng Q, cùng depth).
- Tại tần số hú: độ cắt thực tế tiến về depth đã đặt. Ví dụ ở 300 Hz, khi tâm lệch nửa
  bin, độ cắt đi từ −3,9 dB về khoảng −18 dB.
- Thang lane G có thể dừng ở bậc nông hơn, tức ít cắt tone hơn.
- Phải kẹp độ lệch |Δf| ≤ 0,5 bin và có NaN-guard.

Sau review, BLOCKER/IMPORTANT (Q3) **đi trước P1**.
