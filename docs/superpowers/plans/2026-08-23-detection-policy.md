# Detection Policy Implementation Plan (Tasks 12 + 15 — DSP spine còn lại)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development.
> Checkbox steps. Written 2026-08-23 sau khi bridge landed (`bff5eaf`, suite 219/219).

**Goal:** Detector thật sự QUYẾT đặt/gỡ notch: chấm điểm candidate theo spec §5.2,
đặt notch sau 3 block liên tiếp, nuôi auto-release; cộng thêm Soundcheck mode §5.3.

**Architecture:** Toàn bộ policy chạy trên detector thread bên trong
`NotchController::runOnce()` — nó đã sở hữu model, clock, outbox. Scoring thuần
tách thành `src/dsp/CandidateScorer` để test không cần controller. Không đổi gì
trên audio thread.

**Spec nguồn:** `docs/superpowers/specs/2026-08-19-az-soundtech-hands-free-design.md`
§5.2 (pipeline 7 bước), §5.3 (soundcheck); tham số đo đạc trong
`src/dsp/PeakinessAnalyzer.h` (không được đổi threshold/radius nếu không chạy lại
sweep 60 seed).

## Global Constraints

- Giữ nguyên mọi hằng số của `PeakinessAnalyzer` (threshold 10.0, annulus ±3..±5).
- Detector thread: không allocation sau khi start (pre-allocated buffers);
  KHÔNG phải audio thread nên mutex/vector chấp nhận được ngoài hot loop, nhưng
  hot loop (mỗi block ~10.67 ms) tránh cấp phát lặp.
- Mỗi quyết định thiết kế mới phải ghi lý do vào plan/design, kèm phương án loại.
- Build/test như cũ: VS 18 2026, Release, `ctest -C Release`. Full suite phải
  luôn xanh sau mỗi task.
- Audio-path chưa đổi — tới khi nào lệnh Set thực sự phát ra từ thuật toán rồi
  mới đánh dấu "needs a human listen" lần nữa.

## Quyết định thiết kế (engineering choices, đã quyết có lý do)

| # | Quyết định | Lý do / phương án loại |
|---|---|---|
| KD-1 | Baseline magnitude = **EMA per-bin**, alpha suy từ dt thật (hòa tan ~3 s) | Spec yêu cầu "running average ~3 giây". Ring buffer đầy đủ lịch sử tốn 513×N mẫu mà không thêm thông tin |
| KD-2 | Rise-rate = so `mag_now` với bản copy cách **~500 ms** (ring 47 frame × 513 float ≈ 97 KB, detector thread) | Spec: rise > 1.5× / 200 ms. Dùng đúng 500 ms đơn giản hơn nội suy; ngưỡng giữ 1.5 |
| KD-3 | Harmonic penalty ×0.5 khi candidate freq ∈ (1.4×..4.1×) freq của notch LOCKED | Đúng công thức plan Task 12; spec nói 1.5–4× — lấy khoảng rộng hơn của plan vì an toàn hơn khi bỏ sót hài |
| KD-4 | Score tổng = peakiness-normalised × rise-factor × harmonic-penalty, candidate khi > 0.7 | Spec §5.2 step 5 "weighted sum, threshold vd > 0.7". Chuẩn hoá từng trục về [0,1] trước nhân |
| KD-5 | Depth mặc định −12 dB, Q = 30 cho notch tự động | Trùng `PresetNotchDefaults` (`PresetManager.h:111-112`); spec chỉ khoảng 6–24 dB / Q 10–40. Mapping magnitude→depth là việc sau, cần đo thật |
| KD-6 | Slot: slot `active == false` đầu tiên của kênh đó; đầy thì bỏ qua candidate | Đơn giản, FIFO tự nhiên; không đánh chiếm slot notch preset/manual |
| KD-7 | `Origin::Soundcheck` thêm vào enum; **miễn trừ auto-release** | §5.3: notch soundcheck khóa tới khi Clear. Khác Preset (D-05 cho phép auto-release) |
| KD-8 | Soundcheck = 15 s trên `liveMs_`; hết giờ tự coi như Auto | §5.3 "chạy auto trong 15 giây". GUI đọc `getSoundcheckRemainingMs()` để hiển thị |
| KD-9 | Chỉ phát hiện khi `detectionActive_` (Auto/Soundcheck); snapshot vẫn xuất bản mọi lúc | Bypass không được đặt notch; spectrum GUI vẫn cần sống |

---

### Task A: CandidateScorer (thuần DSP, không thread)

**Files:** Create `src/dsp/CandidateScorer.h/.cpp`; Test `tests/test_candidatescorer.cpp`;
sửa 2 CMakeLists.

**Interfaces produced:** 
```cpp
class CandidateScorer {
public:
    static constexpr double kBaselineTimeConstantMs = 3000.0;  // KD-1
    static constexpr double kRiseReferenceMs        = 500.0;   // KD-2
    static constexpr double kRiseThreshold          = 1.5;     // spec 5.2 step 4
    static constexpr float  kHarmonicPenalty        = 0.5f;    // KD-3
    static constexpr float  kConfirmScore           = 0.7f;    // KD-4

    void beginBlock (double sampleRate, double dtMs);
    // Trả về điểm đã điều chỉnh cho một candidate (PeakinessAnalyzer::Candidate).
    // side effects: cập nhật EMA baseline + rise-history ring.
    float scoreCandidate (const PeakinessAnalyzer::Candidate& c,
                          const float* magnitudes,
                          const double* lockedFrequencies, std::size_t lockedCount);
};
```

Test bắt buộc (TDD, mỗi cái một behaviour):
1. Tone ổn định lâu dài → baseline bắt kịp → score giảm dần về thấp (không tự đặt notch trên tiếng hú đã cắt).
2. Tone mới nổi đột ngột sau nền yên tĩnh → rise factor cao → score > 0.7.
3. Candidate là hài của locked 500 Hz (ví dụ 1500 Hz ∈ 1.4–4.1×... dùng 2000 Hz = 4× nằm TRONG khoảng; 1000 Hz = 2× cũng vậy) → score bị nhân 0.5 so với trường hợp không locked.
4. Im lặng hoàn toàn → không candidate nào vượt 0.7 (60 seed sweep kiểu test_peakiness nếu khả thi; ít nhất 10 seed).

### Task B: Policy loop trong NotchController

**Files:** Modify `src/app/NotchController.h/.cpp`; Test `tests/test_notchcontroller.cpp`.

- `runOnce()` bước 1½: khi `detectionActive_` → `analyzer_.analyse(block)` →
  scorer → candidate > 0.7 **3 block liên tiếp cùng bin (±0)** → `setNotch(channel, slot KD-6, freq, 30, −12, Origin::Detector)`; persistence counter per bin, reset khi mất candidate.
- Với notch đang active (không phải Soundcheck): bin gần freq nhất có
  `peakinessAt > threshold` → refresh `lastDetectedMs = liveMs_`.
- Plumbing: `void setDetectionActive (bool)` (atomic), `Origin::Soundcheck`,
  miễn trừ auto-release cho Soundcheck, `startSoundcheck()` /
  `double getSoundcheckRemainingMs() const` (15 s trên `liveMs_`).

Test bắt buộc: fake spectrum qua tap (sin tone đúng bin như test_peakiness làm)
→ 3 block → đúng 2 lệnh Set (2 kênh)... **Lưu ý**: detection per-channel hay
mono? Tap hiện chỉ là post-notch LEFT → **v1 detect trên tap mono-L, đặt notch
cả 2 kênh như preset adoption** (KD ghi ở đây: tap R chưa tồn tại; mở rộng
per-channel là việc sau). → 2 lệnh Set (ch0, ch1), origin Detector; tone tắt →
35 s live → 2 lệnh Clear; Soundcheck-origin không bao giờ Clear; detection tắt
(detectionActive_ false) → không có lệnh nào dù tone gào.

### Task C: Verify + closeout

Full reconfigure + build + `ctest`; cập nhật progress.md/memory/handoff;
đánh dấu "needs a human listen" (lần này thuật toán thật sự điều khiển notch).

**Ngoài scope:** Link L/R toggle (§5.4), mapping magnitude→depth, tap stereo,
GUI hiển thị soundcheck countdown.
