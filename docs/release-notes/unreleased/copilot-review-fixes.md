# Sửa sau khi soát lại phase AI: bộ lọc câu trả lời, khoá, đồng ý, gỡ cài

**Mức ra thay đổi dự kiến: 0 dB ở mọi ngõ ra.** Không đụng đường âm thanh, detector hay notch.

Nguồn: Fable review toàn bộ diff phase AI (L2a, L2b, C0) sau khi 1.5.0 đã phát hành; quyết định Q26–Q31 trong
`docs/superpowers/decisions/2026-11-09-ai-integration.md`.

## Đổi gì

- **Dòng cảnh báo cố định dưới mọi câu trả lời AI:** "AI có thể sai. Đừng tăng gain hay tắt chống hú theo lời
  này." Không hiện dưới thông báo "bị chặn" hay dưới kết quả `PHÂN TÍCH`.
- **Bộ lọc câu trả lời viết lại.** Chặn thêm: nhắc tới gain / âm lượng / fader / trim / preamp / master / trần
  mà không kèm ý giảm hay phủ định rõ; "to hơn", "lên thêm"; đặt mức về một con số; xoá / reset notch; làm
  notch nông đi; tắt, bypass chống hú gọi bằng tên vòng ("tính năng này", "plugin", "chế độ tự động"); câu trả
  lời bằng chữ không phải Latin hoặc không nhận ra là tiếng Việt / Anh; chữ viết giãn, ký tự giống nhau.
  - **Đo độc lập 10/11/2026** (câu người viết bộ lọc chưa thấy, ba lượt): lời khuyên nguy hiểm tự nhiên còn lọt
    **29/145 (20,0%)**; cả bộ 30/175 (17,1%); câu an toàn bị chặn nhầm **7/74 (9,5%)**. Bản 1.5.0: lọt 27/32.
  - Ngưỡng đặt trước là lọt ≤ 10%: **không đạt**. Ba vòng sửa cho 23,5% → 20,8% → 20,0%: hướng lọc cụm từ đã
    chạm trần. Phần lọt nhiều nhất: làm yếu chống hú theo cách vòng (tạm dừng, hạ độ nhạy detector, nâng
    ngưỡng phát hiện, chỉ SOUNDCHECK) và làm notch nông đi.
  - Hai bộ câu đo độc lập nằm trong `tests/fixtures/copilot-safety-independent-r2.json` và `-r3.json` làm sàn
    hồi quy.
- **Khoá nào được gửi, tới đâu.**
  - App chỉ dùng khoá đã lưu trong `CÀI AI`; khoá trong biến môi trường không còn được dùng khi chạy từ app.
  - Khoá của loại "Tương thích OpenAI" gắn với máy chủ lúc lưu (giao thức + tên máy + cổng). Đổi địa chỉ sang
    máy chủ khác: ô khoá báo "Khoá đã lưu cho địa chỉ khác. Nhập lại khoá." và khoá cũ không được gửi.
    **Khoá lưu bằng 1.5.0 cho loại này phải lưu lại một lần.** Khoá Claude không đổi gì.
  - Địa chỉ `http://` không đi qua proxy của máy; `https://` vẫn dùng proxy hệ thống. Địa chỉ có `user:pass@`
    bị từ chối.
- **Tắt công tắc đồng ý luôn được lưu ngay**, kể cả khi ô model hay địa chỉ đang báo lỗi.
- **Gỡ cài đặt** hỏi riêng về file khoá API, kèm đường dẫn đầy đủ (mặc định giữ). Trước đây: chọn No thì file
  khoá ở lại, chọn Yes thì bị xoá cùng preset, cả hai đều không báo.
- **Bộ cài kèm `THIRD-PARTY-NOTICES.txt`** (quyết định Q22): ghi công JUCE, RTNeural, melatonin_blur, phông
  chữ, các thư viện JUCE mang theo, CPython và các gói Python trong `HandsFreeCopilot.exe`, dòng nhãn hiệu ASIO.

## Tester kiểm

1. Hỏi AI một câu thường: dưới câu trả lời có dòng cảnh báo, không đè lên chân `model · N in / M out`.
2. Nếu thấy AI khuyên tăng mức, tắt/tạm dừng chống hú, hạ độ nhạy, hay làm notch nông đi mà **không bị chặn**:
   gửi lại nguyên văn câu trả lời. Nếu một câu vô hại bị chặn: cũng gửi lại.
3. Loại "Tương thích OpenAI": lưu khoá, đổi địa chỉ sang máy chủ khác — ô khoá phải báo nhập lại.
4. Nhập địa chỉ sai, tắt công tắc đồng ý, đóng, mở lại `CÀI AI`: công tắc phải đang tắt.

## Giới hạn đã biết

- Bộ lọc là bộ lọc cụm từ: xem số đo ở trên. Coi mọi câu trả lời AI là lời khuyên chưa kiểm chứng.
- Vẫn **chưa có lượt gọi nhà cung cấp AI thật nào được kiểm** từ máy build (chưa có khoá).
- Đường gỡ cài mới chỉ được biên dịch kiểm (`makensis`), chưa chạy thử trình gỡ.
- Repo chưa ghi app dùng JUCE và ASIO SDK theo điều khoản nào (Q30, chờ owner).
