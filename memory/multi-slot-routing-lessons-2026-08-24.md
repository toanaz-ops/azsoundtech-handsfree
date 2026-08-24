# Multi-slot routing — bài học kiến trúc & quy trình (2026-08-24)

## Bài học kỹ thuật

1. **By-value member array của object lớn = stack overflow ngầm.** Mỗi
   `NotchController` ~280KB (scorer history 128×513×4B); `std::array<NotchController, 8>`
   khai báo trong MainComponent đặt ~2.3MB lên thread có stack 1MB → segfault.
   Fix: `std::array<std::unique_ptr<NotchController>, kMaxSlots>` (commit 81e4c87).
   Quy tắc: trước khi nhét N bản copy vào mảng member, tính kích thước.

2. **"Legacy preserved" phải nói rõ trên device nào.** Callback mới clear-all
   output rồi chỉ ghi theo slot mapping ⇒ trên device >2 kênh, các kênh không
   map giờ IM LANG (trước đây pass-through). Đúng spec §4 và an toàn hơn, nhưng
   claim "hành vi cũ được bảo toàn" chỉ đúng trên stereo. Claim tương thích
   phải kèm điều kiện phần cứng.

3. **Bound per-ring ≠ bound per-callback.** Cap 256 lệnh/ring × 8 ring =
   2048 recompute/callback tiềm năng. Fix: một budget counter chia sẻ qua cả 8
   drain; lệnh dư đợi callback sau, không drop.

4. **Hai bên phải đồng nhất ngữ nghĩa width.** Engine coi width ngoài {1,2} là
   disabled; controller clamp 0→1 ⇒ preset khai width 0 tạo "phantom protection"
   (controller detect, chain im lặng). Sanitize ở MỘT điểm sớm nhất (preset
   routing pass) thay vì trông chờ mỗi bên tự xử.

5. **ASIO SDK thiếu chỉ warn, không fail configure** — xem
   `memory/asio-sdk-silent-disable-2026-08-24.md`; và define phải là
   `target_compile_definitions` chứ không phải CACHE variable.

## Bài học quy trình (SDD)

6. **Subagent trả rỗng xảy ra thật** — 3 lần trong session này, không commit,
   không report, thậm chí để lại edit ngoài phạm vi. Sau MỖI dispatch: kiểm tra
   `git log` + `git status` + report file trước khi tin kết quả. Work chết giữa
   đường → revert sạch, tách task nhỏ hơn, dispatch lại.

7. **Tác vụ >vài trăm dòng diff nên tách ngay từ plan** (Task 6 gốc phải tách
   6A/6B sau hai lần thất bại).

Chi tiết đầy đủ: `shared/handoff/handoff-20260824-multi-slot-routing.md`.
