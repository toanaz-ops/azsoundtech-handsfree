# Ghi chú cho team test — AZ Soundtech Hands-free v1.3.0

Ngày build: 2026-09-16 · suite test 712/712 xanh (`17f5225`, nhánh
`feat/lane-m-active-soundcheck`) · installer: **Bản cài: `AZSoundtech-Handsfree-Setup-1.3.0.exe` · SHA-256 `C844186E2FBDD76EB345F50EDFD4E3FDC4A1025607C741F76B68321E05D05158` · 26 884 355 B (25,6 MB) · 2026-09-17 · gate 714/714.
tới khi owner duyệt** (xem cuối mục "Mới trong 1.3.0")

SHA-256 của Setup 1.3.0: **C844186E2FBDD76EB345F50EDFD4E3FDC4A1025607C741F76B68321E05D05158** · dung lượng: **26 884 355 B (25,6 MB)**

Bản 1.2.0 (đã đóng gói 2026-09-07 21:46, gate 547/547, 26 848 726 byte /
25,6 MB) SHA-256
`F701FFFCF563835EE9BE7D6ECAB623AC46EA0597ADC02B17ACFB3A00389C83F3`

Thư mục này luôn giữ **3 bản mới nhất** — cài bản số cao nhất trừ khi được
nhờ test bản cũ.

> **1.3.0 CÓ đụng đường audio, và đây là thay đổi lớn nhất tới nay.** Lần đầu
> tiên app **tự phát tín hiệu ra PA** (nút `ĐO`). **HẠ MASTER TRƯỚC**, mở âm
> lượng nhỏ, và đọc hết mục "Mới trong 1.3.0" bên dưới trước khi cắm vào dàn
> thật. Trong app **không có limiter** — chỉ có một kẹp cứng ±1.0.
>
> **1.2.0 cũng CÓ đụng đường audio**: độ sâu của một notch thay đổi theo thời
> gian — xem mục "Mới trong 1.2.0".

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

## Mới trong 1.3.0 (so với 1.2.0) — **HẠ MASTER TRƯỚC**

Bản này app **tự phát tín hiệu ra PA** lần đầu tiên. Từ trước tới giờ nó chỉ
trừ gain khỏi tiếng anh em đưa vào; giờ nó tự bắn một tiếng sweep ra loa để đo
phòng. Đọc hết mục này trước khi cắm vào dàn thật.

1. **HẠ MASTER TRƯỚC.** Sweep phát ở **20 dB dưới toàn thang của hệ, tại vị trí
   master hiện tại của anh em**. Dàn đang chạy show ở mức bình thường thì sweep
   nghe nhỏ hơn chương trình một chút. **Master mở hết thì 20 dB dưới toàn thang
   vẫn là rất to.** Mức đó chỉ chỉnh được **xuống**, không bao giờ lên. Và
   **trong app không có limiter** — giữa sweep và loa chỉ có một cái kẹp cứng
   ±1.0, tức clipper, không phải limiter.

2. **Bấm `ĐO`, xác nhận hộp thoại, rồi để yên.** Kênh ngõ ra đang được đo **im
   hoàn toàn 4,5 giây**, lần lượt từng kênh một. Hệ 4 ngõ ra mất khoảng **18
   giây**; hệ 8 slot stereo (16 kênh ngõ ra) mất khoảng **72 giây**. Hộp thoại
   in đúng con số của dàn đang cắm. **Đừng bấm khi MC đang nói.**

3. **`DỪNG` là nút dừng chính thức.** `Esc` thường cũng ăn nhưng **không bảo
   đảm** — nó chỉ tới được overlay khi overlay đang giữ focus. Từ lúc bấm `DỪNG`
   tới lúc im là khoảng **31 ms ở buffer 64** và **51 ms ở buffer 1024**, cộng
   độ trễ của driver và amp. **Phải KHÔNG có tiếng "cạch"** — ramp tắt 30 ms do
   chính callback audio sinh ra, nên nó chạy xong kể cả khi mọi thứ khác đã
   chết. Nghe thấy "cạch" là **báo lại ngay**.

4. **App chỉ ĐỀ XUẤT. Không gì được đặt cho tới khi bấm `ÁP DỤNG`.** `BỎ`, hoặc
   để hết 20 giây, ném đề xuất đi và không đụng vào notch nào.

5. **⚠️ Lần `ÁP DỤNG` THẬT ĐẦU TIÊN xảy ra trên dàn của anh em.** Không một test
   tự động nào trong dự án đi tới được đường đó — đường `ÁP DỤNG` chỉ chạy được
   khi có GUI thật và device thật. **Mở âm lượng thấp cho lần `ÁP DỤNG` đầu
   tiên.** Sau khi bấm, tối đa 6 bin mỗi làn bị cắt ở `−6 / −12 / −18 / −24 dB`
   (hoặc đúng trần của preset đang chạy, ví dụ `Music` là −10).

6. **Sau `ÁP DỤNG`, bảng báo cáo giữ console tới khi anh em bấm `BỎ`.** Không có
   đồng hồ đếm ngược ở bước này: định tuyến, tuning (Q/DEPTH), và hai nút
   GOOD/FALSE vẫn **khoá** cho tới lúc bấm `BỎ`. Chuyển mode (`AUTO` / `BYPASS`
   / `SOUNDCHECK`) và `CLEAR ALL` thì **dùng được ngay** — cái đó cố ý, để luôn
   có đường thoát. Đó là thiết kế, không phải app treo: **`BỎ` là một cú bấm.**

7. **Băng tin cậy dừng ở 6 kHz.** Sweep đi tới 10 kHz, nhưng một sweep log biên
   độ hằng bơm vào mỗi bin ở 10 kHz **ít hơn 20 dB** so với ở 100 Hz, nên phần
   6–10 kHz app **vẽ mờ** kèm nhãn "độ tin cậy thấp" và **không bao giờ đề
   xuất** cắt ở đó. **Cho chúng tôi biết có đáng mở rộng lên không.**

8. **Vòng hú qua các kênh ngõ ra KHÁC vẫn đóng trong lúc đo.** Loa của chúng vẫn
   đang phát tiếng mic. Phòng sát ngưỡng ở một kênh khác thì sweep **có thể**
   kích nó hú — đó chính là lý do app **từ chối bắt đầu** khi chip `RING RISK`
   đã đọc `RISING` trở lên. App đọc chip của **mọi slot đang bật**, không riêng
   slot đang hiện trên màn hình: bấm `ĐO` mà bị từ chối trong khi chip trên màn
   hình đang `LOW` thì **đúng là như vậy** — một slot khác đang ngân. Không phải
   lỗi.

9. **App tự dừng và nói lý do.** Chín lý do, mỗi lý do một câu riêng dưới đáy
   màn hình, ví dụ "tín hiệu mic quá lớn", "phòng đã hú sẵn trước khi phát",
   "không đo được nền nhiễu", "mất dữ liệu mic". Hai lý do đầu được quyết trong
   **0,5 giây đo nền, trước khi một mẫu sweep nào ra loa** — nên một lần chạy
   trong phòng đang ngân **phải dừng trước khi nghe thấy sweep**. Nghe thấy
   sweep rồi mới dừng là **báo lại ngay**.

10. **Ba câu "không phải phòng sạch".** Một kênh trả về **"không đo được"**
    nghĩa là mic không nghe đủ rõ loa (SNR thấp). **"sai định tuyến"** nghĩa là
    cặp kênh vào/ra của slot đó không hợp lệ với device đang cắm. **"thiếu trần
    cắt"** nghĩa là preset đang chạy không có trần để lượng tử độ sâu — app
    **không** vẽ một đường phẳng 0 dB cho ba trường hợp này, vì đường phẳng
    trông hệt như "phòng rất tốt".

11. **`CLEAR ALL` KHÔNG xoá sổ ledger của lần `ÁP DỤNG` trước.** App nhớ riêng
    một sổ (làn, index, tần số) cho mỗi slot để lần `ĐO` sau dọn đúng notch của
    lần trước. `CLEAR ALL` gỡ notch nhưng không đụng sổ đó. Hệ quả duy nhất nhìn
    thấy được: một lần `ÁP DỤNG` sau đó có thể báo `cleared_previous` lớn hơn số
    notch thật sự vừa gỡ. Không nguy hiểm, nhưng biết trước thì đỡ tưởng lỗi.

12. **Chạy lại `ĐO` chỉ dọn notch của chính lần `ÁP DỤNG` trước đó.** Notch do
    mode `SOUNDCHECK` 15 giây (nút cũ) đặt thì **không bị đụng tới**.

13. **Preset lưu sau soundcheck có mang notch phòng ngừa, nhưng nạp lại thì
    chúng tự nhả.** Nạp lại đưa chúng vào với `Origin::Preset`, nên chúng
    **tự nhả sau 30 giây yên tĩnh** như mọi notch preset khác. Muốn bảo vệ
    phòng ngừa đầy đủ trở lại thì **chạy `ĐO` lại**.

14. **Độ sâu không bao giờ nông hơn slider DEPTH đang để.** Nếu anh em kéo
    slider sâu hơn trong lúc đang xem kết quả, lúc `ÁP DỤNG` app lấy **cái sâu
    hơn** giữa đề xuất và slider. Đây là cái kẹp cuối trước PA.

15. **⚠️ Trong lúc `ĐO`, bộ chống hú TẮT trên MỌI slot.** Không phải chỉ kênh
    đang được quét: suốt cả lần chạy (hệ 16 kênh là ~72 giây) **cả dàn không có
    bảo vệ chống hú**, rồi mới bật lại theo mode đang chạy khi kết thúc. Hộp
    thoại xác nhận có in đúng câu này trước khi anh em bấm. **Phòng bắt đầu hú
    thì bấm `DỪNG` ngay** — trong lúc chạy đó là đường thoát.

**Cần báo lại, theo thứ tự giá trị:**

1. **Mở master lên từng dB cho tới lúc hú, TRƯỚC và SAU `ÁP DỤNG`. Chênh lệch
   đó là con số DUY NHẤT chứng minh tính năng này hoạt động.**
2. Đường `margin` trên màn hình so với một phép đo tham chiếu (REW / Smaart +
   mic đo): **lệch bao nhiêu dB, sai bao nhiêu bin?**
3. Bấm `DỪNG` giữa sweep: bao lâu thì im, **và có tiếng "cạch" không?**
4. 4,5 giây im mỗi kênh có vừa quy trình soundcheck của anh em không? ~72 giây
   có quá dài không?
5. Phòng ồn thì bao nhiêu kênh trả về **"không đo được"**?
6. Ứng viên lane M đề xuất có trùng với bin mà detector thật sự cắt trong show
   sau đó không?


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
   giây, mỗi bậc sau cách 10 giây, tới trần thì nhả hẳn. Một notch đã leo tới
   con số trên slider DEPTH cần **30–60 giây** để biến mất, tùy trần: **30 s**
   nếu trần −6 (đã ở đáy, nhả ngay ở bậc đầu), **40 s** từ trần −12 hoặc −10
   (Music), **50 s** từ trần −18 (Speech), **60 s** từ trần −24. **Trong
   khoảng từ 30 giây tới lúc đó, tone tại tần số ấy mất nhiều hơn 1.1.3** —
   1.1.3 đã nhả sạch ở 30 giây rồi.
4. **Phòng đang căng thì notch không lùi.** Đồng hồ nhả đọc **thẳng số của
   detector** (không qua chip): phòng còn căng (≥ ngưỡng RISING) thì đứng yên,
   **không có giới hạn thời gian**, cho **mọi notch của slot đó**, không riêng
   notch đang hú. Chip **RING RISK** trên màn hình là cùng ngưỡng nhưng có
   **hold 750 ms** và chỉ đọc slot đang hiển thị — chip báo LOW ở một slot
   khác không có nghĩa đồng hồ slot đó đang chạy lại; đừng suy đồng hồ từ chip
   của slot bạn không đang xem.
5. **Hú cũ quay lại trong 5 phút thì bị cắt sâu ngay.** App nhớ đúng tần số đó
   đã từng cần sâu bao nhiêu và đặt thẳng vào, không dò lại từ −6 dB.
6. **Không được có tiếng "cạch".** Mọi lần đổi độ sâu đều trải 10 ms. Nghe thấy
   "cạch" hoặc "zip" lúc notch đổi độ sâu là **lỗi phải báo**.
7. **⚠️ Đọc TRƯỚC khi bấm LOAD trên dàn thật: preset trần NÔNG hơn kéo notch
   đang sâu lên ngay.** Nạp một file mà trần thấp hơn trần đang chạy (vd.
   `Music.json` trần −10 dB trong khi rig đang đứng ở −18/−24) kéo **mọi
   notch Detector** đang sâu hơn trần mới lên ngay ở lượt dò kế tiếp — tới
   **+14 dB** năng lượng quay lại đúng tần số vừa hú, ramp 10 ms nên không có
   tiếng "cạch" nhưng vẫn là một cú tăng mức THẬT. Trần SÂU hơn thì không đổi
   gì lúc nạp. **Mở âm lượng nhỏ trước khi bấm LOAD, đừng LOAD giữa bài.**
   Preset/Manual/Soundcheck không bị kéo theo cách này.
8. **Ô Q/DEPTH có thể hiện một số không có trong danh sách.** Sau khi LOAD một
   file mang trần lệch bậc (vd. −10 dB của `Music.json`), ô đó hiện đúng số đó
   bằng chữ thay vì để trống — bình thường, không phải lỗi hiển thị; nó giữ
   nguyên cho tới khi bạn bấm chọn một mục trong danh sách.

**Báo lại ngay** nếu gặp: tiếng "cạch"/"zip" khi notch đổi độ sâu; một tiếng hú
app không bao giờ khống chế được; hoặc một notch **của detector** (dòng có cột
LANE trong ACTIVE NOTCHES, **không** phải notch từ Preset/Manual/Soundcheck)
sâu hơn con số trên slider DEPTH — đường đó không được phép.

Notch từ **Preset, Manual hoặc Soundcheck mang trần riêng của nó** (Q8) và
**được phép** đứng sâu hơn slider: một preset lưu sau khi vừa hú nạp lại ở
`deepestDb`, có thể tới −24 dB, dù slider đang để −6 — đúng là dòng chảy bước 3
của kịch bản test dưới đây. Đó là thiết kế, không phải lỗi. Kèm file log session
trong `%APPDATA%\AZSoundtech\HandsFree\logs\`.

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
   đang đứng lúc bấm; và LOAD đè lên notch đang sống đi đường ramp mượt 10 ms
   — không xóa filter, không được nghe thấy "cạch" — **chỉ khi** tần số/Q
   trong file khớp đúng notch đang đứng ở cùng index; lệch tần số hoặc Q thì
   vẫn reset như trước, không phải lỗi. LOAD cũng đọc lại **trần** từ file
   [`notchDefaults`] cho mọi slot Global — **mở âm lượng nhỏ trước khi thử
   LOAD `Music.json` (trần −10 dB) trên một rig đang cắt sâu hơn**, xem mục 7
   ở trên.)
4. SOUNDCHECK 15 giây với mic mở → app khóa các đỉnh tìm thấy (bảng ACTIVE
   NOTCHES); hết 15 giây app NGỪNG DÒ (đúng thiết kế) → bấm AUTO để chạy show.
5. AUTO: gây hú nhẹ (mic gần loa, gain thấp) → notch xuất hiện trong ~1 giây,
   marker trên phổ. Nhìn cột độ sâu trong **ACTIVE NOTCHES**: nó phải bắt đầu
   ở **−6** (hoặc −12) rồi đi sâu dần, chứ không nhảy thẳng vào số trên slider
   DEPTH. Hết hú: 30 giây sau nó nông đi một bậc, rồi mỗi 10 giây một bậc, tới
   −6 thì biến mất (**30–60 giây** tổng cộng, tùy bậc nó đang đứng khi hết hú:
   −6 → 30 s; −12 hoặc −10 → 40 s; −18 → 50 s; −24 → 60 s).
6. BYPASS: nghe passthrough trong suốt, không màu.
7. ROUTING: bật thêm slot 02, đổi kênh vào/ra → không click/pop, không sập.
8. Thanh ANALYSER: đổi averaging (0.1 s → 3 s), Peak hold, RANGE (gõ "60",
   "16k") → phổ phản hồi đúng.
9. Rút cáp input giữa chừng → app không crash, DEVICE báo ERROR.
10. Uninstall → thư mục + registry key biến mất sạch.

Báo lỗi kèm: Windows version, card tiếng, bước nào, hiện tượng, screenshot.
