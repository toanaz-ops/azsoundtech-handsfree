# Review độc lập toàn repo — `origin/main` @ `f2cc11e` (2026-10-20)

Reviewer đọc-chỉ, không sửa gì trong repo. Worktree tách riêng tại scratchpad,
build Linux/clang (`-DJUCE_USE_CURL=0`, ASIO SDK vắng — bình thường). Kết quả
build/ctest ở mục 6.

Phạm vi theo yêu cầu: (1) an toàn real-time đường audio, (2) thread/lifetime,
(3) đúng spec lane S/D/G/R/M/U + persistence, (4) kiến trúc/CI/test/docs drift,
(5) `tools/`. Mỗi finding dưới đây đều đã lần theo một đường cụ thể
(input/state → hành vi sai) trước khi ghi; những nghi ngờ không lần được thì bỏ.

## 0. Tổng kết

| Mức | Số | Tóm tắt |
|---|---|---|
| BLOCKER | 1 | Detector của `NotchController` chạy cứng 48 kHz: ở 44.1/88.2/96 kHz **mọi notch tự động đặt sai tần số** (lệch 8,8 % hoặc gấp đôi/một nửa), hú không bị cắt, chain lấp đầy notch cắt nhầm tone |
| IMPORTANT | 4 | (I1) restart do **driver** khởi xướng (ASIO reset / đổi SR trên card) đi vòng qua §6.5 → `clear()` ring đua với consumer, lệnh Set/Clear bị mất, model ≠ chain; (I2) Bypass **ghi đè** thay vì **cộng** khi hai slot chung ngõ ra → một mic câm trong Bypass, +6 dB khi chuyển sang AUTO; (I3) `CandidateScorer` gate bằng hằng `kDefaultThreshold=10` chứ không phải ngưỡng sống → nửa dưới slider PEAKINESS và preset AGGRESSIVE (8.0) **chết**; (I4) `docs/GIOI-THIEU.md` hứa routing 8 slot "kênh bất kỳ của interface" trong khi engine chỉ mở 2 in/2 out |
| MINOR | 10 | Q của preset không kẹp (Q=0,01 → cắt −24 dB băng rộng); memory note lane R ghi sai hệ quả của `setSampleRate`; test wiring tự gọi `setSampleRate(48000)` nên che BLOCKER; thiếu test Bypass-cộng, test ngưỡng <10, test guard "dsp không include audio_devices"; CI không có sanitizer; NaN-heal strand ramp (đã ghi nhận); comment stale; `tools/afc` biên dịch vào test binary; `JUCE_USE_CURL=0` không tới test/tool (Linux link fail); header `KY-THUAT` ghi "v1.2.0" |

Những gì **đúng và đáng giữ** (đã kiểm): callback audio không cấp phát, không
khoá, FTZ/DAZ bật đầu callback, input NaN→0, output NaN→0 + kẹp ±1.0 **sau**
điểm bơm sweep, `Biquad` từ chối freq≥Nyquist/Q≤0/depth>0, ramp độ sâu là tổ
hợp lồi không bao giờ khuếch đại, `NotchChain::setSampleRate` vô hiệu notch
vượt Nyquist thay vì kẹp, lệnh qua ring được bounds-check đủ 3 trường, thứ tự
hủy `MainComponent` (soundcheck → detector → logger → sink=null → engine) đúng,
`SessionLogger::stop` đóng cửa dưới cùng một mutex với `log()`, preset parse 2
pha từ chối non-finite/bool-giả-số, `HostSettings` ghi atomic + quarantine
`.bad` + không ghi đè schema mới hơn.

---

## 1. BLOCKER

### B1. Detector phân tích ở 48 kHz bất kể device — notch tự động đặt sai tần số ở mọi rate khác 48 k

**Ở đâu.**
- `src/app/NotchController.h:626` — `Detector detector { 48000.0 };` trong `LaneAnalysis`.
- `src/app/NotchController.cpp:1559-1580` — `NotchController::setSampleRate` là đường duy nhất đổi rate đó.
- Không có caller production: `grep` toàn `src/` chỉ thấy `NotchChain::setSampleRate` (engine, đúng) và `Detector::setSampleRate` (chỉ từ `NotchController::setSampleRate`). Caller của `NotchController::setSampleRate` chỉ có `tests/` và `tools/snapshot.cpp:238`. `MainComponent::onAfterRestart` (`MainComponent.cpp:630-640`) và `startAudio` (`:1251-1256`) chỉ gọi `setWidth()` rồi `start()`. Chính code tự thú nhận: `NotchController.cpp:112-113` "*even though nothing outside tests calls setSampleRate()*", `:1573` "*this has no production caller today*".

**Đường sai.**
1. Device mở ở 44.1 kHz. `AudioEngine::audioDeviceAboutToStart` (`AudioEngine.cpp:1052-1066`) retarget 16 `NotchChain` về 44 100 — **đúng**.
2. `Detector::processLatestBlock` (`Detector.cpp:96`) gắn `Spectrum::sampleRate = 48000` (giá trị khởi tạo, chưa ai đổi).
3. `PeakinessAnalyzer::analyse` (`PeakinessAnalyzer.cpp:144`): `frequencyHz = bin * 48000 / 2048`. Hú thật ở 1 000 Hz rơi vào bin `round(1000·2048/44100) = 46` → báo **1 078 Hz**.
4. `placeConfirmed` → `setNotchImpl` → `NotchCommand{freq=1078}` → chain ở 44.1 kHz đặt notch tại **1 078 Hz thật** (Q=30 ⇒ rộng ~36 Hz). Hú 1 000 Hz **không nằm trong notch**.
5. Vòng reinforce (`NotchController.cpp:1325`) tính `bin = lround(1078 / (48000/2048)) = 46` — cùng bin sai — nên model thấy "còn hú" → thang G đào tới −24 dB **tại tần số không hú** (mất tone), rồi bin thật vẫn hú → đặt tiếp index kế (cùng sai) cho tới khi 16 slot đầy. Ở 96 kHz tệ hơn: hú 1 000 Hz → bin 21 → báo **492 Hz** (nửa tần số).

**Hệ quả phụ (cùng gốc).**
- `SpectrumView` vẽ trục Hz bằng `snapshot_.sampleRate` (`SpectrumView.cpp:809`) → thang tần số sai, soundman không thể đối chiếu với tai.
- `savePreset` (`MainComponent.cpp:2586-2587`) ghi `sampleRate: 48000` vào file dù device 44.1 k; `validate` so Nyquist với rate giả.
- Lane M: `applySoundcheckResults` → `withinOneBinOfALiveNotch` (`SoundcheckController.cpp:1280`) quy đổi `cand.hz` (tính ở rate **thật**, `params_.sampleRate`) sang bin bằng `snap.sampleRate` (**48 k**) → phép thử invariant 15 (±1 bin quanh notch sống) lệch theo tỉ số rate. Comment ở `:1270-1271` "*validates the frequency against the detector's real rate*" là sai.
- `memory/ring-risk-lane-r-2026-09-06.md §6` đã thấy "không có caller production" từ 06/09 nhưng kết luận "`setWidth` tình cờ reset đúng phần state cần reset" — chỉ đúng cho readout RING RISK, **sai** cho bin→Hz. Lỗi này sống qua 1.1.3 → 1.4.0 vì mọi test wiring tự gọi `controller0->setSampleRate(48000.0)` (`tests/test_gui_wiring.cpp:1028, 1109, …`, `tools/snapshot.cpp:238`) và không test nào dựng `MainComponent` + engine ở rate ≠ 48 k rồi đo tần số notch.

**Vì sao BLOCKER.** Sản phẩm có đúng một việc — cắt hú — và ở mọi rate khác 48 kHz nó không làm được việc đó trong khi màn hình báo "đang chặn N điểm"; đồng thời nó cắt −24 dB vào những tần số đang có nhạc. 48 kHz phổ biến trong live, nhưng card 44.1 k (và 96 k cho người thích "hi-res") có thật trên rig alpha. Không có stack trace, không có test đỏ.

**Sửa đề xuất.** Trong `MainComponent::onAfterRestart` và `startAudio`, ngay cạnh `setWidth()` (thread đang dừng — precondition giống nhau):
```cpp
controller.setSampleRate (engine_.getCurrentSampleRateHz());
controller.setWidth (...);
controller.start();
```
Thêm `jassert`/refuse nếu rate ≤ 0 (engine trả 48000 mặc định trước khi device mở — chấp nhận). Thêm test wiring: dựng engine giả ở 44 100, bơm tone 1 000 Hz qua tap, assert `NotchCommand.frequency` ∈ [1000 ± binWidth/2]; và một test `SnapshotBuffer::sampleRate == engine rate`. Sửa luôn comment `SoundcheckController.cpp:1270` và memory §6.

**Mức thay đổi dự kiến.** Ở 48 kHz: **0 dB, bit-identical** (rate giống giá trị khởi tạo). Ở 44.1/96 kHz: notch dời về đúng bin hú → bin hú giảm theo thang G như thiết kế (−6 → trần), còn các tần số trước đây bị cắt nhầm **trở lại 0 dB** (tăng mức tại đó tới +24 dB so với bản lỗi — là tone bị mất quay về, không phải hú). Nghe ở âm lượng thấp trên card 44.1 k trước khi tin.

---

## 2. IMPORTANT

### I1. Restart do driver khởi xướng đi vòng qua §6.5: `clear()` ring đua với consumer, lệnh notch bị mất

**Ở đâu.** `src/app/AudioEngine.cpp:1044-1134` (`audioDeviceAboutToStart`): `tap.clear()` ×16, `micCapture_.clear()`, `queue.clear()` ×8, reset 6 atomic soundcheck. Precondition ghi ở `LockFreeRingBuffer.h:131-140`: *không producer, không consumer đang chạy*. Precondition đó chỉ được bảo đảm khi restart đi qua `DevicePanel::onBeforeRestart/onAfterRestart` (`MainComponent.cpp:573-662`), tức restart **do app** khởi xướng.

**Đường sai.** JUCE ASIO (`external/JUCE/modules/juce_audio_devices/native/juce_ASIO_windows.cpp`):
- `:1389-1391` `kAsioBufferSizeChange / kAsioResetRequest / kAsioResyncRequest → resetRequest()`; `:1429-1432` `sampleRateChangedCallback → resetRequest()`.
- `:718-739` `resetRequest()` = `startTimer(500)`; `timerCallback()` làm `close(); open(...); start(oldCallback)` → `CallbackHandler::audioDeviceAboutToStart` → `AudioDeviceManager::audioDeviceAboutToStartInt` (`juce_AudioDeviceManager.cpp:1128`) → **`AudioEngine::audioDeviceAboutToStart`** trên message thread, **không ai gọi hook**; 8 detector thread + lane M thread vẫn đang chạy.
Kịch bản thật trên rig: soundman mở control panel của card đổi buffer, hoặc card đổi clock/sample rate (word clock, ADAT) → driver reset.

Hệ quả cụ thể:
1. `commandQueues_[s].clear()` chạy sau khi `NotchController::flushOutbox` (`NotchController.cpp:1592`) đã `write()` thành công → lệnh **Set** mất: model `active=true`, chain không có filter → GUI/log báo "đang chặn" mà không chặn. Lệnh **Clear** mất: chain giữ filter, model đã `active=false` → filter chạy "vô chủ" cho tới khi index được tái dùng (auto-release không bao giờ gửi lại Clear cho slot inactive). Lời hứa ở `NotchController.cpp:1596` "*the model cannot diverge from the chain*" không còn đúng.
2. `tap.clear()` chạy giữa một `read()` của `Detector::processLatestBlock` (`Detector.cpp:67`): consumer store `readPos_ = readIndex + toRead` (giá trị cũ) **sau** `clear()` → `readPos_` lùi lại → block kế đọc lại audio **cũ, ở rate cũ** mà `clear()` tồn tại để loại bỏ. Không out-of-bounds (chỉ số đơn điệu, modulo), nhưng là chính cái splice spec cấm.
3. Lane M: 6 atomic soundcheck về idle giữa lượt đo trong khi `SoundcheckController` vẫn ở `Sweep`; `checkDeviceUnchanged()` chỉ bắt được khi **rate/số kênh đổi**; reset cùng rate (đổi buffer) → capture tắt, lượt đo ra "không đo được" (`NoiseFloorUnmeasured`/không frame) — an toàn nhưng sai, và `micCapture_.clear()` đua với `drainCapture()` như mục 2.
4. `setWidth()` (A-R4 reset `blocksSinceReset`, room memory) không chạy → chip RING RISK và "nhớ phòng" dùng dữ liệu rate cũ — đúng cái lane R ghi là lỗi.

**Sửa đề xuất.** `AudioEngine` phơi hai hook `std::function<void()> onDeviceAboutToStart, onDeviceStarted` (gọi ở đầu và cuối `audioDeviceAboutToStart`, trên thread JUCE gọi — ASIO reset là message thread qua `Timer`; với CoreAudio có thể là thread khác, nên hook phải tự `MessageManager::existsAndIsCurrentThread()`-check hoặc chỉ join). `MainComponent` nối `onDeviceAboutToStart = onBeforeRestart` (join detector + `abortAndJoin` lane M, như hôm nay) và `onDeviceStarted = onAfterRestart` (B1 fix + `setWidth` + `start`). Hook đã idempotent (`stop()` trên thread đã dừng, `start()` trên thread đang chạy đều no-op) nên đường DevicePanel hiện tại gọi hook hai lần vô hại. Lưu ý: JUCE gọi `audioDeviceAboutToStart` **dưới** `audioCallbackLock` (`juce_AudioDeviceManager.cpp:1135-1140`) — hook "before" chỉ nên join (detector không chạm device manager nên không deadlock), còn việc `setWidth`/`start` lại có thể dời sang `ChangeListener` của `AudioDeviceManager` (`sendChangeMessage()` ngay sau, `:1143`) nếu muốn ra khỏi lock. Test: `AudioEngine` giả gọi `audioDeviceAboutToStart` khi một `NotchController` đang `start()`; assert hook được gọi và `isThreadRunning()==false` tại thời điểm `clear()`. **Mức thay đổi**: 0 dB.

### I2. Bypass `copy` ghi đè, Auto `+=` cộng: hai slot chung ngõ ra → một mic câm trong Bypass, nhảy mức khi chuyển mode

**Ở đâu.** `src/app/AudioEngine.cpp:854-859` (Bypass: `FloatVectorOperations::copy (lanes[i].out, lanes[i].in, n)`) so với `:896` (Auto: `out[n] += v`). Buffer đã `clear` toàn bộ ở `:845-852` nên Auto cộng đúng.

**Đường sai.** Routing hợp lệ theo spec G-2 (`2026-08-24-multi-slot-routing-design.md §2`, "route chéo tự do") và được `MainComponent.cpp:1669-1672` khẳng định là use-case thật ("*Two slots feeding one output are two DIFFERENT loops*"): slot 0 `in0→out0`, slot 1 `in1→out0`.
- AUTO: `out0 = notch(in0) + notch(in1)` (test `AudioEngineRouting.TwoMonoSlotsSumIntoOneOutputChannel` chốt).
- BYPASS: lane 0 copy `in0` vào `out0`, lane 1 copy `in1` **đè lên** → `out0 = in1`. Mic 1 **câm hoàn toàn** khi soundman bấm BYPASS ("tắt chống hú" phải là "nguyên bản", không phải "mất một mic"). Bấm AUTO trở lại: mic 1 quay về, tổng tăng tới **+6 dB** (hai nguồn cùng mức) ngay tại fader đang mở. Không test nào phủ Bypass + ngõ ra chung (`BypassFollowsTheSlotMappingNotTheChannelIndex` chỉ 1 slot).

**Sửa đề xuất.** `FloatVectorOperations::add (lanes[i].out, lanes[i].in, numSamples)` — buffer đã là 0 nên với routing không trùng, `add` lên 0 **bit-identical** với `copy` (test `BypassPassesInputThroughWithin001dB` giữ nguyên); clamp ±1.0 ở `:965-981` vẫn phủ tổng. Thêm test `TwoMonoSlotsSumIntoOneOutputChannelInBypass`. **Mức thay đổi**: 0 dB khi ngõ ra không trùng; khi trùng, Bypass = cùng tổng AUTO đang ra hôm nay (mic bị rơi quay lại, tối đa +6 dB so với bản lỗi, kẹp ±1.0).

### I3. `CandidateScorer` gate bằng hằng `kDefaultThreshold` (10) chứ không phải ngưỡng sống — preset AGGRESSIVE và nửa dưới slider PEAKINESS không có tác dụng

**Ở đâu.** `src/dsp/CandidateScorer.cpp:52-53` (`if (candidate.peakiness <= PeakinessAnalyzer::kDefaultThreshold) return out;` → score 0) và `:57` (`pNorm` chuẩn hoá theo cùng hằng). Trong khi `PeakinessAnalyzer::analyse` (`PeakinessAnalyzer.cpp:137`) dùng `threshold_` sống (clamp 5..20, `NotchController::setPeakinessThreshold`).

**Đường sai.** `src/gui/TuningPanel.cpp:63` preset AGGRESSIVE = `{ 100, 1, -24, 20, 8.0f }` → `MainComponent.cpp:789 → controller.setPeakinessThreshold(8.0)`. Một bin peakiness 9 (hú đang lên, chưa tới 10) được `analyse()` chấp nhận làm candidate, vào `scoreCandidateDetailed` → trả `score=0` ở dòng 52 → không bao giờ xác nhận. Kết quả: ngưỡng 8 ≡ ngưỡng 10; cả dải slider 5..10 (UI 6..10) là nút chết; nhãn "AGGRESSIVE" nói dối ở đúng trục nó quảng cáo. Ngược lại ngưỡng > 10 thì `analyse()` lọc trước nên hoạt động — bất đối xứng không ai thấy vì `tests/test_candidatescorer.cpp:515` cũng chỉ so với `kDefaultThreshold`.

**Sửa đề xuất.** Truyền ngưỡng sống vào scorer (`scoreCandidateDetailed(cand, mags, locked, thresholdLive)` hoặc `CandidateScorer::setThreshold` cùng atomic như rise-ref) và dùng nó cho cả gate lẫn `pNorm`. Test: ngưỡng 8, tone có peakiness 9 ⇒ score > 0 và (với rise/novelty đủ) xác nhận. **Mức thay đổi**: ngưỡng ≥ 10: giống hệt; ngưỡng < 10: **nhiều notch hơn** (đúng ý AGGRESSIVE) — là thay đổi hành vi dò, cần owner duyệt và nghe; `pNorm` đổi thang theo ngưỡng (`(p/thr−1)/9`) nên score của cùng một bin khác đi một chút khi thr ≠ 10 — ghi vào release note.

### I4. Docs drift: `GIOI-THIEU.md` hứa routing 8 slot trên "kênh bất kỳ của interface", engine mở 2 in / 2 out

**Ở đâu.** `docs/GIOI-THIEU.md:42` ("*mỗi slot tự chọn kênh vào/ra bất kỳ của interface*"), `:62` ("*8 slot xử lý độc lập … tự chọn kênh vào/ra của interface*"), `:49,:125` ("*hệ 8 slot stereo (16 kênh) mất ~72 giây*"). Thực tế `src/app/AudioEngine.cpp:12,65-67`: `initialise(kNumChannels=2, 2)` → callback nhận `numInputChannels = numOutputChannels = 2`; mọi lane trỏ kênh ≥ 2 bị bỏ ở `:783-785`. Giới hạn này **có** ghi — nhưng chỉ ở `docs/KY-THUAT-CHONG-HU.md:706` và `release-notes/1.4.0-alpha.md` ("Giới hạn đã biết"), không ở tài liệu giới thiệu mà tester đọc đầu tiên; spec lane U (`2026-10-18 … §1, mục 7`) xếp nó IMPORTANT còn mở. Tester cấu hình slot 3 → kênh 5/6 sẽ thấy "đường bị tắt" mà GIOI-THIEU bảo là được. **Sửa**: một dòng giới hạn ở §Routing và §ĐO của GIOI-THIEU, hoặc mở số kênh thật (thay đổi audio path riêng, ngoài review này). Lưu ý `validateRestoredSlots` (lane U) đã xử lý đúng hướng an toàn (tắt, không kẹp).

---

## 3. MINOR

### M1. Preset `Q` không có cận dưới/cận trên — Q=0,01 là một cú cắt −24 dB gần như băng rộng
`src/app/PresetManager.cpp:350` chỉ từ chối `Q <= 0`; `NotchController::setNotchImpl` (`:211`) cũng chỉ `Q > 0`. Depth đã kẹp −24 (Q12) nhưng Q thì không, trong khi detector tự kẹp Q 8..50 (`setNotchDefaults`). Tính thử Q=0,01 @ 1 kHz/48 k, −24 dB: `a2≈−0,926`, `|H|` tại DC ≈ −5 dB, tại 1 kHz −24 dB, băng cắt rộng nhiều octave. Hướng an toàn (chỉ giảm mức) nhưng là "wrong gain" từ một file tay. **Sửa**: `validate()` từ chối Q ngoài [1, 100] (hoặc kẹp về [8,50] như depth, có log). Mức: notch hẹp lại → tần số ngoài tâm trở về 0 dB.

### M2. Memory/spec ghi sai hệ quả của "`setSampleRate` không có caller"
`memory/ring-risk-lane-r-2026-09-06.md §6` + `memory/gain-aware-notch-lane-g-2026-09-07.md:223` + `docs/spec-ring-risk.md` "Known gaps": trình bày như lỗ hổng readout. Thực tế là B1. Cập nhật cả ba cùng fix B1; `MEMORY.md` index thêm bài học "giá trị khởi tạo 48000 trong `LaneAnalysis` là một hằng ẩn".

### M3. Test che lỗi: fixture tự gọi `setSampleRate(48000)`; không có test nào chạy wiring thật ở rate ≠ 48 k
`tests/test_gui_wiring.cpp:1028,1109,1184,1221,1258,1303,1529,1736`, `tools/snapshot.cpp:238`. Đề xuất một test "rate truyền từ engine tới snapshot" (xem B1) và bỏ các lời gọi tay sau khi B1 vá, để test đỏ nếu đường wiring đứt lần nữa.

### M4. Thiếu test cho ba đường đã tìm thấy lỗi
(a) Bypass + hai slot chung ngõ ra (I2); (b) ngưỡng peakiness < 10 xác nhận được (I3); (c) `audioDeviceAboutToStart` khi detector đang chạy (I1). Thêm (d) grep-test "`src/dsp/**` không `#include <juce_audio_devices`/`juce_gui_basics`/`JuceHeader`" theo kiểu `test_aztheme.cpp` — quy tắc CLAUDE.md đang đúng (đã grep) nhưng chưa có gì giữ nó.

### M5. CI không có sanitizer, không build Linux/clang mỗi PR
`.github/workflows/build.yml`: Windows mỗi PR, macOS tuần một lần. `memory/lane-u-dumb-geek-2026-10-18.md` ghi ASan (Linux) đã bắt một use-after-free trong fixture `Rig` lane M — tức có bug mà MSVC Release không thấy. Một job `ubuntu-latest` `-fsanitize=address,undefined` chạy `ctest` (bỏ test GUI nếu cần) rẻ hơn job macOS hiện có. Ghi chú: file này dùng `-A x64` không `-G` theo đúng CLAUDE.md.

### M6. NaN self-heal `chain.reset()` cancel mọi ramp đang chạy và không nạp lại thiết kế (đã ghi nhận, chưa đóng)
`AudioEngine.cpp:890-894` + `NotchChain.cpp:105-116` + `Biquad.h:100-111` tự mô tả GAP m-5. Hậu quả: sau một sample NaN, mọi notch đang ramp đứng ở độ sâu giữa chừng **vô hạn** trong khi `NotchInfo.depthDB` báo đích. Sửa rẻ: `NotchChain::reset()` (hoặc một `healNaN()` riêng cho đường này) sau khi reset state thì `setNotchFilter` lại từ `notchInfo_` cho slot Active — giống đường `setSampleRate`. Mức: 0 dB ở trạng thái bình thường; sau NaN-heal notch về đúng đích thay vì kẹt.

### M7. Comment/số dòng stale dễ dẫn người sửa sai
`AudioEngine.cpp:3` ("m-20"), `Biquad.h:100` ("AudioEngine.cpp:617"), `SoundcheckSignal.h:50` ("AudioEngine.cpp:642"), `SoundcheckController.cpp:1270-1271` (sai nội dung, xem B1), `SoundcheckController.h:418-420` ("NotchController.cpp:1093", "MainComponent.cpp:802-803"). Codebase này dựa nhiều vào comment làm hợp đồng; số dòng chết làm giảm độ tin. Đề xuất trỏ theo tên hàm/định danh thay vì số dòng.

### M8. `tools/afc/*.cpp` được biên dịch vào `HandsFreeTests`
`tests/CMakeLists.txt:49-51` + `test_afc.cpp`. Lane A đã DỪNG (owner 2026-10-19); mã nghiên cứu vẫn là một phần gate `ctest` của bản ship. Không sai, nhưng mỗi sửa trong `tools/afc` làm đỏ gate sản phẩm. Cân nhắc target test riêng (`AfcTests`) ngoài gate, hoặc để nguyên nhưng ghi vào `tools/CMakeLists.txt`.

### M10. `-DJUCE_USE_CURL=0` chỉ có tác dụng với target app — test/tool trên Linux không link được nếu thiếu libcurl
`CMakeLists.txt:200-206` đặt `JUCE_USE_CURL=0`/`JUCE_WEB_BROWSER=0` bằng `target_compile_definitions(HandsFree PRIVATE …)`; `tests/CMakeLists.txt` và `tools/CMakeLists.txt` không có dòng tương đương, nên biến cache `-DJUCE_USE_CURL=0` mà hướng dẫn Linux dùng **không** tới `juce_core.cpp` của `HandsFreeTests`/`HandsFreeSnapshot`/`PeakinessSweep`/`AfcSim` → 4 target này fail link `undefined reference to curl_easy_init` (log `build-build.log:247-276` trên worktree review; app link được). CI macOS không gặp vì JUCE mac không dùng curl; CI Windows cũng không. Sửa: lặp hai definition đó cho 4 target (hoặc một `target_compile_definitions` chung qua INTERFACE target), rồi hướng dẫn Linux trong task/CLAUDE.md không còn cần `-DJUCE_USE_CURL=0`. Không ảnh hưởng bản ship.

### M9. Docs/header nhỏ
- `docs/KY-THUAT-CHONG-HU.md:4` header "tại thời điểm 07/09/2026, v1.2.0" trong khi thân đã có §3.7 (1.3.0) và §3.8 (1.4.0).
- `README.md:11` "1.2.0 in alpha gate" — stale (1.3.0 đã đóng gói, 1.4.0 chờ).
- `CMakeLists.txt` `VERSION 1.3.0` + `release-notes/1.4.0-alpha.md` "chưa đóng gói" — nhất quán với quy trình (bump lúc release), chỉ ghi để người đọc không tưởng drift.
- `tools/logstats.py:48-58`: một `notch_set` tới cùng key `(slot,lane,index)` khi record cũ chưa có `notch_clear` (ví dụ Clear mất như I1) sẽ **mồ côi** record cũ (giữ `clear_t=None` → "still active", `held` = cả phiên). Nên đóng record cũ với `reason="superseded"` khi key bị ghi đè.

---

## 4. Những điểm đã kiểm và KHÔNG ghi thành finding (để người sau khỏi đào lại)

- `Biquad::designPeaking` nhận `depthDB = NaN/−inf` (NaN > 0 là false) → hệ số NaN. **Không tới được từ production**: `NotchController::setNotchImpl` (`:206-214`) `std::max(NaN,−24)` trả NaN rồi `!(NaN<=0)` từ chối; `−inf` kẹp về −24; `PresetManager::readNumber` từ chối non-finite; JSON không mã hoá NaN. Chỉ cần ghi nhớ nếu có caller mới của `NotchChain::setNotch`.
- SPSC: audio thread là producer duy nhất của tap/capture; `NotchController` producer duy nhất của command ring (mỗi slot một ring, một controller). `MainComponent::startAudio` khởi detector **sau** `engine_.start()` nên lần `clear()` đầu đúng precondition.
- `LockFreeRingBuffer` ordering acquire/release đúng; `clear()` chỉ sai khi vi phạm precondition (I1).
- `NotchController` không có đường deadlock: `modelMutex_` và `snapshotMutex_` không bao giờ lồng (kiểm `runOnce` step 3 tách scope); sink chạy ngoài lock; `placeConfirmed` hỏi `soundcheckActive()` trước khi khoá.
- `SoundcheckController` lifetime: `MainComponent` khai báo `soundcheck_` cuối, hủy đầu; 3 lambda gán một lần trước `start()`. `requestStop` dừng tiếng trên message thread không chờ poll. Ramp-out anchor latched trên audio thread (I-2) — kiểm đúng.
- `applySoundcheckResults` từ chối chạy ngoài message thread (F27) — đúng, nhưng xem B1 về rate.
- `HostSettings`: `(double) var` của chuỗi là 0 → rơi vào "không lưu"; `product` là `var` tự do, `AppSettings::readSlot` cả-hoặc-không. Lane U "card đã lưu mất → tắt mọi đường" đúng hướng an toàn.
- `PresetManager`: dedup `(slot,lane,index)` kể cả lane −1; `routeNotchesToSlots` kẹp routing (hợp lý, chỉ routing) và **không** kẹp hệ số (đúng Biquad.h). `planFor` giữ notch vượt Nyquist để tái dùng (D-00).
- `SessionLogger::log` không ở audio thread; `pruneOldFiles` không xoá file đang mở.
- `tools/logstats.py` đọc đúng `ev`, `notch_retune` không đóng record (B-3 lane G) — đúng.
- `PeakinessAnalyzer::peakinessAt` trả 0 cho mean ≤ 0 / không hữu hạn; `CandidateScorer` floor 1e-12 cho rise & novelty; `LoopGainEstimator` floor 1e-20; `SoundcheckCandidates` từ chối NaN hai lớp — kiểm đủ.
- Kiến trúc: `src/dsp/` chỉ include `juce_dsp` (grep sạch); `az_host_settings` là INTERFACE target không trỏ ngược `src/`; CLAUDE.md nói tách thư viện **khi có sản phẩm thứ hai** nên `LockFreeRingBuffer/SessionLogger/LicenseManager/theme` còn trong `src/` chưa phải vi phạm — chỉ thiếu guard (M4d).

## 5. Thứ tự làm đề xuất

1. **B1** (10 dòng + 2 test) — ship trước mọi bản alpha kế tiếp; ghi rõ trong note tester: "ở 44.1/96 kHz notch nay đặt đúng tần số; tone từng bị cắt nhầm quay lại".
2. **I2** (1 dòng + 1 test) — cùng PR hoặc PR nhỏ riêng; 0 dB cho routing không trùng.
3. **I1** (hook + test) — PR riêng, đụng lifecycle.
4. **I3** — cần owner duyệt vì đổi hành vi dò; kèm M4b.
5. I4/M2/M7/M9 — docs, không build.
6. M1, M6, M4d, M5, M8 — khi rảnh.

## 6. Build & test trên worktree review (Linux, clang, `origin/main` @ `f2cc11e`)

- Worktree tách riêng (`git worktree add --detach … origin/main`); `external/JUCE` và `external/melatonin_blur` rỗng trong worktree nên trỏ symlink tới submodule đã có của checkout chính (chỉ đọc), gỡ link trước khi `git worktree remove`.
- `cmake -B build -DJUCE_USE_CURL=0 -DCMAKE_BUILD_TYPE=Release` → cấu hình OK (ASIO SDK vắng, chỉ warning như kỳ vọng).
- `cmake --build build -j8`: target **HandsFree** (app) link được; **HandsFreeTests / HandsFreeSnapshot / PeakinessSweep / AfcSim** fail link `undefined reference to curl_*` (M10). Khắc phục chỉ cho mục đích review: `-DCMAKE_CXX_STANDARD_LIBRARIES=-lcurl`, build lại `HandsFreeTests`.
- `ctest --test-dir build -j8 --output-on-failure`:

```
100% tests passed, 0 tests failed out of 795
```

Suite xanh **không** mâu thuẫn với B1/I1/I2/I3: không test nào dựng wiring thật ở rate ≠ 48 k, không test Bypass với hai slot chung ngõ ra, không test ngưỡng < 10, không test `audioDeviceAboutToStart` khi detector đang chạy (xem M3/M4).
