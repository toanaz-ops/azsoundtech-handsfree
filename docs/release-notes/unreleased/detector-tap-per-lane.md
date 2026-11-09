# Detector nghe riêng từng làn (HW-174.Q2)

- Trước: khi nhiều slot cùng ra một kênh, detector của mọi slot đều nghe TỔNG kênh ra, nên hú ở vòng của slot 1 cũng làm detector slot 0 xác nhận và hai chuỗi notch cùng cắt một bin.
- Nay: mỗi làn có tap riêng mang tín hiệu sau notch CỦA LÀN ĐÓ.
- **Mức ra thay đổi dự kiến: 0 dB ở mọi ngõ ra** (tổng ngõ ra không đổi, bit-identical). Chỉ đầu vào detector đổi, và chỉ trên rig có từ 2 làn trở lên cùng một kênh ra; rig mỗi làn một kênh ra giống hệt bản trước.
- Tester kiểm: hai mic vào cùng một kênh ra, cho hú ở một vòng; chỉ notch của slot đó xuất hiện, slot kia không.
