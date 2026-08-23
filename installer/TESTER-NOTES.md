# Ghi chú cho team test — AZ Soundtech Hands-free v1.0.0

Ngày build: 2026-08-23 · từ main @ `826bf81` · suite test 233/233

## Hai gói, chọn một

| File | Dùng khi | Ghi chú |
|---|---|---|
| `AZSoundtech-Handsfree-Setup-1.0.0.exe` (26.5 MB) | Muốn cài chuẩn: Start Menu, uninstall trong Apps & Features | **Tự cài VC++ Runtime** nếu máy chưa có. Khuyên dùng. |
| `AZSoundtech-Handsfree-Portable-1.0.0.zip` (3.2 MB) | Chạy không cần cài | Chỉ có exe. Nếu mở lên báo thiếu `VCRUNTIME140.dll` → cài [VC++ x64 redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe) trước |

SHA-256 của Setup:
`1CF2EF9D70C1B0375B245B1C95911871588CB43853A8EA511DBA1CE62A76279D`

## Ba điều biết trước để không tưởng app hỏng

1. **SmartScreen chặn lần đầu** — file chưa ký số (chưa mua chứng chỉ).
   Bấm "More info" → "Run anyway". Đây là hành vi đúng của bản unsigned,
   không phải virus.
2. **Chưa kèm driver ASIO** — ràng buộc licence Steinberg. Không có ASIO thì
   app chạy WASAPI (latency cao hơn) — vẫn test được UI/logic, chỉ đừng đo
   latency thật.
3. **Đây là bản dev**: GUI còn thô sơ mặc định (panel thiết bị + mode +
   status). Spectrum/marker/bảng notch là phase kế tiếp.

## Kịch bản test đề nghị (10 phút)

1. Cài (hoặc giải nén) → mở app ở **âm lượng nhỏ nhất**.
2. Chọn device → Status bar hiện đúng sample rate / buffer / latency.
3. Đổi mode Auto → Bypass → Auto, nghe passthrough trong suốt.
4. Đổi sample rate / buffer size trong panel → app không sập, combo hiển thị
   giá trị THẬT thiết bị chạy lại.
5. Rút cáp input giữa chừng → app không crash, status báo lỗi thiết bị thay vì im lặng.
6. Uninstall (bản Setup) → thư mục + registry key biến mất sạch.

Báo lỗi kèm: Windows version, card tiếng, bước nào, hiện tượng gì, screenshot.
