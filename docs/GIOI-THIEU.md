# AZ Soundtech Hands-free — Giới thiệu

> Phần mềm chống hú (feedback elimination) tự động cho âm thanh sự kiện live.
> Windows · Standalone · ASIO · Freeware (quyết định D-07, xem ghi chú cuối).

## Vấn đề

Hú micro (acoustic feedback) là cơn ác mộng của mọi buổi show: micro hái tiếng
của chính loa nó đang phát ra, vòng lặp khuếch đại chạy lên một tần số nào đó và
phát ra tiếng rít sắc như dao. Người xử lý sound truyền thống phải:

1. **Nghe** để nhận ra đang hú,
2. **Suy đoán** tần số hú bằng tai,
3. **Tìm** slider EQ tương ứng,
4. **Cắt** đủ sâu mà không cắt oan dải tần khác,

— tất cả trong vài giây, giữa tiếng hú đang xé tai khán giả.

## Giải pháp

AZ Soundtech Hands-free làm thay 4 bước đó **tự động và liên tục**:

```
Micro ──► Mixer (Wing/iD14...) ──► [Hands-free] ──► Loa/PA
                                        │
                            phát hiện tần số hú trong ~1 giây
                            tự đặt filter notch siêu hẹp đúng chỗ
                            nhả dần từng bậc khi hết hú
```

App cắm vào đường tín hiệu như một thiết bị insert: lấy audio từ bất kỳ audio
interface ASIO nào, phân tích phổ bằng FFT trên một luồng nền, và khi thấy một
tần số đang "vươn lên" thành hú thì đặt ngay một bộ lọc notch cực hẹp đúng tần
số đó — chỉ khoét đúng chỗ hú, gần như không ảnh hưởng chất lượng nhạc.

## Tính năng chính

| | |
|---|---|
| **Phát hiện tự động** | FFT 2048 điểm, chấm điểm "độ nhọn" của đỉnh phổ — phân biệt tiếng hú (một gai duy nhất nhô lên) với nhạc (nhiều đỉnh tự nhiên). Thời gian khóa mục tiêu trong ~1 giây kể từ khi hú đạt ngưỡng |
| **Notch siêu hẹp** | 16 notch/làn, mỗi slot mono (1 làn) hoặc stereo (2 làn), 8 slot — tối đa 256 chuỗi notch. Q chỉnh được 8–50 (mặc định 30). Độ sâu −6 đến −24 dB (mặc định −18) nay là **TRẦN**: notch đặt ở −6 dB (hoặc −12 nếu đỉnh lên dốc) rồi chỉ đào sâu thêm 6 dB mỗi 300 ms chừng nào bin đó còn hú, không bao giờ quá trần. Chỉ mất đúng vài Hz quanh tần số hú |
| **Routing 8 slot** | 8 slot xử lý độc lập, mỗi slot tự chọn kênh vào/ra bất kỳ của interface — không còn cố định stereo in/out. Mặc định chỉ slot 01 bật |
| **Stereo độc lập** | Mic hú qua loa trái thì chỉ cắt cánh trái; nút LINK mỗi slot để cắt cả hai bên như trước |
| **Chấm notch để app học** | Mỗi dòng ACTIVE NOTCHES có nút **GOOD** / **FALSE**. FALSE xóa notch ngay và ghi nhãn "cắt oan"; GOOD ghi nhãn "cắt đúng". Nhãn + phổ lúc quyết định vào log session trên máy (không audio) — dữ liệu cho bộ phân loại ở bản sau |
| **Nhả dần theo bậc** | Hết hú 30 giây → nông đi 6 dB; mỗi 10 giây yên tiếp theo nông thêm một bậc; tới trần thì nhả hẳn (**30–60 giây** tùy trần đang đứng: trần −6 là 30 s vì đã ở đáy, −12/−10 là 40 s, −18 là 50 s, −24 là 60 s). Hú quay lại là kẹp ngay về bậc sâu nhất đã từng đứng. Phòng đang căng (chip RING RISK ≥ RISING) thì đồng hồ **đứng yên** — mọi notch của slot đó giữ nguyên bậc chừng nào phòng còn căng |
| **Nhớ phòng 5 phút** | Hú quay lại đúng bin cũ trong 5 phút sau khi nhả hẳn → đặt lại thẳng ở độ sâu đã từng cần, không dò lại từ −6 dB. Bộ nhớ chỉ **làm sâu hơn**, không bao giờ làm nông đi, và **dùng một lần** |
| **Chống báo nhầm harmonic** | Nếu đã khóa tần số F thì bậc harmonics 1.4F–4.1F bị trừ điểm, tránh cắt oan bội số của nốt nhạc |
| **3 chế độ** | **Bypass** (thông tuyến thuần), **Auto** (chạy liên tục), **Soundcheck** (nghe 15 giây đầu show, khóa mọi đỉnh tìm thấy, không tự nhả; hết 15 giây detector tự ngừng dò — bấm **Auto** để chạy tiếp) |
| **Soundcheck đo chủ động (`ĐO`)** | Nút **`ĐO`** riêng (không phải mode `SOUNDCHECK` 15 giây cũ): app **tự phát sweep 100 Hz – 10 kHz trong 3 giây ra từng kênh ngõ ra một** để đo loop gain của phòng **trước khi** phòng kịp hú. Kênh đang đo **im hoàn toàn 4,52 giây** — mọi làn của mọi slot định tuyến vào kênh đó đều bị tắt — nên hệ 4 ngõ ra mất ~18 giây, hệ 8 slot stereo (16 kênh) mất **~72 giây**. App **chỉ đề xuất**, không đặt gì cho tới khi bấm **`ÁP DỤNG`**; trên **6 kHz** bản này **vẽ nhưng không đề xuất**. **Hạ master trước**: sweep phát ở 20 dB dưới toàn thang tại master hiện tại, và app không có limiter. Xem mục riêng bên dưới |
| **Chỉnh độ nhạy một nút** | ONE-KNOB RESPONSE: **SAFE** / **BALANCED** / **AGGRESSIVE** (tự chuyển **CUSTOM** khi chỉnh tay); mỗi slot chọn theo tuning **Global** chung hoặc **Custom** riêng |
| **Chọn device trực quan** | Chọn driver/device/sample rate/buffer ngay trong app; hiện latency và trạng thái theo thời gian thực |
| **Preset có sẵn** | `Speech` (Q=40, trần −18 dB — hà khắc cho loa hội thoại) và `Music` (Q=25, trần −10 dB — dịu cho nhạc sống) đúng giá trị trong repo. Trần −10 của Music **tới được**: theo Q13 thang hiệu lực là −6 → −10, bậc cuối chính là trần, nên Music vẫn cắt đủ 10 dB như 1.1.3 chứ không bị lượng tử về −6. Installer chép hai preset vào máy, app tự seed chúng vào `%APPDATA%` lúc first-run (không bao giờ ghi đè file người dùng đã sửa), và GUI có hai nút **LOAD… / SAVE…** dưới mục INTERFACE để nạp/lưu preset (`*.json`). Từ 1.2.0 file lưu ra mang **độ sâu phòng đã cần** (`deepestDb`) và mang cả trần trong `notchDefaults`, nên nạp lại đúng như lúc lưu — nạp một file **có** khối `notchDefaults` đặt lại trần đó cho mọi slot đang dùng tuning Global (slot Custom giữ trần riêng), file **không có** khối đó thì không đụng tới trần đang chạy. **Lưu ý**: một preset lưu sau khi vừa hú sẽ nạp lại notch ở `deepestDb` (có thể tới −24 dB) bất kể slider đang để bao nhiêu — cắt nhiều hơn một file lưu trên 1.1.3, nhưng luôn theo hướng an toàn (chỉ cắt thêm). **⚠️ Cảnh báo, đọc trước khi LOAD trên dàn thật**: nạp một file mà trần NÔNG hơn trần đang chạy (ví dụ `Music.json` trần −10 trong khi rig đang đứng ở −18/−24) kéo mọi notch Detector đang sâu hơn lên ngay ở lượt dò kế tiếp — tới **+14 dB** năng lượng quay lại đúng tần số vừa hú, ramp 10 ms không có tiếng "cạch" nhưng vẫn là một cú tăng mức thật; trần SÂU hơn thì 0 dB ngay lúc nạp. Mở âm lượng thấp trước khi LOAD, đừng LOAD giữa bài. **Bất đối xứng của soundcheck đo chủ động (1.3.0)**: preset lưu sau khi chạy `ĐO` **có** mang notch phòng ngừa, nhưng nạp lại đưa chúng vào với `Origin::Preset` nên chúng **tự nhả sau 30 giây yên tĩnh** như mọi notch preset khác — muốn bảo vệ phòng ngừa đầy đủ trở lại thì **chạy `ĐO` lại** |
| **An toàn theo thiết kế** | Không cấp phát bộ nhớ hay khóa mutex trên đường audio real-time; từ chối độ sâu dương (điều gì sẽ xảy ra nếu một lỗi đánh dấu biến notch thành máy khuếch đại hú?); tự vô hiệu notch khi đổi sample rate khiến notch vượt Nyquist |

## Thông số nhanh

| | |
|---|---|
| Nền tảng | Windows 10/11 64-bit, standalone (không phải plugin) |
| Audio I/O | ASIO (khuyên dùng) hoặc Windows Audio/DirectSound dự phòng |
| Sample rate | Theo device báo về (không phải danh sách cứng) |
| Kênh | 8 slot xử lý độc lập; mỗi slot mono hoặc stereo, tự chọn kênh vào/ra của interface |
| Độ trễ thêm vào | Vài ms tùy buffer size (app hiển thị số thật) |
| Điểm mù đã biết | Dưới ~117 Hz ở 48 kHz (~107 Hz ở 44.1 kHz, ~234 Hz ở 96 kHz; công thức 5 × sample rate / 2048) do cần 5 bin headroom hai bên cho phép chấm điểm — hú sub-bass vẫn phải xử lý bằng EQ tay |

## Cài đặt & sử dụng

Chạy installer `AZSoundtech-Handsfree-Setup-x.y.z.exe`, mở app, chọn device →
sample rate → buffer nhỏ nhất ổn định. Cấu hình bảng **ROUTING** — mặc định chỉ
slot 01 bật, thêm slot/chọn kênh vào-ra nếu cần — rồi bật **Soundcheck** 15 giây
trước show để "soi" sẵn phòng, sau đó chuyển **Auto** và quên nó đi.

## Theo dõi khi chạy

Masthead hiện **CPU %** và trạng thái **DEVICE** (OK/ERROR) theo thời gian
thực. Màn hình phổ (RTA) có averaging Off/0.1/0.3/0.5/1/3/5/10 s, peak hold và
độ phân giải 1/1–1/3 octave; tab per-slot theo dõi từng slot riêng. Với slot
stereo, bộ chọn **L/R** trên thanh analyser đổi làn đang vẽ, notch của làn phải
vẽ bằng nét đứt kèm nhãn "R", và bảng **ACTIVE NOTCHES** có cột **LANE** cho
biết mỗi notch nằm ở làn nào. Chip **RING RISK** (góc phải thanh
analyser) cho biết phòng đang gần hú tới đâu, đọc thẳng số của detector trên
slot đang theo dõi: **N/A** = chưa có dữ liệu (detection tắt, detector chưa đủ
lịch sử, hoặc vừa đổi slot/đổi device); **LOW** = yên; **RISING** = detector
sắp đặt notch; **CRITICAL** = detector đã vượt ngưỡng đặt notch tại tần số đó —
thường sẽ thấy notch mới trong **ACTIVE NOTCHES** ngay sau, trừ khi bảng đã đầy,
bin đó đang bị chặn (guard), hoặc đỉnh chưa trụ đủ số frame liên tiếp
(persistence). Chip đã bước lên thì giữ ít nhất **750 ms** trước khi được bước
xuống, nên nó không nháy trên tín hiệu sát ngưỡng. Lưu ý một khoảng trống đã
biết: khi rig **dừng hoặc đang restart** thì không còn snapshot nào được publish,
nên chip **đứng nguyên ở mức đọc cuối** thay vì về N/A — đừng tin chip khi máy
không chạy.

## Chấm notch để app học

Thấy app cắt oan một nốt nhạc: bấm **FALSE** trên dòng đó — notch nhả ngay và app ghi
lại "đây là cắt oan". Thấy nó cắt đúng tiếng hú: bấm **GOOD**. Không bắt buộc, nhưng mỗi
lần bấm là một mẫu huấn luyện. Nếu một tiếng hú **quay lại đúng tần số cũ**, dòng đó
hiện lại hai nút trắng: đó là một notch MỚI và nó chờ đánh giá mới, không mang theo
GOOD/FALSE của lần trước. Log nằm ở `%APPDATA%\AZSoundtech\HandsFree\logs\`, một
file mỗi lần mở app, không chứa audio; gửi file khi được hỏi. Nếu app **không ghi được
log** (thư mục bị chặn), dòng trạng thái dưới đáy báo `LOG: khong ghi duoc ...` — app vẫn
chạy bình thường, chỉ là không có file để gửi.

## Soundcheck đo chủ động — nút `ĐO` (1.3.0)

Nút **`ĐO`** là nút riêng, **không phải** mode `SOUNDCHECK` 15 giây cũ (mode cũ
giữ nguyên nghĩa cũ: ngồi nghe và chờ phòng tự hú). `ĐO` làm điều ngược lại —
**app tự phát tín hiệu ra PA để đo phòng trước khi phòng kịp hú.**

**⚠️ HẠ MASTER TRƯỚC.** Sweep phát ở **20 dB dưới toàn thang của hệ, tại vị trí
master hiện tại của bạn**. Dàn đang chạy ở mức bình thường thì sweep nghe nhỏ
hơn chương trình một chút; master mở hết thì 20 dB dưới toàn thang **vẫn rất
to**. App không biết SPL tuyệt đối và không có phép quy đổi nào đúng nếu không
biết gain của bàn và amp — nên đây là câu trung thực duy nhất nói được. Mức phát
chỉ chỉnh được **xuống**. Và **trong app không có limiter**: giữa sweep và
driver chỉ có một kẹp cứng ±1.0 (một clipper toàn thang), độ to thật do fader
master quyết định.

**Một lượt đo, từng kênh ngõ ra một.** 0,5 s đo nền nhiễu → 20 ms chạy đà im
lặng → **sweep log 100 Hz – 10 kHz trong 3,0 giây** → 0,7 s đuôi (đo phần ngân)
→ 0,3 s nghỉ. Tổng **4,52 giây mỗi kênh ngõ ra**, và trong suốt 4,52 giây đó
kênh ấy **im hoàn toàn**: **mọi làn của mọi slot** định tuyến vào kênh đó đều bị
tắt, không phải chỉ một `(slot, làn)` — vì nhiều slot cộng dồn lên cùng một kênh
ra, tắt một cặp thì vòng hú của kênh đó vẫn đóng. Hệ 4 ngõ ra mất khoảng **18
giây**; hệ 8 slot stereo (16 kênh ngõ ra) mất khoảng **72 giây**. Hộp thoại xác
nhận in đúng con số của rig đang cắm trước khi bạn bấm. **Đừng bấm `ĐO` giữa lúc
MC đang nói.**

**Vòng hú qua các kênh ngõ ra KHÁC vẫn đóng** trong lúc đo — loa của chúng vẫn
đang phát tiếng mic. Vì vậy app **từ chối bắt đầu** khi chip `RING RISK` đã đọc
`RISING` trở lên, và vẫn giữ đủ bộ tự hủy trong lúc chạy. App đọc chip của **mọi
slot đang bật**, không riêng slot đang hiện trên màn hình — nên một phòng đang
ngân ở slot 1 vẫn chặn được lần đo, kể cả khi console đang xem slot 0.

**⚠️ Trong suốt phép đo, bộ chống hú TẮT trên MỌI slot** — không phải chỉ kênh
đang được quét. `ĐO` tắt detection trên cả dàn suốt cả lần chạy (hệ 16 kênh là
~72 giây), rồi mới trả lại theo mode đang chạy khi kết thúc. Hộp thoại xác nhận
in đúng câu đó trước khi bạn bấm. **Phòng bắt đầu hú giữa chừng thì bấm `DỪNG`
ngay** — trong lúc chạy đó là đường thoát.

**App đo gì.** So phổ mic thu được với phổ sweep đã phát, trừ nền nhiễu, ra
`H_dB` từng bin. Màn hình vẽ **`margin = −H_dB`** — biên còn lại trước khi bin đó
hú; `H_dB ≥ 0` nghĩa là bin đó **đã** hú được. **Băng tin cậy là 100 Hz – 6
kHz**: sweep đi tới 10 kHz nhưng một sweep log biên độ hằng bơm vào mỗi bin ở
10 kHz **ít hơn 20 dB** so với ở 100 Hz, nên 6–10 kHz được **vẽ mờ** kèm nhãn
"độ tin cậy thấp" và **không bao giờ được đề xuất**.

**App chỉ ĐỀ XUẤT — không đặt gì cả cho tới khi bạn bấm `ÁP DỤNG`.** `BỎ`, hoặc
để hết 20 giây, ném đề xuất đi và không đụng vào notch nào. Khi `ÁP DỤNG`:

- độ sâu rơi đúng bậc **−6 / −12 / −18 / −24 dB**, hoặc **chính trần preset** khi
  trần nông hơn (ví dụ `Music.json` trần −10), tối đa **6 bin mỗi làn**;
- độ sâu thật đặt xuống là **cái sâu hơn** giữa đề xuất và slider DEPTH đang để —
  kéo slider sâu hơn trong lúc xem kết quả thì lúc `ÁP DỤNG` app theo slider;
- bin đã có notch sống trong **±1 bin** — kể cả notch vừa do chính lượt này đặt —
  **bị bỏ qua**, không đè lên;
- chạy `ĐO` lại chỉ **dọn đúng những notch của lần `ÁP DỤNG` trước**: app giữ
  một sổ `(làn, index, tần số)` cho mỗi slot. Notch do mode `SOUNDCHECK` 15 giây
  đặt **không bị đụng tới**.

**Console bị khoá ở hai mức.** Trong lúc đo (`ĐANG ĐO`): khoá hết. Lúc đang xem
kết quả và sau khi `ÁP DỤNG`: chuyển mode (`AUTO`/`BYPASS`/`SOUNDCHECK`) và
`CLEAR ALL` **dùng được ngay** — luôn phải có đường thoát — còn `ĐO`, `PRESET
LOAD/SAVE`, định tuyến (LINK/width/kênh), tuning (Q/DEPTH) và hai nút GOOD/FALSE
vẫn **khoá**. **Bảng báo cáo sau `ÁP DỤNG` giữ console tới khi bạn bấm `BỎ`** —
không có đồng hồ đếm ngược ở bước này, vì kết quả "đã gỡ N notch cũ mà không đặt
lại được cái nào" là thứ không bao giờ được biến mất im lặng.

**Ba câu KHÔNG có nghĩa là "phòng sạch".** *"không đo được"* = mic không nghe đủ
rõ loa (SNR thấp). *"sai định tuyến"* = cặp kênh vào/ra của slot không hợp lệ với
device đang cắm. *"thiếu trần cắt"* = preset đang chạy không có trần để lượng tử
độ sâu. Cả ba hiện thành **câu riêng**, không thành một đường phẳng 0 dB — đường
phẳng trông hệt như "phòng rất tốt".

**Chín lý do app tự dừng**, mỗi lý do một câu riêng trên màn hình: bạn bấm `DỪNG`
· `Esc` · mic quá lớn · **phòng đã hú sẵn trước khi phát** · **không đo được nền
nhiễu** · mất dữ liệu mic · thiết bị hoặc định tuyến đổi giữa chừng · thiết bị
báo lỗi · thiết bị dừng. Hai lý do in đậm được quyết **trong 0,5 giây đo nền,
trước khi một mẫu sweep nào ra loa**: cổng nền so độ nhọn phổ với **đúng ngưỡng
peakiness detector đang dùng** (mặc định 10,0, đọc một lần lúc bắt đầu), giới hạn
trong băng 100 Hz – 6 kHz, và **hỏng theo hướng đóng** — không chấm được khung
nào thì dừng chứ không đoán là phòng sạch. `DỪNG` là đường dừng chính thức
(`Esc` chỉ là tiện lợi); từ lúc bấm tới lúc im là **≤ 31 ms ở buffer 64** và
**≤ 51 ms ở buffer 1024**, cộng độ trễ driver/amp, và ramp tắt do chính callback
audio sinh nên không có đường cắt phựt.

## Freeware

Theo quyết định chủ sở hữu **D-07** (23/08/2026): bản phát hành đầu là
**freeware**, không kích hoạt license. Phần mã license vẫn nằm trong source
(đã test đầy đủ 45 test) nhưng cố tình chưa được nối vào app — sẽ bật lại khi
có thông báo. Xem `.superpowers/sdd/2026-08-19-az-soundtech-hands-free/owner-decisions.md`.

## Muốn hiểu bên trong?

Xem [`docs/KY-THUAT-CHONG-HU.md`](KY-THUAT-CHONG-HU.md) — mô tả kỹ thuật đầy
đủ kèm sơ đồ: đường tín hiệu, thuật toán chấm điểm peakiness, mô hình
detector-so-author, và mọi biện pháp an toàn.
