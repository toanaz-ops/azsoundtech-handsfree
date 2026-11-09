# Ghi chú cho team test — AZ Soundtech Hands-free v1.5.0

Ngày build: 2026-11-09 (`main` `5d47b9e`) · gate ctest 951/951 · installer
`AZSoundtech-Handsfree-Setup-1.5.0.exe`.

SHA-256 của Setup 1.5.0: **BA205503520E3890FC24B737E7A01334DF8C67511DABF348D9590A65CB0C16BE** · dung lượng: **42 137 569 B (40,2 MB)** — lớn hơn 1.4.1 khoảng 14,5 MB vì lần đầu kèm `HandsFreeCopilot.exe`.

Bản 1.4.1 (build 2026-11-09 19:23, `main` `7408f73`, gate 847/847, 26 919 339 B / 25,7 MB) SHA-256
`616C62E85FD963CD84E331745770ED85719ABEA6EAAF898928CB552FBD4391BB`

Bản 1.4.0 (build 2026-11-09 18:02, `main` `2628cdf`, gate 833/833, 26 917 930 B / 25,7 MB) SHA-256
`4A627B878A5E84866094F30B3AB5DFC51329811F08A238DBE0038976F95E8D6F`

Bản 1.3.0 (build 2026-09-16, suite 712/712 tại `17f5225`, gate 714/714 ngày 2026-09-17,
26 884 355 B / 25,6 MB) SHA-256
`C844186E2FBDD76EB345F50EDFD4E3FDC4A1025607C741F76B68321E05D05158`

Bản 1.2.0 (đã đóng gói 2026-09-07 21:46, gate 547/547, 26 848 726 byte /
25,6 MB) SHA-256
`F701FFFCF563835EE9BE7D6ECAB623AC46EA0597ADC02B17ACFB3A00389C83F3`

Thư mục này hiện giữ **mọi bản từ 1.1.3 đến 1.5.0** (đóng gói không dọn bản cũ) —
cài bản số cao nhất trừ khi được nhờ test bản cũ.

> **ĐÍNH CHÍNH 09/11/2026 cho 1.5.0 — đọc trước khi dùng ô hỏi AI.** Bản ghi chú này và release note
> từng viết rằng câu trả lời khuyên tăng gain hay tắt chống hú "bị thay bằng một thông báo cố định".
> Soát lại sau khi phát hành cho thấy điều đó **không đúng với phần lớn cách nói thông thường**: bộ lọc bỏ
> lọt những câu như "mở monitor to hơn", "tắt tính năng chống hú ở slot này", "để gain ở +3", "xoá notch
> này đi", và cả câu trả lời bằng ngôn ngữ khác. Nó cũng chặn nhầm một số lời khuyên an toàn.
> **Coi mọi câu trả lời của AI là lời khuyên chưa được kiểm chứng. Không tăng gain, âm lượng, trần độ sâu,
> không xoá notch, không tắt hay bypass chống hú chỉ vì AI bảo.** Nút `PHÂN TÍCH` (không dùng AI) không bị
> ảnh hưởng. Ba điều nữa về khoá và cài đặt, cho tới khi có bản sửa:
> 1. Chế độ "Tương thích OpenAI": nếu máy có biến môi trường `OPENAI_API_KEY`, khoá đó **được gửi** tới địa
>    chỉ bạn nhập dù màn hình báo "Chưa có khoá"; và khoá đã lưu sẽ đi theo **bất kỳ** địa chỉ nào bạn đổi
>    sang. Đừng trỏ địa chỉ sang nhà cung cấp khác với nhà cung cấp của khoá.
> 2. Máy có cấu hình proxy HTTP: đừng dùng địa chỉ `http://localhost…` hay `http://127.0.0.1…` kèm khoá —
>    khoá có thể đi tới proxy ở dạng không mã hoá.
> 3. Tắt công tắc đồng ý trong lúc một ô khác đang báo lỗi thì thay đổi **không được lưu**. Mở lại `CÀI AI`
>    để kiểm công tắc đã tắt thật chưa.
>
> **1.5.0: 0 dB ở mọi ngõ ra.** Không đổi hệ số lọc, gain, buffer, clamp hay quyết định đặt notch. Thêm bảng `COPILOT` (Geek mode), ô hỏi AI bằng khoá của chính bạn, và log ghi thêm ứng viên suýt thành notch — xem mục "Mới trong 1.5.0". **File cài lần đầu kèm một chương trình phụ (`HandsFreeCopilot.exe`): antivirus có thể hỏi.**
>
> **1.4.1: 0 dB ở mọi ngõ ra.** Detector nghe riêng từng làn, và sửa routing một slot không còn đặt lại 7 slot kia — xem mục "Mới trong 1.4.1".
>
> **1.4.0 không đổi DSP/gain/buffer so với 1.3.0**, nhưng **routing lần trước được
> khôi phục tự động lúc mở app → hạ master trước** (xem mục "Mới trong 1.4.0").
>
> **1.3.0 CÓ đụng đường audio, và đây là thay đổi lớn nhất tới nay.** Lần đầu
> tiên app **tự phát tín hiệu ra PA** (nút `ĐO`). **HẠ MASTER TRƯỚC**, mở âm
> lượng nhỏ, và đọc hết mục "Mới trong 1.3.0" bên dưới trước khi cắm vào dàn
> thật. Trong app **không có limiter** — chỉ có một kẹp cứng ±1.0.
>
> **1.2.0 cũng CÓ đụng đường audio**: độ sâu của một notch thay đổi theo thời
> gian — xem mục "Mới trong 1.2.0".

## Mới trong 1.5.0 (so với 1.4.1) — 0 dB ở mọi ngõ ra

Chi tiết: `docs/release-notes/1.5.0-alpha.md`. Không đụng đường audio.

1. **Bảng `COPILOT`** (Geek mode, góc dưới trái, dưới ACTIVE NOTCHES). Chạy một buổi tới khi có
   ít nhất ba notch, chấm một cái FALSE, bấm `PHÂN TÍCH`: các nhận xét có khớp với việc vừa xảy
   ra không? Bước này không dùng mạng, không dùng khoá, không có model AI.
2. **Lần bấm đầu sau khi cài mất bao lâu?** Chương trình phụ khởi động nguội 1,5–7,5 giây trên
   máy build; antivirus quét có thể làm chậm hơn. **Antivirus có cảnh báo `HandsFreeCopilot.exe`
   không?** Nếu chương trình phụ bị chặn, bảng chỉ hiện một dòng trạng thái, phần chống hú vẫn chạy.
3. **`CÀI AI` + ô hỏi** (tuỳ chọn, cần khoá API của chính bạn). Lớp phủ che cột trái tới khi bấm
   `ĐÓNG`; nút BYPASS bên phải vẫn bấm được. `HỎI` gửi câu hỏi và log buổi diễn (không có audio)
   tới nhà cung cấp bạn chọn. **Chưa có lượt gọi nhà cung cấp thật nào được kiểm** — độ trễ, số
   token và lỗi riêng của từng nhà cung cấp là chưa biết.
4. **Khoá API** được cất bằng DPAPI của Windows: chặn người dùng Windows khác và việc đọc ổ đĩa
   rời, **không** chặn phần mềm độc hại chạy dưới chính tài khoản của bạn — đừng dùng khoá có hạn
   mức lớn để thử. Giới hạn đã biết: các bản sao khoá nằm thoáng qua trong ô nhập được giải phóng
   mà không bị ghi đè số 0.
5. **Bộ lọc câu trả lời yếu hơn mô tả ban đầu — xem ĐÍNH CHÍNH ở đầu file.** Nó chỉ bắt được một số
   cách nói; phần lớn lời khuyên tăng gain / tắt chống hú diễn đạt tự nhiên vẫn lọt qua. Đừng làm
   theo lời AI khi nó bảo tăng mức hay gỡ bảo vệ; báo lại câu trả lời đó.
6. **Log buổi diễn có thêm dòng `candidate_features`** (ứng viên suýt thành notch). Bạn không cần
   làm gì khác: cứ chấm TRUE/FALSE như cũ và **gửi lại file `session-*.jsonl`** — đây là dữ liệu
   để huấn luyện bộ phân loại hú / nốt nhạc ở bước sau. Tệ nhất khoảng 1,4 MB mỗi giờ mỗi slot.
7. Thu nhỏ cửa sổ theo chiều cao: bảng `COPILOT` phải biến mất và ACTIVE NOTCHES trông y như cũ.

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

## Mới trong 1.4.1 (so với 1.4.0) — 0 dB ở mọi ngõ ra

Chi tiết: `docs/release-notes/1.4.1-alpha.md`.

1. **Detector nghe riêng từng làn.** Hai mic vào CÙNG một kênh ra, cho hú ở một vòng: chỉ slot
   của vòng đó đặt notch, slot kia không. Báo lại nếu hai slot cùng cắt một tần số.
2. **Sửa routing một slot không đặt lại các slot khác.** Chạy 2 slot, để slot 0 giữ một notch, sửa
   routing slot 1: notch và chip RING RISK của slot 0 phải còn nguyên.
3. **Đổi buffer trong control panel ASIO lúc đang chạy**: sau khi driver chạy lại, mọi chip RING
   RISK phải quay về đọc số (không kẹt N/A) và notch vẫn đặt được.

## Mới trong 1.4.0 (so với 1.3.0) — giao diện, không đổi âm thanh

**Không đổi DSP/gain/buffer.** Cùng cấu hình thì mức ra y như 1.3.0. Chi tiết:
`docs/release-notes/1.4.0-alpha.md`.

1. **Mở lần đầu**: máy đã có preset từ bản trước mở ở **Geek** (như cũ); máy
   mới cài mở ở **Đơn giản** và tự hiện trình cài đặt 3 bước.
2. **App nay nhớ cấu hình** (driver, sound card, rate, buffer, routing, chế độ).
   **Hạ master trước khi mở app trên rig thật** — routing lần trước tự quay lại.
3. **Rút sound card rồi mở app** → mọi đường bị tắt, đèn "Cần cài đặt"; cắm lại
   và chọn đúng card thì routing cũ quay lại. Báo lại nếu app chạy routing cũ
   lên một card KHÁC.
4. **Chế độ Đơn giản**: thử `ĐO PHÒNG` (cùng hộp thoại hạ master như `ĐO`),
   `BẬT CHỐNG HÚ`, `TẮT`. Lúc đang đo, `TẮT` bị khoá — dùng `DỪNG`.
5. Trình cài đặt bước 2: nói vào mic, thanh mức của kênh đúng phải nhảy. Báo lại
   nếu thanh nhảy ở kênh sai.

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
