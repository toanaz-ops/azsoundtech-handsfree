# Ghi chú cho team test — AZ Soundtech Hands-free v1.1.3

Ngày build: 2026-09-06 · suite test 454/454 xanh · installer ~26.8 MB

SHA-256 của Setup 1.1.3:
`0B09D7FDBF71B39C31EF9114800CDF10523273E4C817DC68E15EC5AEB7376E26`

Thư mục này luôn giữ **3 bản mới nhất** — cài bản số cao nhất trừ khi được
nhờ test bản cũ.

## Biết trước để không tưởng app hỏng

1. **SmartScreen chặn lần đầu** — file chưa ký số. "More info" → "Run
   anyway". Hành vi đúng của bản unsigned, không phải virus.
2. **Chưa kèm driver ASIO** (licence Steinberg). Không có ASIO thì app chạy
   Windows Audio — vẫn test được mọi thứ, chỉ đừng đo latency thật.
3. **Ô RING RISK — cách đọc (từ 1.1.3 đã có dữ liệu thật).** Chip đọc số của
   detector trên slot đang theo dõi:
   - **N/A** — chưa có gì để nói: detection đang tắt, detector chưa đủ lịch sử,
     hoặc vừa đổi slot / đổi device. **Không phải bug.**
   - **LOW** — có đo, phòng đang yên.
   - **RISING** — có gai đang lên, sắp đặt notch.
   - **CRITICAL** — detector đã vượt ngưỡng đặt notch; thường thấy dòng notch
     mới trong ACTIVE NOTCHES ngay sau, trừ khi bảng đã đầy, bin đó đang bị
     chặn, hoặc đỉnh chưa trụ đủ số frame liên tiếp (persistence).

   Chip đã bước lên thì **giữ 750 ms** trước khi được bước xuống — để nó không
   nháy trên tín hiệu sát ngưỡng. **Khi rig dừng hoặc đang restart, chip đứng
   nguyên ở mức đọc cuối** thay vì về N/A: khoảng trống đã biết của bản này,
   đừng tin chip lúc máy không chạy. Báo lại nếu: chip **đỏ mà không hú**, hoặc
   **hú mà chip vẫn LOW**.

## Mới trong 1.0.5 (so với 1.0.4)

- **Chuỗi preset trọn vẹn.** Installer nay chép sẵn hai preset `Speech` và
  `Music` vào máy; lần đầu mở app tự nạp chúng vào
  `%APPDATA%\AZSoundtech\HandsFree\presets\` — **không bao giờ ghi đè** file
  anh em đã sửa. Dưới mục **INTERFACE** có hai nút mới **LOAD… / SAVE…**:
  LOAD nạp một preset `*.json`, SAVE lưu trạng thái notch đang chạy ra file
  mở lại được. (Preset đóng gói chỉ mang Q/độ sâu mặc định, chưa khóa notch
  nào — nạp xong app vẫn tự dò như thường.)

## Kịch bản test đề nghị (15 phút)

1. Cài → mở app ở **âm lượng nhỏ nhất**.
2. INTERFACE: chọn driver/device → masthead hiện DEVICE OK + CPU %.
3. Kiểm tra preset: mở
   `%APPDATA%\AZSoundtech\HandsFree\presets\` → có `Speech.json` + `Music.json`.
   Bấm **SAVE…** lưu một file, rồi **LOAD…** nạp lại → app không sập, notch
   phản ánh đúng file.
4. SOUNDCHECK 15 giây với mic mở → app khóa các đỉnh tìm thấy (bảng ACTIVE
   NOTCHES); hết 15 giây app NGỪNG DÒ (đúng thiết kế) → bấm AUTO để chạy show.
5. AUTO: gây hú nhẹ (mic gần loa, gain thấp) → notch xuất hiện trong ~1 giây,
   marker trên phổ; hết hú ~30 giây notch tự nhả.
6. BYPASS: nghe passthrough trong suốt, không màu.
7. ROUTING: bật thêm slot 02, đổi kênh vào/ra → không click/pop, không sập.
8. Thanh ANALYSER: đổi averaging (0.1 s → 3 s), Peak hold, RANGE (gõ "60",
   "16k") → phổ phản hồi đúng.
9. Rút cáp input giữa chừng → app không crash, DEVICE báo ERROR.
10. Uninstall → thư mục + registry key biến mất sạch.

Báo lỗi kèm: Windows version, card tiếng, bước nào, hiện tượng, screenshot.
