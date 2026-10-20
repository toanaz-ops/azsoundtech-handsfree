# Lane A (AFC) — dừng 2026-10-19

Spec `docs/superpowers/specs/2026-10-19-afc-design.md` (§9 số đo), sổ
`docs/superpowers/decisions/2026-10-19-lane-a-afc.md`, code nghiên cứu `tools/afc/`.

1. **Ràng buộc owner (cứng): "không chấp nhận bất kỳ méo mó tiêu cực nào về âm
   thanh sau khi qua handsfree".** Mọi kỹ thuật đổi tiếng (dịch tần, delay
   modulation) bị loại; mọi lane xử lý tín hiệu sau này phải chứng minh "không
   xấu hơn khi tắt" trên mọi nguồn, không chỉ "ổn định hơn".
2. **"Ổn định" ≠ "tốt".** Tiêu chí MSG đơn thuần cho AFC bước lớn +13 dB trong khi
   tiếng hỏng (SNR e/s 6 dB). Luôn đo kèm độ giống tín hiệu nguồn.
3. **Độ lệch dải rộng đánh lừa** với nguồn băng hẹp: dải nguồn không chạm thì
   không nhận dạng được; đo theo dải.
4. **Kiểm vòng hở trước** (u trắng độc lập) để tách lỗi toán khỏi bias vòng kín —
   tiết kiệm nhiều giờ.
5. NLMS khối chia K partition phải chuẩn hóa bằng **tổng** công suất K partition
   (phản biện B1, tái hiện đúng bằng mô phỏng). PEM ở 48 kHz cần bậc ~32.
6. Cổng "thiếu kích thích" phải đo trên tham chiếu **chưa làm trắng** — làm trắng
   làm phẳng đúng những bin cần chặn.
7. Kết quả giống hệt nhau giữa các nguồn rất khác nhau = bộ lọc không học, không
   phải "bền vững".
8. Quy trình: `pkill -f <tên>` khớp chính dòng lệnh shell đang chạy và tự giết nó
   (exit 144) — dùng `pkill -x`. Vòng `until` có `exit` bên trong thoát luôn shell.
