# Sửa routing một slot không còn đặt lại các slot khác (HW-174.Q3)

- Trước: sửa kênh vào/ra của một slot dừng và đặt lại detector của cả 8 slot, các chip RING RISK về N/A và bộ nhớ phòng bị quên; mỗi lần sửa còn ghi file cài đặt.
- Nay: chỉ detector của slot vừa sửa được dừng, đặt lại và chạy lại. Bảy slot còn lại chạy tiếp. Đổi device / sample rate vẫn đặt lại cả 8 slot.
- **Mức ra thay đổi dự kiến: 0 dB ở mọi ngõ ra** (không đụng DSP, hệ số lọc, gain hay buffer).
- Các vòng khôi phục/mismatch/wizard ghi file cài đặt một lần thay vì một lần mỗi slot.
- Tester kiểm: chạy 2 slot, để slot 0 giữ một notch, sửa routing slot 1: notch của slot 0 và chip RING RISK của slot 0 vẫn còn; chỉ chip của slot 1 về N/A một lúc.
