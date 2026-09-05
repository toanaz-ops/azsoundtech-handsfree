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
                            tự nhả filter khi hết hú
```

App cắm vào đường tín hiệu như một thiết bị insert: lấy audio từ bất kỳ audio
interface ASIO nào, phân tích phổ bằng FFT trên một luồng nền, và khi thấy một
tần số đang "vươn lên" thành hú thì đặt ngay một bộ lọc notch cực hẹp đúng tần
số đó — chỉ khoét đúng chỗ hú, gần như không ảnh hưởng chất lượng nhạc.

## Tính năng chính

| | |
|---|---|
| **Phát hiện tự động** | FFT 2048 điểm, chấm điểm "độ nhọn" của đỉnh phổ — phân biệt tiếng hú (một gai duy nhất nhô lên) với nhạc (nhiều đỉnh tự nhiên). Thời gian khóa mục tiêu trong ~1 giây kể từ khi hú đạt ngưỡng |
| **Notch siêu hẹp** | 16 notch/làn, mỗi slot mono (1 làn) hoặc stereo (2 làn), 8 slot — tối đa 256 chuỗi notch. Q chỉnh được 8–50 (mặc định 30), độ sâu −6 đến −24 dB (mặc định −18 dB). Chỉ mất đúng vài Hz quanh tần số hú |
| **Routing 8 slot** | 8 slot xử lý độc lập, mỗi slot tự chọn kênh vào/ra bất kỳ của interface — không còn cố định stereo in/out. Mặc định chỉ slot 01 bật |
| **Stereo độc lập** | Mic hú qua loa trái thì chỉ cắt cánh trái; nút LINK mỗi slot để cắt cả hai bên như trước |
| **Chấm notch để app học** | Mỗi dòng ACTIVE NOTCHES có nút **GOOD** / **FALSE**. FALSE xóa notch ngay và ghi nhãn "cắt oan"; GOOD ghi nhãn "cắt đúng". Nhãn + phổ lúc quyết định vào log session trên máy (không audio) — dữ liệu cho bộ phân loại ở bản sau |
| **Tự nhả sau 30 giây** | Hết hú là filter tự nhả — không tích tụ vết cắt vô nghĩa suốt buổi show |
| **Chống báo nhầm harmonic** | Nếu đã khóa tần số F thì bậc harmonics 1.4F–4.1F bị trừ điểm, tránh cắt oan bội số của nốt nhạc |
| **3 chế độ** | **Bypass** (thông tuyến thuần), **Auto** (chạy liên tục), **Soundcheck** (nghe 15 giây đầu show, khóa mọi đỉnh tìm thấy, không tự nhả; hết 15 giây detector tự ngừng dò — bấm **Auto** để chạy tiếp) |
| **Chỉnh độ nhạy một nút** | ONE-KNOB RESPONSE: **SAFE** / **BALANCED** / **AGGRESSIVE** (tự chuyển **CUSTOM** khi chỉnh tay); mỗi slot chọn theo tuning **Global** chung hoặc **Custom** riêng |
| **Chọn device trực quan** | Chọn driver/device/sample rate/buffer ngay trong app; hiện latency và trạng thái theo thời gian thực |
| **Preset có sẵn** | `Speech` (Q=40, −18 dB — hà khắc cho loa hội thoại) và `Music` (Q=25, −10 dB — dịu cho nhạc sống) đúng giá trị trong repo. Installer chép hai preset vào máy, app tự seed chúng vào `%APPDATA%` lúc first-run (không bao giờ ghi đè file người dùng đã sửa), và GUI có hai nút **LOAD… / SAVE…** dưới mục INTERFACE để nạp/lưu preset (`*.json`). |
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
biết mỗi notch nằm ở làn nào. Chip **RING RISK** hiện luôn hiển thị "N/A" —
nguồn dữ liệu sẽ được nối ở bản sau.

## Chấm notch để app học

Thấy app cắt oan một nốt nhạc: bấm **FALSE** trên dòng đó — notch nhả ngay và app ghi
lại "đây là cắt oan". Thấy nó cắt đúng tiếng hú: bấm **GOOD**. Không bắt buộc, nhưng mỗi
lần bấm là một mẫu huấn luyện. Log nằm ở `%APPDATA%\AZSoundtech\HandsFree\logs\`, một
file mỗi lần mở app, không chứa audio; gửi file khi được hỏi.

## Freeware

Theo quyết định chủ sở hữu **D-07** (23/08/2026): bản phát hành đầu là
**freeware**, không kích hoạt license. Phần mã license vẫn nằm trong source
(đã test đầy đủ 45 test) nhưng cố tình chưa được nối vào app — sẽ bật lại khi
có thông báo. Xem `.superpowers/sdd/2026-08-19-az-soundtech-hands-free/owner-decisions.md`.

## Muốn hiểu bên trong?

Xem [`docs/KY-THUAT-CHONG-HU.md`](KY-THUAT-CHONG-HU.md) — mô tả kỹ thuật đầy
đủ kèm sơ đồ: đường tín hiệu, thuật toán chấm điểm peakiness, mô hình
detector-so-author, và mọi biện pháp an toàn.
