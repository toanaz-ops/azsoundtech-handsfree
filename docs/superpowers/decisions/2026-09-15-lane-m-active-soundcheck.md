# Sổ quyết định — Lane M: Soundcheck đo chủ động (2026-09-15)

**Trạng thái: tất cả Q do điều phối chốt tạm theo phương án khuyên dùng; chờ
owner duyệt.**

**Cập nhật 2026-09-15 sau phản biện read-only vòng 1 (8 BLOCKER / 11 IMPORTANT /
8 MINOR, không finding nào bị bác).** Bốn mục mới ở cuối file: **Q3 (lật lại)**,
**Q15 (lật lại)**, **Q16**, **Q17**. Mục cũ giữ nguyên, không sửa — theo đúng
`recording-design-decisions`.

**Danh sách owner phải xác nhận TRƯỚC KHI phát tín hiệu ra PA thật — tám mục,
theo kết luận của phản biện:**

1. **Q2** — mức phát −20 dBFS, với cách diễn đạt trung thực ở spec §3 ("20 dB
   dưới toàn thang ở master hiện tại", không quy ra dB SPL).
2. **Q15 (lật lại)** — im **cả kênh ngõ ra** 4,5 s mỗi kênh, **~72 s** xấu nhất
   cho hệ 8 slot stereo.
3. **Q6** — chỉ đề xuất hay tự đặt. Prompt gốc của owner viết "**đặt** notch
   phòng ngừa"; điều phối chọn "đề xuất". Đây là một chỗ lệch với chỉ thị gốc.
4. **Q3 (lật lại)** — bộ điều kiện tự hủy còn lại, và **độ trễ dừng xấu nhất**
   (≤ 31 ms @ buffer 64, ≤ 51 ms @ buffer 1024, cộng trễ driver/amp).
5. **Q16** — nút `ĐO` riêng hay tái dùng `SOUNDCHECK`.
6. **Q7 + Q9** — bất đối xứng khi nạp lại preset: notch phòng ngừa đặt thì
   vĩnh viễn, nạp lại từ preset thì tự nhả sau 30 s.
7. **`kResultsTimeoutMs`** — 20 s (hạ từ 60 s).
8. **`ClearReason::SoundcheckReplace`** — thêm một giá trị vào enum của lane D.

Mỗi mục là một ngả rẽ thiết kế. Ghi đủ: câu hỏi, các phương án đã đề xuất (kèm
phương án khuyên dùng), và lựa chọn. Lật lại khi cần đổi hướng; đừng hỏi lại
câu đã có đáp án ở đây.

Spec đích: `docs/superpowers/specs/2026-09-15-active-soundcheck-design.md` (**rev 2**).
Người hỏi: Fable (điều phối). Người quyết: **owner (ToanAZ) — chưa duyệt.**
Owner không có mặt 2026-09-15 và đã yêu cầu giảm tối đa số câu hỏi, nên điều
phối chốt tạm để spec viết được; mọi `Chọn` dưới đây lật lại được, chi phí là
sửa spec, chưa có dòng code nào dựa vào nó.

---

## Bối cảnh nêu trước khi hỏi (đọc code 2026-09-15, main `a6be099`, 1.2.0)

Những sự thật dưới đây quyết định phần lớn không gian phương án:

- **"Soundcheck" đã là một thứ khác trong code.** `AudioEngine::Mode::Soundcheck`
  (`src/app/AudioEngine.h:66`) là chế độ **thụ động**: `applyModeGating` gọi
  `NotchController::startSoundcheck()` (`src/app/MainComponent.cpp:801-802`),
  mở một cửa sổ **15 s** (`kSoundcheckDurationMs`,
  `src/app/NotchController.h:91`) trong đó notch đặt ra mang
  `Origin::Soundcheck` (`src/app/NotchController.h:62`) và theo KD-7 **không bao
  giờ tự nhả, không bao giờ đào sâu** (`src/app/NotchController.cpp:727-729`).
  Nói cách khác: app hôm nay "soundcheck" bằng cách **chờ phòng tự hú 15 giây**
  — đúng như research §0 mô tả ("Soundcheck = chờ phòng tự hú 15 s. Không có
  phép đo chủ động"). Lane M thêm phép đo chủ động, nó **không phải** là cái nút
  đó.
- **Nút SOUNDCHECK có sẵn** (`gui::ModeRail::soundcheckButton`,
  `src/gui/ModeRail.h:81`) cùng đồng hồ đếm ngược đọc từ
  `getSoundcheckRemainingMs` (`src/app/MainComponent.cpp:247-251`).
- **Tap mà detector đọc là OUTPUT, không phải mic.** Callback gán
  `tapSource[slot][lane] = out` (`src/app/AudioEngine.cpp:561`) rồi ghi ring từ
  đó (`src/app/AudioEngine.cpp:657-666`). Trộn tín hiệu test vào `out` là đầu độc
  chính đường detector đang chạy.
- **Không có limiter ở đâu cả.** Đường ra chỉ có một **kẹp cứng**
  ±`kMaxOutputLevel` = 1.0f (`src/app/AudioEngine.cpp:15`, áp ở `:631-647`) — đó
  là *clipper* toàn thang, không phải limiter. Prompt gốc của owner viết
  "limiter vẫn nằm trong đường" — sự thật là **không có**; spec phải nói đúng và
  tự lo phần an toàn.
- **Đường đặt notch có sẵn** là `NotchController::setNotch(channel, index, freq,
  Q, depthDB, Origin)` (`src/app/NotchController.h:221-223`). Nó **ghi đè im
  lặng** ô `index` đang có notch sống (`setNotchImpl`, `src/app/NotchController.cpp:195`
  trở đi — không kiểm `n.active`), và với origin khác `Detector` thì **đặt luôn
  `ceilingDb = depth`**. Không có API "tìm index còn trống".
- **Thang độ sâu lane G**: `kDepthLadderDb = {−6, −12, −18, −24}`
  (`src/app/NotchController.h:109`), trần `kMaxDepthDb = −24`
  (`src/app/NotchController.h:114`), Q mặc định `kDefaultNotchQ = 30`
  (`src/app/NotchController.h:100`), 16 ô mỗi làn (`kSlots`,
  `src/app/NotchController.h:73`).
- **Phân giải phân tích** cố định ở `Detector::kFftSize = 2048`,
  `kHopSize = 512`, `kNumBins = 1025` (`src/dsp/Detector.h:66-68`) → 23,4 Hz/bin
  @ 48 kHz.
- **Đồng hồ tiêm được** đã có: `ClockSource` (`src/dsp/ClockSource.h:8-13`) —
  máy trạng thái lane M test headless được nhờ nó.
- **RING RISK** đã có data (`SnapshotBuffer::ringRiskScore/Valid/Threshold`,
  `src/app/NotchController.h:426-436`; `docs/spec-ring-risk.md`), lane M dùng
  được làm điều kiện "phòng đã gần hú, đừng quét".

---

## Q1 — Tín hiệu test là gì, dài bao lâu mỗi ngõ ra?

| # | Phương án | |
|---|---|---|
| 1 | **Log sine sweep (chirp) 100 Hz → 10 kHz, 3,0 s + đuôi im 0,7 s mỗi ngõ ra**, ramp raised-cosine 30 ms hai đầu. Sinh bằng bộ tích lũy pha dạng đóng — không cấp phát, không buffer, test headless khẳng định được từng mẫu | khuyên dùng |
| 2 | MLS (chuỗi giả ngẫu nhiên): crest factor phẳng nhất, nhưng cần tương quan vòng để giải, nhạy với phi tuyến và với phòng thay đổi theo thời gian, và khó khẳng định bằng test đơn vị | |
| 3 | Tone bậc thang 1/6 octave: đơn giản và an toàn nhất, nhưng ~40 bậc × 200 ms = 8 s mỗi ngõ ra và phổ thưa — bỏ sót mode hẹp giữa hai bậc | |

**Chọn: 1.** — điều phối tự chọn phương án khuyên dùng 2026-09-15, owner CHƯA
duyệt; lật lại được trước khi release.

## Q2 — Mức phát: trần dBFS của sweep, ramp, và cảnh báo "hạ master trước"?

| # | Phương án | |
|---|---|---|
| 1 | **Đỉnh −20 dBFS mặc định**, chỉnh được **chỉ xuống** (−20 … −40 dBFS), **kẹp cứng trong code** ở −20 dBFS (không đường nào phát to hơn, kể cả preset hay file cấu hình), ramp 30 ms hai đầu, hộp thoại bắt buộc "HẠ MASTER TRƯỚC — app sắp phát tín hiệu test ra loa" phải bấm xác nhận mỗi lần chạy | khuyên dùng |
| 2 | Đỉnh −12 dBFS mặc định: SNR tốt hơn 8 dB, gần mức chương trình hơn nên phép đo đúng hơn với hệ phi tuyến — đổi lại ồn hơn hẳn và sai lầm đắt hơn | |
| 3 | Đỉnh −30 dBFS mặc định: an toàn nhất, nhưng trong phòng ồn (quạt, khán giả vào sớm) nhiều bin sẽ rơi xuống "không đo được" và lane M trả về tay trắng | |

**Chọn: 1.** — điều phối tự chọn phương án khuyên dùng 2026-09-15, owner CHƯA
duyệt; lật lại được trước khi release. **Đây là Q owner phải xác nhận trước khi
có tín hiệu thật ra PA.**

## Q3 — An toàn: dừng khẩn, tự hủy, và điều kiện từ chối chạy?

| # | Phương án | |
|---|---|---|
| 1 | **Cả ba tầng**: (a) dừng khẩn = nút STOP to trên analyser **và** bất kỳ phím nào **và** engine dừng/thiết bị lỗi → ramp-out 30 ms rồi im (không cắt phựt); (b) tự hủy khi mic vượt −6 dBFS RMS trong > 20 ms, hoặc khi `ringRiskScore ≥ ringRiskThreshold` (phòng bắt đầu hú thật), hoặc khi `getTapDropCount` tăng (driver hụt); (c) **từ chối chạy** khi engine chưa `isRunning()`, hoặc `getNumOutputChannels()/getNumInputChannels()` còn 0 (chưa callback nào chạy), hoặc slot bị disable, hoặc RING RISK đang ≥ RISING | khuyên dùng |
| 2 | Chỉ nút STOP + điều kiện từ chối; không tự hủy theo mức đo được | |
| 3 | Chỉ tự hủy theo mức; không có điều kiện từ chối (cho người vận hành tự chịu) | |

**Chọn: 1.** — điều phối tự chọn phương án khuyên dùng 2026-09-15, owner CHƯA
duyệt; lật lại được trước khi release.

## Q4 — Trình tự theo ngõ ra (per-lane = per-output, lane S): lần lượt hay đồng thời? Bao nhiêu slot?

| # | Phương án | |
|---|---|---|
| 1 | **Một ngõ ra một lúc**, thứ tự (slot, lane) tăng dần, **chỉ slot đang enabled**, cách nhau 300 ms im. Hộp thoại xác nhận nêu trước tổng thời lượng ("N ngõ ra × 3,7 s ≈ M giây"). Lý do: hai loa quét cùng lúc thì không tách được cộng hưởng nào do loa nào — mà đó chính là lý do lane S tồn tại — và công suất âm đỉnh chỉ tập trung ở một loa | khuyên dùng |
| 2 | Quét đồng thời mọi ngõ ra bằng tín hiệu trực giao (sweep ngược chiều / lệch pha): nhanh gấp N lần, nhưng tách kênh dựa trên giả định, và mọi loa cùng kêu | |
| 3 | Chỉ quét slot đang hiển thị trên GUI: nhanh, nhưng người vận hành 4 khu sẽ tưởng đã đo cả 4 | |

**Chọn: 1.** — điều phối tự chọn phương án khuyên dùng 2026-09-15, owner CHƯA
duyệt; lật lại được trước khi release.

## Q5 — Đo cái gì, bằng FFT nào, trung bình ra sao?

| # | Phương án | |
|---|---|---|
| 1 | **Loop gain theo tần số bằng TỈ SỐ NĂNG LƯỢNG mỗi bin**: `H[k]² = Σ_f |Y_f[k]|² / Σ_f |X_f[k]|²`, `X` = phổ tín hiệu phát, `Y` = phổ mic, tổng trên TOÀN bộ lần quét (kể cả đuôi) nên **không cần bù trễ round-trip**. Cùng FFT của detector: 2048 điểm, hop 512, Hann. Ứng viên = cực đại cục bộ nhô **≥ 6 dB** trên đường `H` đã làm trơn 1/3 octave (để notch **mode phòng**, không notch đáp tuyến loa) | khuyên dùng |
| 2 | Chỉ nhặt đỉnh của phổ mic, không chia cho phổ phát: rẻ, nhưng nhặt trúng hình dạng của chính cái sweep + đáp tuyến loa | |
| 3 | Giải chập sweep → đáp ứng xung đầy đủ → phân tích mode (tần số, Q, τ): chính xác nhất, ra được cả τ mà lane G đang thiếu — nhưng cần căn mẫu chính xác, nhiều code, và là một lane riêng | |

**Chọn: 1.** — điều phối tự chọn phương án khuyên dùng 2026-09-15, owner CHƯA
duyệt; lật lại được trước khi release. Ghi chú: chọn đúng FFT của detector là có
chủ ý — notch phòng ngừa rơi vào **đúng bin** mà detector sau này sẽ gia cố.

## Q6 — Hành động: tự đặt notch, hay chỉ đề xuất?

| # | Phương án | |
|---|---|---|
| 1 | **Chỉ đề xuất, rồi ÁP DỤNG một cú bấm**: kết quả vẽ lên analyser (đường margin + marker ứng viên), một dải kết quả ghi "tìm thấy N điểm dễ hú" với nút `ÁP DỤNG` và `BỎ`. Không có gì tới PA nếu người vận hành không bấm | khuyên dùng |
| 2 | Tự đặt ngay khi đo xong (một bấm là xong hẳn), có UNDO 30 s | |
| 3 | Tự đặt **và** không cho bỏ: soundcheck xong là phòng đã được bảo vệ | |

**Chọn: 1.** — điều phối tự chọn phương án khuyên dùng 2026-09-15, owner CHƯA
duyệt; lật lại được trước khi release. Tự đặt (PA 2) là **bậc sau**, mở khi có
bằng chứng từ rig rằng ứng viên lane M không sai. **Đây là Q owner phải xác nhận
trước khi có tín hiệu thật ra PA.**

## Q7 — Độ sâu / Q của notch phòng ngừa, quan hệ với thang lane G?

| # | Phương án | |
|---|---|---|
| 1 | **Đi qua đúng API có sẵn** `setNotch(lane, index, f, Q, depth, Origin::Soundcheck)` — không thêm đường thứ hai. Độ sâu = **bậc nông nhất của `kDepthLadderDb` đủ kéo margin về ≥ 6 dB**, tức lượng tử `−(H_dB + 6)` lên bậc sâu kế tiếp, rồi kẹp trần preset và kẹp `kMaxDepthDb = −24`. `Q` = `getNotchQ()` (mặc định 30). KD-7 áp dụng nguyên xi: notch phòng ngừa **không tự nhả, không tự đào sâu** | khuyên dùng |
| 2 | Vào với `Origin::Detector` ở −6 rồi để thang G tự đào: thống nhất với lane G, nhưng notch phòng ngừa được đặt **đúng lúc phòng đang im** nên đồng hồ nhả 30 s sẽ xóa nó trước khi có ai hát | |
| 3 | Thêm `Origin::Preventive` với luật nhả riêng: sạch về ngữ nghĩa, nhưng là đường code thứ hai — prompt owner cấm | |

**Chọn: 1.** — điều phối tự chọn phương án khuyên dùng 2026-09-15, owner CHƯA
duyệt; lật lại được trước khi release. Hệ quả phải ghi vào spec: (a) notch
phòng ngừa **không bao giờ tự biến mất**, nên mỗi lần chạy mới phải **xóa notch
`Origin::Soundcheck` của lần trước** trên slot đó, không thì chúng chồng lên
nhau; (b) "phòng nhớ" của lane G đã có cổng không cho Soundcheck tiêu ký ức
(memory note lane G, mục A.13) — lane M **không ghi và không tiêu** ký ức phòng.

## Q8 — Mic không nghe thấy loa (SNR quá thấp) thì sao?

| # | Phương án | |
|---|---|---|
| 1 | **Hai tầng ngưỡng, và tay trắng là kết quả hợp lệ**: đo nền nhiễu 500 ms im **trước** mỗi sweep; nếu năng lượng băng đo của lần quét không vượt nền **≥ 12 dB** → ngõ ra đó trả "**không đo được**", không đặt gì, log `soundcheck_output` với `ok: false`. Trong một ngõ ra đo được, bin nào không vượt nền **≥ 6 dB** thì đánh dấu **không tin cậy** và **loại khỏi việc nhặt đỉnh** | khuyên dùng |
| 2 | Tự tăng mức phát lên tới trần rồi đo lại: SNR tốt hơn, nhưng "app tự vặn to hơn" là đúng thứ không được phép trong live sound | |
| 3 | Vẫn đo, gắn cờ "độ tin cậy thấp" và vẫn cho đề xuất notch | |

**Chọn: 1.** — điều phối tự chọn phương án khuyên dùng 2026-09-15, owner CHƯA
duyệt; lật lại được trước khi release.

## Q9 — Lưu kết quả: vào preset? vào log session JSONL?

| # | Phương án | |
|---|---|---|
| 1 | **Log JSONL là sổ cái; preset không đổi định dạng.** Năm sự kiện mới, khóa dispatch là `ev` (không phải `kind` — bài học lane G mục A.7): `soundcheck_start`, `soundcheck_output`, `soundcheck_result`, `soundcheck_apply`, `soundcheck_abort`. Notch phòng ngừa đã áp dụng là notch bình thường trong model nên `savePreset` **đã** ghi chúng; nạp lại chúng về là `Origin::Preset` và **sẽ tự nhả** — chấp nhận cho v1, **ghi rõ vào docs**. Đường cong đo được **không** vào preset, chỉ sống trong phiên (analyser vẽ) và trong log | khuyên dùng |
| 2 | Thêm cờ `preventive: true` vào `PresetNotch` và adopt lại thành `Origin::Soundcheck`: đúng ngữ nghĩa hơn, nhưng đổi định dạng preset + đổi đường `adoptPreset` — một lane con của chính nó | |
| 3 | Loại notch Soundcheck khỏi `savePreset`: người vận hành lưu preset sau soundcheck rồi mất sạch công đo, im lặng | |

**Chọn: 1.** — điều phối tự chọn phương án khuyên dùng 2026-09-15, owner CHƯA
duyệt; lật lại được trước khi release.

## Q10 — GUI: analyser hiện gì trong lúc đo và sau khi đo?

| # | Phương án | |
|---|---|---|
| 1 | **Tối thiểu, ba thứ**: (a) trong lúc đo — một overlay mờ toàn analyser với `ĐANG ĐO · ngõ ra 2/4` + đồng hồ (tái dùng `countdownLabel` của `ModeRail` qua `getSoundcheckRemainingMs`) và một nút `DỪNG` to; (b) sau khi đo — một **đường margin** (dB) vẽ chồng lên phổ của làn đang hiển thị + marker ở các ứng viên; (c) một dải kết quả một dòng: "tìm thấy N điểm dễ hú" + `ÁP DỤNG` / `BỎ`. Số dB chỉ hiện khi rê chuột, không phải chữ chính | khuyên dùng |
| 2 | Không overlay, chỉ một bảng số (tần số / margin / độ sâu đề xuất) trong panel bên phải | |
| 3 | Không GUI mới: nút + log, kết quả đọc bằng `tools/logstats.py` | |

**Chọn: 1.** — điều phối tự chọn phương án khuyên dùng 2026-09-15, owner CHƯA
duyệt; lật lại được trước khi release. Ràng buộc cho lane U (Dumb mode) sau này:
chữ chính phải là câu người, không phải số — "tìm thấy N điểm dễ hú", không phải
"margin 3,2 dB @ 2,1 kHz".

## Q11 — Test headless được tới đâu, cái gì buộc phải ra rig?

| # | Phương án | |
|---|---|---|
| 1 | **Tách bốn khối thuần tính toán ra khỏi JUCE/thiết bị** để `ctest` phủ: bộ sinh sweep (hàm thuần của chỉ số mẫu), bộ ước lượng loop gain (chạy trên "phòng tổng hợp" = biquad cộng hưởng + trễ), bộ nhặt ứng viên (prominence, loại bin không tin cậy, lượng tử lên thang, kẹp trần), và máy trạng thái chạy trên `ClockSource` giả. Chỉ mức thật ra loa, đúng-sai của đường cong so với mic đo chuẩn, và tốc độ dừng khẩn trong phòng thật là **việc của rig** | khuyên dùng |
| 2 | Test tích hợp qua `AudioEngine` với thiết bị giả: thật hơn, nhưng phụ thuộc JUCE device layer trong CI | |
| 3 | Chỉ test bộ sinh tín hiệu, phần đo để rig xác nhận | |

**Chọn: 1.** — điều phối tự chọn phương án khuyên dùng 2026-09-15, owner CHƯA
duyệt; lật lại được trước khi release.

## Q12 — Quan hệ với mode `Soundcheck` THỤ ĐỘNG đã có (chờ phòng tự hú 15 s)?

| # | Phương án | |
|---|---|---|
| 1 | **Một hành động riêng, không đụng `AudioEngine::Mode`.** Nút mới `ĐO` cạnh `SOUNDCHECK`. Khi chạy: ghi nhớ mode hiện tại, **tắt detection trên MỌI slot** (`setDetectionActive(false)`), quét, rồi khôi phục bằng đúng `applyModeGating` (`src/app/MainComponent.cpp:783`). Lý do bắt buộc phải tắt: sweep **là** một tone thuần quét ngang phổ với rise rất dốc — để detector chạy là bảo nó notch chính cái sweep của mình | khuyên dùng |
| 2 | Bấm `SOUNDCHECK` lần nữa khi đang ở mode Soundcheck = chạy đo chủ động: không thêm nút, nhưng một nút hai nghĩa trên thiết bị live-sound | |
| 3 | Thêm mode thứ tư `Measure` vào `AudioEngine::Mode`: rõ ràng nhất, nhưng mọi `switch` trên Mode (ModeRail, ModeBar, `applyModeGating`, `modeName`, preset) phải mở ra | |

**Chọn: 1.** — điều phối tự chọn phương án khuyên dùng 2026-09-15, owner CHƯA
duyệt; lật lại được trước khi release.

## Q13 — Tín hiệu ĐO lấy từ đâu: mic thô, hay tap post-notch đang có?

| # | Phương án | |
|---|---|---|
| 1 | **Ring thu riêng cho lane M, ghi từ `inputChannelData[inIdx]` (mic THÔ, trước mọi DSP)**, một ring mỗi làn, chỉ cấp phát lúc app khởi động và chỉ ghi khi đang đo. Đường tap/detector có sẵn **không bị đụng tới** | khuyên dùng |
| 2 | Tái dùng tap có sẵn: không thêm ring nào — nhưng tap được ghi từ **`out`** (`src/app/AudioEngine.cpp:561`), nên một khi sweep được trộn vào `out` thì tap chứa chính cái sweep và `H` ra ≈ 1 bất kể phòng. **Sai về bản chất** | |
| 3 | Tiêm sweep **sau** khối ghi tap để tap sạch: không thêm ring — nhưng lúc đó sweep đã đi qua khỏi kẹp đầu ra (`src/app/AudioEngine.cpp:631-647`) và tới driver **không được kẹp**. **Sai về an toàn** | |

**Chọn: 1.** — điều phối tự chọn phương án khuyên dùng 2026-09-15, owner CHƯA
duyệt; lật lại được trước khi release. Hệ quả: `H` đo được **không** chứa các
notch đang chạy, tức là loop gain trần của phòng. Nên bin nào **đã có notch
sống trong ±1 bin** phải bị loại khỏi danh sách ứng viên, không thì notch chồng
notch.

## Q14 — Các số điều phối tự chốt

Gom một chỗ để phản biện bắn vào một chỗ. Không con số nào dưới đây có cơ sở đo
đạc trên rig này — chúng là mặc định hợp lý, chờ lần đầu chạy thật.

| Số | Giá trị | Vì sao |
|---|---|---|
| Băng quét | 100 Hz – 10 kHz | Dưới 100 Hz mic cardioid + loa không đủ đáp; trên 10 kHz hú rất hiếm |
| Thời lượng sweep | 3,0 s | Đủ để mỗi bin nhận năng lượng qua vài frame ở 2048/512 |
| Đuôi im sau sweep | 0,7 s | Bắt phần ngân của mode Q cao; RT60 phòng nhỏ thường < 0,7 s |
| Ramp hai đầu | 30 ms raised-cosine | Không click; ngắn hơn nhiều so với hằng thời gian tai |
| Khoảng cách hai ngõ ra | 300 ms | Đủ để ring thu xả, và nghe ra là hai lần quét riêng |
| Đỉnh mặc định | −20 dBFS | Q2 |
| Kẹp cứng | −20 dBFS | Q2 |
| Ngưỡng tự hủy mic | −6 dBFS RMS trong > 20 ms | Cách clip 6 dB |
| SNR toàn băng tối thiểu | +12 dB trên nền | Dưới mức này tỉ số năng lượng chủ yếu là nhiễu |
| SNR mỗi bin tối thiểu | +6 dB trên nền | Cùng lý do, mức bin |
| Ứng viên khi | `H_dB ≥ −6 dB` | "Còn 6 dB nữa là dao động" — margin cổ điển |
| Prominence tối thiểu | 6 dB trên đường trơn 1/3 octave | Tách mode phòng khỏi đáp tuyến loa |
| Margin đích sau notch | 6 dB | Đủ để một thao tác vặn volume nhỏ không làm hú ngay |
| Số notch phòng ngừa tối đa | 6 mỗi làn | Còn 10 ô trống trong 16 cho detector lúc chạy show |

**Chọn: giữ** — điều phối tự chọn 2026-09-15, owner CHƯA duyệt; lật lại được
trước khi release.

## Q15 — Trong lúc quét, có tắt đường mic → ngõ ra đang đo không?

Câu hỏi này nảy ra từ chính Q13: nếu đường mic vẫn chạy thì lúc đo vòng hú vẫn
đóng, và ta đang phát tín hiệu vào một vòng có thể tự kích.

| # | Phương án | |
|---|---|---|
| 1 | **Tắt phần đóng góp của slot đang đo vào ngõ ra đang đo** trong suốt lần quét (callback bỏ qua việc cộng dồn làn đó). Vòng hú **hở** → không thể hú trong lúc test, và `H` đo được là **thuần loa → phòng → mic**. Đổi lại: trong 3,7 s đó ngõ ra ấy không có tiếng mic | khuyên dùng |
| 2 | Để nguyên đường mic (vòng kín): đo đúng vòng như nó đang là, nhưng **phép test có thể tự gây hú** — đúng cái ta đang cố tránh | |
| 3 | Tắt MỌI slot trên MỌI ngõ ra trong suốt lần chạy: sạch nhất, nhưng cả hệ câm trong ~M giây, kể cả các khu không liên quan | |

**Chọn: 1.** — điều phối tự chọn phương án khuyên dùng 2026-09-15, owner CHƯA
duyệt; lật lại được trước khi release. **Đây là Q owner phải xác nhận trước khi
có tín hiệu thật ra PA** (nó là một thay đổi level: một ngõ ra về im trong 3,7
s).

---

---

**Q3 và Q15 lật lại, cộng Q16, Q17 — từ phản biện read-only vòng 1 (2026-09-15).**
Mục cũ ở trên **không bị sửa**; đây là các lựa chọn thay thế, ghi nối tiếp.

## Q3 (lật lại 2026-09-15) — Bộ điều kiện tự hủy: `ringRiskScore` trong lúc chạy là một điều kiện CHẾT

Phản biện (BLOCKER 1) chứng minh điều kiện tự hủy `ringRiskScore ≥
ringRiskThreshold` **không bao giờ có thể đúng** trong lúc quét: detection tắt ⇒
`processSpectrumForDetection` trả về ngay (`src/app/NotchController.cpp:1281-1282`)
nên `frameScoreValid_` không bao giờ true; và tap bị treo ⇒ không block nào được
drain ⇒ khối publish snapshot (`src/app/NotchController.cpp:617-650`) không chạy
⇒ `copySnapshot()` trả một khung đông cứng. Cùng lúc, điều kiện "`getTapDropCount`
tăng" cũng chết, vì thiết kế treo hẳn việc ghi tap.

| # | Phương án | |
|---|---|---|
| 1 | **Ring risk đọc ở `Preflight` và `Arm`** (lúc tap còn chạy, snapshot còn sống). Trong lúc chạy, thay bằng phép đo của **chính lane M**: RMS mic trượt 20 ms từ `micCapture_`, và peakiness của cửa sổ nền 0,5 s trước mỗi sweep vượt `CandidateScorer::kConfirmScore`. Thay `getTapDropCount` bằng `micCaptureDrops_`. Cộng: đổi sample rate / số kênh, `isRunning()` false, `getLastDeviceError()` khác rỗng | khuyên dùng |
| 2 | Giữ ring risk trong lúc chạy bằng cách **bật lại detection giữa các kênh** để snapshot refresh: đúng số liệu hơn, nhưng detector sẽ nhìn thấy đuôi sweep và có thể đặt notch lên chính nó | |
| 3 | Bỏ hẳn tự hủy theo mức trong lúc chạy, chỉ còn nút `DỪNG` và các điều kiện từ chối ở `Preflight` | |

**Chọn: 1.** — điều phối tự chọn phương án khuyên dùng 2026-09-15, owner CHƯA
duyệt; lật lại được trước khi release. Kèm: độ trễ dừng xấu nhất được ghi thành
bảng trong spec §3 (≤ 31,3 ms @ buffer 64 / ≤ 51,3 ms @ buffer 1024, cộng trễ
driver và amp), và ramp-out do **chính audio callback** sinh nên không phụ thuộc
thread nào còn sống.

## Q15 (lật lại 2026-09-15) — Tắt tiếng theo (slot, lane) KHÔNG mở được vòng hú

Phản biện (BLOCKER 2): nhiều slot **cộng dồn** lên cùng một kênh ngõ ra — callback
xoá trắng mọi kênh ra trước (`src/app/AudioEngine.cpp:565-577`) đúng để DSP cộng
dồn `out[n] += v` (`src/app/AudioEngine.cpp:621`). Tắt một `(slot, lane)` để các
slot khác tiếp tục bơm tiếng mic vào **cùng kênh đó**: vòng vẫn đóng, mà mọi
detector đã bị tắt. `EY` của phép đo cũng nhiễm chương trình của slot khác trong
khi `EX` thì không.

| # | Phương án | |
|---|---|---|
| 1 | **Tắt MỌI làn của MỌI slot có `outputChannels[lane] == scOutChannel_`.** Kênh đó chỉ mang sweep. Giá: kênh im **4,5 s** mỗi lượt, **~72 s** xấu nhất (16 kênh × 4,5 s). Phụ thu tốt: không còn nguy cơ clipping trên kênh đang đo, và `EY` sạch phần chương trình của app | khuyên dùng |
| 2 | Giữ lựa chọn cũ (một `(slot, lane)`) — phản biện đã chứng minh nó không mở được vòng | |
| 3 | Tắt **mọi kênh ngõ ra** trong suốt lần chạy: sạch nhất, loại luôn rủi ro "vòng qua kênh khác vẫn đóng" (spec §7.3), nhưng cả hệ câm ~72 s kể cả khu không liên quan | |

**Chọn: 1.** — điều phối tự chọn phương án khuyên dùng 2026-09-15, owner CHƯA
duyệt; lật lại được trước khi release. **Owner phải xác nhận trước khi có tín
hiệu thật ra PA.** Nếu owner thấy "vòng qua kênh khác vẫn đóng" là không chấp
nhận được thì PA 3 là câu trả lời, và giá của nó chỉ là sự im lặng, không phải
code thêm.

## Q16 — Nút `ĐO` riêng, hay tái dùng nút `SOUNDCHECK` đã có?

Phản biện (MINOR 24): prompt gốc của owner nói **tái dùng** nút SOUNDCHECK, Q12
lại thêm một nút mới mà không gắn cờ. Và mô tả "placeholder sweep the room"
trong prompt **không tồn tại trong repo**: hai nút thật là
`gui::ModeRail::soundcheckButton {"SOUNDCHECK"}` (`src/gui/ModeRail.h:81`, đang
hiển thị) và `gui::ModeBar::soundcheckButton {"Run Soundcheck (15s)"}`
(`src/gui/ModeBar.h:43`, `modeBar_` bị ẩn — `src/app/MainComponent.cpp:211`).

| # | Phương án | |
|---|---|---|
| 1 | **Nút `ĐO` riêng** cạnh `SOUNDCHECK`. Hai chức năng khác hẳn nhau (chờ phòng tự hú 15 s, so với phát tín hiệu ra PA 72 s) thì phải là hai nút; một nút phát ra loa mà người vận hành có thể bấm nhầm khi định làm việc khác là chuyện không nên có trên thiết bị live-sound | khuyên dùng |
| 2 | Tái dùng `SOUNDCHECK` như prompt gốc: bấm khi đang ở mode Soundcheck = chạy đo chủ động. Không thêm widget, nhưng một nút hai nghĩa | |
| 3 | Nút `ĐO` riêng **và** bỏ luôn mode Soundcheck thụ động (đo chủ động thay thế hẳn nó): giao diện gọn nhất, nhưng xoá một hành vi đang chạy và phá KD-7 | |

**Chọn: 1.** — điều phối tự chọn phương án khuyên dùng 2026-09-15, owner CHƯA
duyệt; lật lại được trước khi release. **Đây là chỗ lệch với chỉ thị gốc của
owner, nên owner phải chốt.**

## Q17 — Số sửa sau phản biện

Các hằng số đổi so với Q14, mỗi dòng gắn với finding đã buộc nó đổi.

| Hằng | Q14 | Q17 | Vì sao |
|---|---|---|---|
| `kResultsTimeoutMs` | 60 000 ms | **20 000 ms** | F9: `Results` nay chạy **với detection đã bật lại**, nên đồng hồ nhả lane G chạy thật; 60 s đủ để một notch −24 mất hai bậc trong khi spec khai 0 dB |
| `kTrustedHighHz` | (không có) | **6 000 Hz** | F19: sweep biên độ hằng gửi năng lượng tỉ lệ 1/f, nên ở 10 kHz mỗi bin thấp hơn 100 Hz đúng 20 dB và rụng dưới `kMinBinSnrDb`. 6–10 kHz vẽ nhưng **không** đề xuất |
| `kMinUsefulCutDb` | (không có) | **3,0 dB** | F6/F22: `kCandidateMarginDb = −6` là ngưỡng **đánh dấu**; cần một ngưỡng **đề xuất** riêng, không thì một bin cần 0,1 dB vẫn ăn một notch −6 và một ô chuỗi |
| `kRampOutMs` | (ngầm = `kRampMs`) | **30,0 ms**, hằng riêng | F8: ramp-out do **callback** sinh từ `scRampOutAtSample_`, là một hàm khác cửa vào, nên nó phải có tên riêng |
| Công thức độ sâu | `needed = -(H_dB + 6)` | **`needed = H_dB + kTargetMarginDb`, `depth_raw = -needed`** | F6: dấu của rev 1 cho `depth_raw` **dương**, mà `setNotchImpl` từ chối `depth > 0` (`src/app/NotchController.cpp:214`). Không phải chỉnh số, là sửa lỗi |
| Bão hoà độ sâu | (không định nghĩa) | **kẹp ở `kMaxDepthDb = −24`, báo `residualDb`** | F6: rev 1 không nói gì khi cần cắt hơn 24 dB |

**Chọn: giữ** — điều phối tự chọn 2026-09-15, owner CHƯA duyệt. Riêng
`kResultsTimeoutMs` và `ClearReason::SoundcheckReplace` nằm trong danh sách tám
mục owner phải xác nhận ở đầu file.


## Ghi chú quy trình

Sổ này viết theo `.claude/skills/recording-design-decisions/SKILL.md`, với một
sai lệch cố ý và đã khai báo: **không có owner để trả lời**, nên mọi
`**Chọn:**` là lựa chọn tạm của điều phối. Khi owner duyệt (hoặc đổi), **không
sửa mục cũ** — thêm mục `Q<n> (lật lại <ngày>)` như skill quy định.
