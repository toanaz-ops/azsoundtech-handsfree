# Ghi chú cho team test — AZ Soundtech Hands-free v1.2.0

Ngày build: 2026-09-07 · suite test 535/535 xanh · installer: kích thước điền
sau khi đóng gói

SHA-256 của Setup 1.2.0:
`<điền sau khi installer\release-alpha.ps1 -Part minor chạy xong>`

Thư mục này luôn giữ **3 bản mới nhất** — cài bản số cao nhất trừ khi được
nhờ test bản cũ.

> **1.2.0 CÓ đụng đường audio.** Độ sâu của một notch nay thay đổi theo thời
> gian. **Mở âm lượng nhỏ trước** và đọc mục "Mới trong 1.2.0" bên dưới trước
> khi cắm vào PA thật.

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

## Mới trong 1.2.0 (so với 1.1.3) — **nghe ở âm lượng thấp trước**

Từ trước tới nay, notch cắt thẳng đúng con số trên slider **DEPTH** (thường
−18 dB) rồi biến mất hẳn sau 30 giây im. Bản này đổi cả hai đầu.

1. **Notch bắt đầu NÔNG rồi mới sâu dần.** Đặt ở −6 dB (−12 nếu đỉnh lên rất
   dốc), rồi cứ **6 dB mỗi 300 ms** chừng nào còn hú. Hệ quả anh em sẽ nghe:
   **một tiếng hú bùng nhanh có thể kêu lâu hơn 1.1.3 khoảng 0,3–0,6 giây**
   trước khi bị cắt hết. **Mở nhỏ âm lượng lần thử đầu.**
2. **Một notch không cần sâu sẽ đứng mãi ở −6 hoặc −12 — đúng, không phải
   lỗi.** Đó chính là cái được của bản này: chỉ cắt đúng lượng phòng cần, tone
   giữ được nhiều hơn.
   **Và đây là điều duy nhất không một test tự động nào trong dự án kiểm tra
   được** — chỉ dàn thật mới trả lời. Nên: **nếu trên dàn của anh em MỌI notch
   đều kết thúc đúng bằng con số trên slider DEPTH, hãy báo lại.** Đó là dấu
   hiệu máy đang đọc sai phổ, không phải chuyện sở thích.
3. **Hết hú thì notch lùi từng bậc, không biến mất một phát.** Bậc đầu ở 30
   giây, mỗi bậc sau cách 10 giây, tới −6 dB thì nhả hẳn. Một notch đã leo tới
   con số trên slider DEPTH cần **40–60 giây** để biến mất (40 s từ −12, 50 s
   từ −18, 60 s từ −24). **Trong khoảng từ 30 giây tới lúc đó, tone tại tần số
   ấy mất nhiều hơn 1.1.3** — 1.1.3 đã nhả sạch ở 30 giây rồi.
4. **Phòng đang căng thì notch không lùi.** Chip **RING RISK** báo **RISING**
   hoặc **CRITICAL** → đồng hồ nhả đứng yên, **không có giới hạn thời gian**,
   và đứng cho **mọi notch của slot đó**, không riêng notch đang hú.
5. **Hú cũ quay lại trong 5 phút thì bị cắt sâu ngay.** App nhớ đúng tần số đó
   đã từng cần sâu bao nhiêu và đặt thẳng vào, không dò lại từ −6 dB.
6. **Không được có tiếng "cạch".** Mọi lần đổi độ sâu đều trải 10 ms. Nghe thấy
   "cạch" hoặc "zip" lúc notch đổi độ sâu là **lỗi phải báo**.

**Báo lại ngay** nếu gặp: tiếng "cạch"/"zip" khi notch đổi độ sâu; một tiếng hú
app không bao giờ khống chế được; hoặc một notch **sâu hơn** con số trên slider
DEPTH (không đường nào được phép). Kèm file log session trong
`%APPDATA%\AZSoundtech\HandsFree\logs\`.

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
   phản ánh đúng file. (Từ 1.2.0: SAVE ghi độ sâu phòng đã CẦN, không phải bậc
   đang đứng lúc bấm; và LOAD đè lên notch đang sống nay chuyển mượt 10 ms chứ
   không xóa filter — không được nghe thấy "cạch".)
4. SOUNDCHECK 15 giây với mic mở → app khóa các đỉnh tìm thấy (bảng ACTIVE
   NOTCHES); hết 15 giây app NGỪNG DÒ (đúng thiết kế) → bấm AUTO để chạy show.
5. AUTO: gây hú nhẹ (mic gần loa, gain thấp) → notch xuất hiện trong ~1 giây,
   marker trên phổ. Nhìn cột độ sâu trong **ACTIVE NOTCHES**: nó phải bắt đầu
   ở **−6** (hoặc −12) rồi đi sâu dần, chứ không nhảy thẳng vào số trên slider
   DEPTH. Hết hú: 30 giây sau nó nông đi một bậc, rồi mỗi 10 giây một bậc, tới
   −6 thì biến mất (40–60 giây tổng cộng).
6. BYPASS: nghe passthrough trong suốt, không màu.
7. ROUTING: bật thêm slot 02, đổi kênh vào/ra → không click/pop, không sập.
8. Thanh ANALYSER: đổi averaging (0.1 s → 3 s), Peak hold, RANGE (gõ "60",
   "16k") → phổ phản hồi đúng.
9. Rút cáp input giữa chừng → app không crash, DEVICE báo ERROR.
10. Uninstall → thư mục + registry key biến mất sạch.

Báo lỗi kèm: Windows version, card tiếng, bước nào, hiện tượng, screenshot.
