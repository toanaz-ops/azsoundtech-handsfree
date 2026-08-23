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
| **Phát hiện tự động** | FFT 1024 điểm, chấm điểm "độ nhọn" của đỉnh phổ — phân biệt tiếng hú (một gai duy nhất nhô lên) với nhạc (nhiều đỉnh tự nhiên). Thời gian khóa mục tiêu trong ~1 giây kể từ khi hú đạt ngưỡng |
| **Notch siêu hẹp** | Tối đa 16 notch/kênh × stereo, Q tới 40, độ sâu cấu hình được (−6 đến −24 dB, mặc định −12 dB). Chỉ mất đúng vài Hz quanh tần số hú |
| **Tự nhả sau 30 giây** | Hết hú là filter tự nhả — không tích tụ vết cắt vô nghĩa suốt buổi show |
| **Chống báo nhầm harmonic** | Nếu đã khóa tần số F thì bậc harmonics 1.4F–4.1F bị trừ điểm, tránh cắt oan bội số của nốt nhạc |
| **3 chế độ** | **Bypass** (thông tuyến thuần), **Auto** (chạy liên tục), **Soundcheck** (nghe 15 giây đầu show, khóa mọi đỉnh tìm thấy, không tự nhả) |
| **Chọn device trực quan** | Chọn driver/device/sample rate/buffer ngay trong app; hiện latency và trạng thái theo thời gian thực |
| **Preset có sẵn** | `Speech` (Q=40, −18 dB — hà khắc cho loa hội thoại) và `Music` (Q=25, −10 dB — dịu cho nhạc sống); lưu/nạp preset JSON |
| **An toàn theo thiết kế** | Không cấp phát bộ nhớ hay khóa mutex trên đường audio real-time; từ chối độ sâu dương (điều gì sẽ xảy ra nếu một lỗi đánh dấu biến notch thành máy khuếch đại hú?); tự vô hiệu notch khi đổi sample rate khiến notch vượt Nyquist |

## Thông số nhanh

| | |
|---|---|
| Nền tảng | Windows 10/11 64-bit, standalone (không phải plugin) |
| Audio I/O | ASIO (khuyên dùng) hoặc Windows Audio/DirectSound dự phòng |
| Sample rate | 44.1 / 48 / 88.2 / 96 kHz |
| Kênh | Stereo vào / stereo ra |
| Độ trễ thêm vào | Vài ms tùy buffer size (app hiển thị số thật) |
| Điểm mù đã biết | Dưới ~234 Hz ở 48 kHz (~469 Hz ở 96 kHz) do cần 5 bin headroom hai bên cho phép chấm điểm — hú sub-bass vẫn phải xử lý bằng EQ tay |

## Cài đặt & sử dụng

Chạy installer `AZSoundtech-Handsfree-Setup-x.y.z.exe`, mở app, chọn device →
sample rate → buffer nhỏ nhất ổn định, bật **Soundcheck** 15 giây trước show để
"soi" sẵn phòng, rồi chuyển **Auto** và quên nó đi.

## Freeware

Theo quyết định chủ sở hữu **D-07** (23/08/2026): bản phát hành đầu là
**freeware**, không kích hoạt license. Phần mã license vẫn nằm trong source
(đã test đầy đủ 45 test) nhưng cố tình chưa được nối vào app — sẽ bật lại khi
có thông báo. Xem `.superpowers/sdd/2026-08-19-az-soundtech-hands-free/owner-decisions.md`.

## Muốn hiểu bên trong?

Xem [`docs/KY-THUAT-CHONG-HU.md`](KY-THUAT-CHONG-HU.md) — mô tả kỹ thuật đầy
đủ kèm sơ đồ: đường tín hiệu, thuật toán chấm điểm peakiness, mô hình
detector-so-author, và mọi biện pháp an toàn.
