# AZ Soundtech Hands-free – Anti Feedback (v1)

**Tên sản phẩm:** AZ Soundtech Hands-free – Anti Feedback
**Phiên bản:** v1.0.0
**Ngày:** 2026-08-19
**Trạng thái:** Đã duyệt, sẵn sàng cho implementation plan

---

## 1. Tóm tắt (Overview)

Phần mềm standalone Windows giúp loại bỏ feedback (howling) trong live sound bằng cách tự phát hiện tần số feedback và áp dụng bộ lọc notch siêu hẹp lên tín hiệu âm thanh. Lấy audio qua bất kỳ audio interface ASIO nào (test với Audient iD14 MK2), hoạt động như một thiết bị insert trong daisy-chain USB giữa mixer (vd Behringer Wing) và PA/DAW. Sản phẩm thương mại, bán theo perpetual license.

## 2. Mục tiêu phi chức năng (Goals)

- **Latency bổ sung do phần mềm: gần 0.** Đường audio chỉ qua IIR biquad, cộng micro-giây. Tổng trễ thực tế = độ trễ USB driver của interface/console — không cải thiện, nhưng không thêm đáng kể.
- **Tỷ lệ false-positive (ăn nhầm nốt nhạc): thấp hơn Waves X-FDBK**, nhờ peakiness metric + harmonic-aware scoring.
- **Không rớt tiếng:** detector chạy nền, audio thread không block, không allocate, không lock dài.
- **Hoạt động liên tục nhiều giờ** không crash, không memory leak.
- **Cài đặt trên Windows 10/11 64-bit** bằng installer chuẩn (NSIS hoặc MSI).
- **Boot time < 3s**.

## 3. Không thuộc mục tiêu v1 (Out of scope)

- VST3/AU/AAX plugin (v2).
- macOS/Linux.
- >2 kênh I/O.
- Adaptive AFC (NLMS) — ghi nhận trong future plan, không làm v1.
- Multi-band envelope detector / ML scoring — future plan.
- Tích hợp sâu với Wing MIDI/HUI control.
- Cloud sync presets, account system, telemetry.
- Mobile app / remote control.

## 4. Kiến trúc (Architecture)

### 4.1 Tech stack

- **Ngôn ngữ/framework:** C++ với **JUCE** (chuẩn ngành audio plugin/desktop, hỗ trợ ASIO tốt, có sẵn FFT, audio buffer, GUI components).
- **Build:** CMake (JUCE hỗ trợ CMake từ v7+), CI build qua GitHub Actions — Windows MSVC, ASIO SDK include.
- **DSP:** `juce::dsp::FFT` cho detector, `juce::dsp::IIR::Filter` (biquad) cho notch chain; tự viết detection algorithm.
- **GUI:** JUCE native components (slider, button, spectrum visualizer dùng `juce::Component` + custom paint).
- **Audio device:** JUCE `AudioDeviceManager` với ASIO selected by default.
- **License key:** lưu trong file encrypted ở `%APPDATA%\AZSoundtech\HandsFree\license.key`, kèm online activation đơn giản qua HTTPS.
- **Distribution:** MSI hoặc NSIS installer.

### 4.2 Sơ đồ khối

```
                    ASIO Input (2ch)
                          │
                          ▼
                  ┌───────────────┐
                  │ Input buffer  │
                  └───────┬───────┘
                          │
                          ▼
              ┌────────────────────────┐         ┌──────────────────────────┐
              │  Notch chain (per ch)  │◄────────┤  Notch controller        │
              │  max 16 biquad/channel │  set    │  - state per notch       │
              │  sample-by-sample      │ params  │  - freq, depth, Q        │
              └───────────┬────────────┘         │  - locked_at, expire     │
                          │                     └──────────┬───────────────┘
                          │                                ▲
                          ▼                                │
                  ┌───────────────┐                        │
                  │ Output buffer │                        │
                  └───────┬───────┘                        │
                          │                                │
                          ▼                                │
                       ASIO Output                         │
                                                           │
                  ┌───────────────┐    ┌───────────────────┴────────────────┐
                  │ Tap ring buf  │───►│ Detector thread (background)       │
                  │ (1 ch mono    │     │  - FFT 1024 hop 50%                │
                  │  or L)        │     │  - Peakiness scoring                │
                  └───────────────┘     │  - Harmonic-aware                  │
                                      │  - Score threshold                  │
                                      │  - Emit set/clear notch cmd         │
                                      └─────────────────────────────────────┘
```

### 4.3 Hai thread, trách nhiệm rõ ràng

- **Audio thread (real-time):** chỉ đọc input, chạy notch chain, ghi output, ghi tap vào lock-free ring buffer. **Không allocate, không lock, không FFT ở đây.**
- **Detector thread (background, priority normal):** đọc ring buffer, chạy FFT, scoring, ra quyết định set/clear notch. Giao tiếp với audio thread qua **lock-free command queue** (SPSC ring).

### 4.4 Latency budget

| Thành phần                          | Trễ        |
|-------------------------------------|------------|
| ASIO buffer (64 samples @ 48 kHz)   | 1.33 ms    |
| ASIO buffer out                     | 1.33 ms    |
| Biquad chain (16 notch)             | ~0.05 ms   |
| Detector                            | không nằm đường tiếng |
| **Tổng thêm do app**                | **~2.7 ms**, cộng vào USB driver của interface |
| Wing USB driver (typical)           | ~5–8 ms    |
| **Tổng system**                     | **~8–11 ms** |

## 5. DSP chi tiết

### 5.1 Notch filter (biquad)

- Công thức RBJ Audio EQ Cookbook: notch filter, Q = 10–40, depth 6–24 dB.
- 16 biquad mỗi kênh = 16 notch tối đa mỗi kênh, độc lập L/R.
- Param mỗi notch: `freq (Hz)`, `depth (dB)`, `Q`, `state (idle|locked)`, `locked_at (time)`.
- Tính lại coefficient khi set/clear, không áp dụng khi idle (bypass).

### 5.2 Detector pipeline (trong detector thread)

1. Đọc block 1024 mẫu từ ring buffer (overlap 50% → hop 512 mẫu @ 48 kHz = 10.67 ms).
2. Áp dụng Hann window.
3. FFT 1024 → magnitude spectrum 513 bins.
4. Với mỗi bin ≥ 100 Hz: tính
   - **Magnitude** so với running average (RMS theo dõi ~3 giây).
   - **Peakiness** = `bin_mag / mean(neighbors tại offset ±3, ±4, ±5)` — annulus 6 bin, **loại trừ main lobe** (offset 0, ±1, ±2). Feedback = peakiness > threshold (10×, xem plan Task 11).
     > **Đính chính (Task 11).** Công thức cũ `mean(neighbors ±2 bins)` sai với pipeline này: bước 2 áp Hann window nên main lobe rộng **4 bin** (bin ±1 giữ ~0.50 biên độ đỉnh, bin ±2 chỉ 0.0003), tức vùng ±2 đo tone với chính nó và peakiness bị chặn trên ở **4.0**. Đo thực tế ở radius cũ: tone đúng bin 3.99, tone 1 kHz 3.29, nhiễu broadband tới 3.46 — nhiễu còn cao hơn tone, threshold 10.0 không bao giờ kích hoạt được. Với annulus ±3..±5: tone 1 kHz = **131.7**, nhiễu tệ nhất qua 60 seed = **7.35**, 0 false positive. Threshold giữ nguyên 10.0 — lỗi nằm ở radius, không phải ở hằng số.
     > **Giới hạn v1.** Annulus cần đủ 5 bin ở cả hai phía nên bin thấp nhất chấm được là bin 5 = **234 Hz @ 48 kHz**; ngưỡng “≥ 100 Hz” ở trên **không đạt được** và detector mù dưới ~234 Hz (tỉ lệ theo sample rate: ~215 Hz @ 44.1 kHz, ~469 Hz @ 96 kHz). Feedback low-mid 200–250 Hz là có thật — xem `src/dsp/PeakinessAnalyzer.h`.
   - **Rise rate** = `mag_now / mag_500ms_ago`. Feedback = rise > 1.5× / 200 ms.
   - **Harmonic test:** nếu freq này là bội số (1.5–4×) của một đỉnh locked khác → giảm score.
5. **Score** = weighted sum, threshold (vd > 0.7) → candidate.
6. Nếu candidate kéo dài ≥ 3 block liên tiếp (~30 ms) → **set notch**.
7. Auto-release: notch nào không còn peakiness > threshold trong 30 giây → clear (chuyển sang idle).

### 5.3 Soundcheck mode

- Khi user nhấn "Run Soundcheck": detector chạy auto trong 15 giây, tất cả notch đặt ra ở state `locked`, không auto-release cho đến khi user nhấn "Clear".
- Sau soundcheck, chế độ Auto continuous tiếp tục: chỉ set thêm notch mới, không auto-release notch locked từ soundcheck.

### 5.4 Per-channel L/R

- Mặc định **xử lý L và R độc lập** (mỗi kênh 16 notch riêng).
- Toggle "Link L/R": nếu bật, notch đặt ở L sẽ mirror sang R cùng freq/depth/Q (giảm notch tổng còn 16, dùng cho linked stereo source).

## 6. GUI

### 6.1 Layout đơn giản, 1 cửa sổ chính

```
┌─────────────────────────────────────────────────────────────┐
│  AZ Soundtech Hands-free                            [─ □ ×] │
├─────────────────────────────────────────────────────────────┤
│  Audio Device: [ASIO: Audient iD14 MK2 ASIO Driver ▼]      │
│  Sample Rate:  [48000 ▼]    Buffer: [64 samples ▼]          │
│  Status: ● 48 kHz / 2 in / 2 out / Latency 5.3 ms          │
├─────────────────────────────────────────────────────────────┤
│  Channel:  [ L ] [ R ]   [Link L/R ☐]                       │
│  Mode:     [Soundcheck] [Auto] [Bypass]                    │
│                                                             │
│  Spectrum + Notch overlay (real-time):                      │
│   - Spectrum analyzer (đường liền)                          │
│   - Notch locked hiển thị ▼ marker tại từng freq,           │
│     cao độ thể hiện depth                                   │
│   - Notch mới detect (vài giây gần đây) hiển thị khác màu   │
│                                                             │
│  Notch list (click để xem chi tiết từng notch):             │
│   ┌────┬───────┬────────┬────┬─────────────────┐            │
│   │ #  │ Freq  │ Depth  │ Q  │ Status          │            │
│   │ 1  │ 482Hz │ -12dB  │ 30 │ Locked (2m ago) │            │
│   │ 2  │ 1.2kHz│ -8dB   │ 25 │ Auto (just)     │            │
│   │ ... │       │        │    │                 │            │
│   └────┴───────┴────────┴────┴─────────────────┘            │
│                                                             │
│  [Run Soundcheck (15s)]  [Clear All]  [Save Preset ▼]       │
├─────────────────────────────────────────────────────────────┤
│  License: Activated ✓  [Deactivate]                         │
└─────────────────────────────────────────────────────────────┘
```

### 6.2 Nguyên tắc UX

- Một màn hình, không menu ẩn — soundman không có thời gian click chuột giữa show.
- Mọi thông số quan trọng (freq, depth, Q, locked time) hiển thị trực tiếp.
- Visual feedback realtime (spectrum + notch) để soundman thấy app đang hoạt động.

## 7. Cấu hình & Preset

- Preset = danh sách notch đã lock + audio device + buffer size. Lưu JSON ở `%APPDATA%\AZSoundtech\HandsFree\presets\`.
- Có sẵn 2 preset mặc định: "Speech", "Music".
- Không cloud sync (v1).

## 8. Lỗi & Edge cases

- ASIO device bị ngắt giữa show: app hiển thị banner "Audio device disconnected", audio mute, không crash.
- Sample rate thay đổi: clear notch, re-init.
- CPU overload: hiển thị warning nếu audio thread > 70% (dù JUCE có reporting).
- License hết hạn / chưa activate: hiển thị dialog ngay khi mở app, không vào main UI cho đến khi activate (offline grace 7 ngày).
- Không tìm thấy ASIO device: hiển thị hướng dẫn cài driver.

## 9. Testing & QA

### 9.1 Unit test

- Biquad coefficient correctness (known input → known output).
- FFT correctness.
- Detector scoring với synthetic signals (tone + noise → expect detection/no detection).
- Lock-free ring buffer không drop sample khi load cao.

### 9.2 Integration test

- Loopback trên Audient iD14 MK2: cáp analog out → in, đo latency round-trip bằng impulse.
- Test với file âm thanh thực: nhạc + simulated feedback (tone bơm lên), đếm số notch đặt đúng + false positive.
- Test với nhiều thể loại: vocal, brass, guitar, full band, classical.

### 9.3 Field test

- Test với mixer thật (Wing hoặc tương đương) trong phòng thu trước, sau đó 1 show thật với backup plan.
- Người dùng mục tiêu: soundman quen — feedback sau 5 phút sử dụng.

## 10. Phát hành & Pháp lý

### 10.1 Installer

- Windows MSI hoặc NSIS, đặt tại `AZSoundtech-Handsfree-Setup-1.0.0.exe`.
- Code signing bằng EV code signing certificate ($300–500/năm, dùng để SmartScreen không cảnh báo).

### 10.2 License

- License key format: `AZHF-XXXX-XXXX-XXXX-XXXX`.
- Online activation qua HTTPS tới `https://license.azsoundtech.com/activate` (POST key + machine hash → JWT response).
- Offline grace: 7 ngày sau khi mất kết nối.

### 10.3 Website bán hàng

- Trang đơn giản: giới thiệu, demo video, download trial (14 ngày), mua ($59–79 tùy chọn pricing).

### 10.4 Giá (đề xuất)

- $59 standard (early bird), $79 sau 6 tháng.
- Trial 14 ngày full feature.

## 11. Cấu trúc file dự kiến

```
handsfree/
├── CMakeLists.txt
├── README.md
├── docs/
│   └── superpowers/
│       └── specs/
│           └── 2026-08-19-az-soundtech-hands-free-design.md   ← file này
├── src/
│   ├── main.cpp
│   ├── app/
│   │   ├── MainComponent.{h,cpp}
│   │   ├── AudioEngine.{h,cpp}        ← ASIO I/O + audio thread + notch chain
│   │   ├── LicenseManager.{h,cpp}
│   │   └── PresetManager.{h,cpp}
│   ├── dsp/
│   │   ├── NotchChain.{h,cpp}         ← biquad chain per channel
│   │   ├── Biquad.{h,cpp}             ← RBJ biquad impl
│   │   ├── Detector.{h,cpp}           ← FFT + scoring + harmonic-aware
│   │   └── LockFreeRingBuffer.{h,cpp}
│   ├── gui/
│   │   ├── SpectrumView.{h,cpp}
│   │   ├── NotchListView.{h,cpp}
│   │   └── DeviceSelector.{h,cpp}
│   └── util/
│       └── Logger.{h,cpp}
├── tests/
│   ├── test_biquad.cpp
│   ├── test_detector.cpp
│   └── test_ringbuffer.cpp
├── installer/
│   └── handsfree.nsi
└── .github/
    └── workflows/
        └── build.yml
```

## 12. Timeline (ước tính, 1 dev fulltime)

- Tuần 1–2: Project skeleton JUCE + ASIO passthrough + GUI placeholder.
- Tuần 3–4: Biquad chain + lock-free ring buffer + threading.
- Tuần 5–6: Detector FFT + scoring + harmonic-aware.
- Tuần 7: Mode (Soundcheck + Auto + Bypass), GUI polish.
- Tuần 8: License manager + installer.
- Tuần 9–10: Testing với iD14 MK2 + Wing + nhiều thể loại nhạc.
- Tuần 11–12: Tuning detector (giảm false-positive), tạo demo video, landing page.
- **Ship v1 cuối tuần 12.**

## 13. Tiêu chí "v1 ship được"

1. Cài được trên Windows 10/11, chạy với Audient iD14 MK2 và Behringer Wing.
2. Audio passthrough không rớt mẫu nào trong 8 giờ liên tục (test loopback).
3. Auto-detect đặt notch trong vòng 1 giây từ lúc feedback bắt đầu.
4. False-positive < 5% trên bộ test 10 bản nhạc đa dạng thể loại.
5. License activate/deactivate hoạt động đúng.
6. Installer chạy sạch, không cần dependency ngoài ASIO driver đã có sẵn.

## 14. Future plan (ghi nhận, không làm v1)

- **Multi-band envelope + ML-ish scoring** (Section 3 nói trên).
- VST3 plugin format.
- macOS port.
- Multi-channel (4, 8, 16+).
- Adaptive AFC (NLMS) hybrid.
- Cloud preset sync.
- Remote control qua phone (WiFi).
- Tích hợp sâu mixer console (MIDI/HUI control, OSC).
- Wing companion: app nhỏ hiển thị notch state trên tablet của FOH.