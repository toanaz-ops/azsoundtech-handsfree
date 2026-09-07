### Task 10: docs, tester notes, release notes, memory, roadmap

**Mức level dự kiến:** **0 dB** — no code changes. This task exists because a behaviour change nobody wrote down is a behaviour change the next session will "fix" back. Every level figure quoted below must be copied from spec §3 and from the number `NotchChain.MeasuredAttenuationMatchesEveryLadderRungWithinHalfADecibel` actually printed in Task 2, not invented here.

**Two claims this task must carry BECAUSE no test can (M-3, §5.4).** Both belong in the tester notes, phrased as things to listen for rather than as facts already established:

1. **"The ladder stops at the first rung that quiets the bin" (Q7) is a RIG claim.** The test harness writes the raw tone into `h.tap` (`tests/test_notchcontroller.cpp:344-349`), so the analyser never sees a notched spectrum and the ladder always climbs to the ceiling in the suite. On a real rig it should frequently stop at −6 or −12 — that is the tone win this lane was asked for. If it never does, if every notch on the rig ends at the ceiling, the reinforce loop is reading the wrong spectrum: that is a bug report, not a preference.
2. **The snapshot image** (`console-live.png`, Step 9) is the only check anywhere that the ACTIVE NOTCHES depth column reads a ladder rung at all.

**Files:**
- Modify: `docs/GIOI-THIEU.md:28`, `:41`, `:45`, `:50`
- Modify: `docs/KY-THUAT-CHONG-HU.md:32`, `:79-92`, `:177`, `:193-196`, `:225`, `:280-285`, `:322-326`, `:350-360`, `:389`
- Modify: `docs/spec-ring-risk.md` (new section before "## Out of scope")
- Modify: `docs/superpowers/specs/2026-09-05-data-loop-design.md:103-111`
- Modify: `docs/superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md:23`, `:70`, and lane A's row
- Modify: `docs/superpowers/specs/2026-09-06-gain-aware-notch-design.md:5` (status line)
- Modify: `installer/TESTER-NOTES.md` (header block + a new "Mới trong 1.2.0" section)
- Create: `docs/release-notes/1.2.0-alpha.md`
- Create: `memory/gain-aware-notch-lane-g-2026-09-07.md`; modify `memory/MEMORY.md`

**Interfaces:** none — prose only.

- [ ] **Step 1: `docs/GIOI-THIEU.md`**

Line 28, inside the overview diagram, replace `tự nhả filter khi hết hú`:
```
                            nhả dần từng bậc khi hết hú
```

Line 41, the **Notch siêu hẹp** row — the depth default is now a ceiling:
```
| **Notch siêu hẹp** | 16 notch/làn, mỗi slot mono (1 làn) hoặc stereo (2 làn), 8 slot — tối đa 256 chuỗi notch. Q chỉnh được 8–50 (mặc định 30). Độ sâu −6 đến −24 dB (mặc định −18) nay là **TRẦN**: notch đặt ở −6 dB (hoặc −12 nếu đỉnh lên dốc) rồi chỉ đào sâu thêm 6 dB mỗi 300 ms chừng nào bin đó còn hú, không bao giờ quá trần. Chỉ mất đúng vài Hz quanh tần số hú |
```

Line 45, replace the **Tự nhả sau 30 giây** row:
```
| **Nhả dần theo bậc** | Hết hú 30 giây → nông đi 6 dB; mỗi 10 giây yên tiếp theo nông thêm một bậc; tới −6 dB thì nhả hẳn (tổng ~50 giây). Hú quay lại là kẹp ngay về bậc sâu nhất đã từng đứng. Phòng đang căng (chip RING RISK ≥ RISING) thì đồng hồ **đứng yên** — notch giữ nguyên bậc chừng nào phòng còn căng |
| **Nhớ phòng 5 phút** | Hú quay lại đúng bin cũ trong 5 phút sau khi nhả hẳn → đặt lại thẳng ở độ sâu đã từng cần, không dò lại từ −6 dB |
```

Line 50, the presets row — say the two numbers are ceilings now:
```
| **Preset có sẵn** | `Speech` (Q=40, trần −18 dB — hà khắc cho loa hội thoại) và `Music` (Q=25, trần −10 dB — dịu cho nhạc sống) đúng giá trị trong repo. Trần −10 của Music **tới được**: theo Q13 thang hiệu lực là −6 → −10, bậc cuối chính là trần, nên Music vẫn cắt đủ 10 dB như 1.1.3 chứ không bị lượng tử về −6. Installer chép hai preset vào máy, app tự seed chúng vào `%APPDATA%` lúc first-run (không bao giờ ghi đè file người dùng đã sửa), và GUI có hai nút **LOAD… / SAVE…** dưới mục INTERFACE để nạp/lưu preset (`*.json`). Từ 1.2.0 file lưu ra mang **độ sâu phòng đã cần** (`deepestDb`) và mang cả trần trong `notchDefaults`, nên nạp lại đúng như lúc lưu. |
```

- [ ] **Step 2: `docs/KY-THUAT-CHONG-HU.md`**

Line 32, the mermaid box: `auto-release 30 s` → `thang nhả 30 s + 10 s/bậc`.

Line 225, the mermaid note: `Khóa khi vượt ngưỡng,<br/>tự nhả sau 30 s im` → `Đặt nông rồi đào sâu theo nhu cầu,<br/>nhả dần từng bậc khi im`.

Section **### Bộ lọc notch và độ sâu** (lines 79-92) — append after the existing `depthDB > 0` paragraph:

```markdown
**Đổi độ sâu trên notch đang chạy (1.2.0).** `Biquad::setNotchFilter` gọi
`reset()` mỗi lần đổi hệ số, đúng khi tần số hoặc Q đổi và **sai** khi chỉ độ
sâu đổi: xóa `z1/z2` giữa dòng tín hiệu là một bước nhảy vào loa.
`Biquad::rampNotchDepth` giữ nguyên state và nội suy tuyến tính 5 hệ số trong
`NotchChain::kRampMs` = **10 ms** (≤ 0,6 dB/ms). `NotchChain::setNotch` chỉ đi
đường ramp khi slot đang Active **và** `freq`, `Q` bằng đúng giá trị đã lưu;
mọi trường hợp khác vẫn reset như cũ.

Vì sao bộ hệ số nội suy an toàn: cố định `freq/Q/sr`, mọi tổ hợp lồi của các bộ
peaking đã chuẩn hóa vẫn **là** một peaking RBJ với gain tử `A_n ≤ 1 ≤ 1/A_d`,
nên `|H| ≤ 1` ở **mọi** tần số — ramp không thể khuếch đại gì, kể cả tần số nó
đang nhắm — và bán kính cực `sqrt((1 − α/A_d)/(1 + α/A_d)) < 1` nên không phân
kỳ. Đo 06/09/2026: gain lớn nhất 1,9e-15 dB trên 3 sample rate × 5 tần số × 3 Q
× 7 cặp độ sâu × 101 điểm, và 20 000 tổ hợp lồi ngẫu nhiên × 400 tần số. Test
`Biquad.RampMidpointsNeverBoostAnyFrequency` chốt điều này bằng cách đọc hệ số
**đang chạy** giữa ramp rồi tính `|H|` giải tích.

Suy giảm đo được tại f0 sau khi ramp xong, từng bậc, sai số ±0,5 dB:
`NotchChain.MeasuredAttenuationMatchesEveryLadderRungWithinHalfADecibel`.
```

Line 177, the RESPONSE preset line — note that the depth figure is a ceiling:
```
SAFE (500 ms/4/trần −12 dB/Q40/thr 12.0), BALANCED (250/3/trần −18/30/10.0), AGGRESSIVE
```

Section **### 3.4 NotchController**, lines 193-196 — replace the whole "Auto-release 30 s" paragraph:

```markdown
**Thang độ sâu (1.2.0, lane G).** Depth không còn là một số cố định đọc lúc
đặt. Bậc cố định: `kDepthLadderDb = {−6, −12, −18, −24}`; slider độ sâu của
preset là **trần**, đọc **sống** mỗi tick. **Thang hiệu lực (Q13) = các bậc cố
định NÔNG HƠN trần, cộng chính trần làm bậc cuối**: trần −10 (`presets/Music.json`)
⇒ −6 → −10; trần −13,7 ⇒ −6 → −12 → −13,7; trần −18 ⇒ −6 → −12 → −18; trần −6
⇒ không bao giờ đào. Bậc cuối = trần, có thể là số lẻ — đây là chỗ DUY NHẤT một
notch Detector không đứng trên bậc cố định. Không có bước lượng tử hóa nào:
lượng tử trần −10 xuống −6 sẽ làm Music nông hơn 1.1.3 4 dB mà không ai báo.

- **Đặt**: −6 dB, hoặc −12 nếu `riseRatio ≥ 2.0` (đỉnh lên ≥ 6 dB trong cửa sổ
  rise), rồi kẹp về trần. Notch Soundcheck không có thang (KD-7).
- **Đào**: trong vòng reinforce, bin còn vượt ngưỡng peakiness ⇒ sâu thêm một
  bậc mỗi `kDeepenAfterMs` = 300 ms, dừng ở trần (bước cuối có thể NHỎ hơn
  6 dB: trần −10 thì bước cuối là 4 dB). Tiêu chí đào và tiêu
  chí "còn hú" là **cùng một phép thử trên phổ SAU notch**, nên thang dừng ở
  bậc đầu tiên làm bin hết vượt ngưỡng — có thể là −6 hoặc −12 suốt đời notch.
  Đó là chủ ý (Q7): độ sâu theo nhu cầu, không theo mặc định. **Không test nào
  trong suite chứng minh được điều này** (M-3): harness ghi tone THÔ vào
  `h.tap` (`tests/test_notchcontroller.cpp:344-349`) nên analyser không bao giờ
  thấy phổ đã bị notch, và trong test thang luôn leo tới trần. Chỉ dàn thật
  kiểm chứng được — nêu trong tester notes, đừng dựng test giả cho nó.
- **Nhả**: `quietMs` tích lũy khi bin im; ≥ 30 s ⇒ nông một bậc, mỗi 10 s tiếp
  theo một bậc nữa, tới −6 thì `Clear(AutoRelease)`. Đồng hồ **đóng băng** khi
  `frameScoreValid_ && frameMaxScore_ ≥ 0.55 × kConfirmScore` (băng RISING của
  chip), không có trần thời gian (Q9); cờ `SnapshotBuffer::releaseFrozen`
  publish ra nhưng 1.2.0 chưa vẽ.
- **Kẹp lại**: bin hú lại khi đang nhả ⇒ về `deepestDb` **ngay trong frame
  đó**, không chờ 300 ms — nhưng **luôn kẹp bởi trần đang có hiệu lực**
  (`max(deepestDb, ceilingRung)`, M-B). Trần đọc sống, nên nếu người vận hành
  kéo slider nông đi trong lúc notch đang nhả, lần kẹp lại KHÔNG được vượt
  giá trị mới. Song song, `deepestDb` của notch Detector bị kẹp về trần mỗi
  tick, kể cả khi notch đang nông hơn trần — đó là đường duy nhất bắt được
  trường hợp này.
- **Nhớ phòng**: mỗi làn 16 mục `{tần số, deepestDb, thời điểm nhả}`, TTL 5
  phút, khớp **đúng cùng bin** (±0 — lệch một bin là 21,5 Hz @44,1k/2048, đủ
  để một partial nhạc cụ bên cạnh kế thừa nhầm một vết cắt sâu). Dùng một lần.
  Xóa sạch khi `setWidth` / `clearAll` / `setSampleRate`. Không persist ra đĩa.
- Mọi lần đổi độ sâu đi qua `pushRetuneLocked` — anh em của `pushClearLocked`,
  gọi khi đã cầm `modelMutex_`. Không gọi `setNotch`/`setNotchImpl` từ đó
  được: mutex **không đệ quy**, và `setNotchImpl` sẽ ghi đè `lockedAtMs`, tức
  nhãn tuổi mà lane D ghi vào mọi `notch_clear`.

Notch Soundcheck vẫn **miễn trừ** mọi thứ ở trên (KD-7) — chỉ nhả qua
`clearNotch`/`clearAll` tường minh.
```

Section **## 4. Preset** (around line 285) — append:

```markdown
Từ 1.2.0 `savePreset` ghi `deepestDb` (độ sâu phòng đã cần) chứ không phải bậc
đang đứng lúc bấm SAVE, và ghi cả `notchDefaults {Q, depth}` từ tuning đang
chạy — trước đó `savePreset` không set `notchDefaults`, nên nạp lại rơi về mặc
định −12 dB của `PresetNotchDefaults` và trần bị hạ hai bậc mà không ai báo.
Độ sâu sâu hơn −24 dB trong file bị **kẹp về −24** lúc adopt, cho mọi Origin,
và `adoptPreset` ghi một dòng `juce::Logger` đếm số notch bị kẹp (Q12).
```

Section **## 5. Tổng hợp các giới hạn an toàn** (line 322-326) — add three rows:

```markdown
| Đổi độ sâu giữa dòng tín hiệu thành tiếng "cạch" | `Biquad::rampNotchDepth` giữ state, nội suy 10 ms; chỉ đi đường này khi cùng `freq`/`Q` |
| Ramp khuếch đại giữa chừng | Chứng minh tổ hợp lồi `A_n ≤ 1 ≤ 1/A_d` + test đo `\|H\|` tại 5 điểm giữa ramp |
| Notch sâu hơn −24 dB từ file preset | Kẹp ở `setNotchImpl` cho MỌI Origin, có log (Q12); `pushRetuneLocked` từ chối |
```

Section **## 7. Log session** (line 359-360) — add the row and amend `notch_set`:

```markdown
| `notch_retune` | lane G đổi độ sâu một notch đang chạy | slot, làn, index, Hz, Q, `depth_db` (mới), `from_db` (cũ), `origin`, `reason`: `deepen` / `release` / `reclamp` / `ceiling`, `age_ms` (tính từ lúc ĐẶT, không phải từ lần retune trước) |
```

Section **## 8. Trạng thái & kiểm chứng** (line 389) — replace the version line with 1.2.0 and the suite count the final run reports.

- [ ] **Step 3: `docs/spec-ring-risk.md`**

Insert before `## Out of scope`:

```markdown
## Người đọc thứ hai: thang nhả lane G (1.2.0)

Cho tới 1.1.3 chỉ GUI đọc `ringRiskScore`. Từ 1.2.0 `NotchController` tự đọc
**số của chính nó** — `frameMaxScore_` / `frameScoreValid_`, hai member chỉ
sống trên detector thread và chứa đúng hai số vừa publish vào snapshot — để
**đóng băng đồng hồ nhả** khi `frameScoreValid_ && frameMaxScore_ ≥ 0.55 ×
CandidateScorer::kConfirmScore`, tức đúng ranh RISING của
`SpectrumView::riskForScore`. Hằng số là MỘT theo nghĩa đen (m-D, rev 3):
`NotchController::kRiskFreezeFraction = 0.55f` là ĐỊNH NGHĨA, còn
`SpectrumView::kRingRiskRisingFraction` chỉ là bí danh
(`= NotchController::kRiskFreezeFraction`). Không còn hai literal 0.55f để
lệch nhau; đổi một chỗ là đổi cả hai.

Ba điều cần biết khi đọc chip cạnh thang nhả:

- **Không lấy `snapshotMutex_`.** Hôm nay `modelMutex_` và `snapshotMutex_`
  chưa bao giờ lồng nhau; đọc snapshot từ trong vòng nhả sẽ tạo thứ tự khóa
  mới cho một readout — không đáng (M-5). Cờ `releaseFrozen` được publish
  trong scope RIÊNG, **trước** khi lấy `modelMutex_`.
- **Chip và đồng hồ có thể lệch nhau** (M-8). Chip còn qua hold 750 ms và chỉ
  đọc `displayedSlot_`, nên chip có thể báo RISING khi đồng hồ đã chạy lại, và
  một slot không hiển thị có thể đang đóng băng mà màn hình không nói gì.
  `SnapshotBuffer::releaseFrozen` tồn tại để sau này nói được điều đó — 1.2.0
  publish nhưng chưa vẽ (Q9).
- **`ringRiskValid == false` KHÔNG đóng băng** (invariant 7). Tắt detection
  phải nhả y như 1.1.3, nếu không thì tắt detection sẽ giam mọi notch lại.
- **Không có trần thời gian cho đóng băng** (Q9). Phòng còn căng thì notch còn
  giữ bậc, không giới hạn.
```

- [ ] **Step 4: `docs/superpowers/specs/2026-09-05-data-loop-design.md`**

In the §3.2 table (lines 103-111), insert after the `notch_set` row:

```markdown
| `notch_retune` | NotchController (detector thread) | `slot`, `lane`, `index`, `hz`, `q`, `depth_db` (mới), `from_db` (cũ), `origin`, `reason`: `deepen` / `release` / `reclamp` / `ceiling`, `age_ms` |
```

and append below the table:

```markdown
`notch_retune` (lane G, 1.2.0) là **cập nhật**, không phải đóng/mở: một notch
có thể retune nhiều lần giữa `notch_set` và `notch_clear` của nó.
`tools/logstats.py` xử lý nó bằng cách sửa bản ghi đang mở (`depth_db`,
`deepest_db`, `retunes`) và **không** đóng bản ghi — nếu đóng thì một lần đào
sâu ở mốc 300 ms sẽ biến mọi notch được đào thành "notch sống 300 ms", tức một
false positive giả trên mọi dòng thống kê.

Reader là một chuỗi `if/elif` trên `ev` và **bỏ qua tên lạ**, nên một file log
cũ đọc bằng tool mới vẫn chạy đúng, và một `ev` thêm sau này cũng không làm
hỏng reader cũ.
```

- [ ] **Step 5: roadmap + spec status**

`docs/superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md` line 23 — replace the lane G row's last column with `đã hạ cánh 1.2.0`, and line 70's `| G | chờ S, D | |` with `| G | đã hạ cánh 1.2.0 | thang độ sâu + nhả dần + nhớ phòng; ramp 10 ms trong Biquad |`. In lane A's row, add: `fallback notch = thang G (đặt −6, đào 6 dB/300 ms, nhả 30 s + 10 s/bậc)`.

`docs/superpowers/specs/2026-09-06-gain-aware-notch-design.md` line 5 — change `**Trạng thái:** spec v2 ... chờ owner duyệt trước khi viết plan` to `**Trạng thái:** đã thực thi, 1.2.0 alpha (plan: docs/superpowers/plans/2026-09-07-gain-aware-notch.md).`

- [ ] **Step 6: `docs/release-notes/1.2.0-alpha.md`**

Create it in the shape of `1.1.3-alpha.md`. It must contain, in this order: the version and date; the suite count from the final `ctest` run; the four measured attenuations from Task 2's test; a "cái gì đổi" list covering placement (−6/−12), deepening (6 dB/300 ms), the release ladder (30 s + 10 s/rung, Clear at −6), the freeze (no time cap), room memory (5 min, same bin, one use), the −24 clamp, the 10 ms ramp, `notch_retune` in the log, and `savePreset` writing `deepestDb` + `notchDefaults`; and an explicit **"nghe ở âm lượng thấp trước"** block naming the three rows from spec §3 that testers will hear: placement (up to ~0.6 s more howl before it is fully suppressed), the release window from 30 s to the Clear at 40–60 s depending on the rung reached (up to 18 dB more tone missing than 1.1.3), and the "stuck on a shallow rung" case (6–12 dB shallower for the notch's whole life — the intended tone win, and the one thing the suite cannot check). It must also state the Q13 consequence for the shipped presets: `Music` still reaches its −10 dB ceiling and `Speech` its −18, so neither is quieter than on 1.1.3 once the ladder has climbed.

- [ ] **Step 7: `installer/TESTER-NOTES.md`**

Update the header block (version, build date, suite count, SHA-256 — the SHA comes from the installer the release script actually produces, so fill it in AFTER Step 9). Add a `## Mới trong 1.2.0` section before `## Mới trong 1.0.5`, written for a soundman, not a developer. It must say:

- notches now start shallow and get deeper only while the howl continues, so a sudden howl may be audible ~0.3–0.6 s longer than on 1.1.3 — **test at low volume first**;
- a notch that never needs to go deep will stay at −6 or −12 for its whole life, and that is correct, not a bug — **and this is the one behaviour no automated test in the project can check** (M-3), so it is the rig's job: if every notch on your rig ends up at the DEPTH slider's value, say so, because that is the reinforce loop misreading the spectrum;
- after a howl stops the notch now backs off in steps instead of vanishing at 30 s: the first step is at 30 s and each one after it costs 10 s, so a notch that reached the DEPTH slider's value takes **40–60 s** to disappear (40 s from −12, 50 s from −18, 60 s from −24) and between 30 s and that moment **more tone is missing than on 1.1.3**;
- while RING RISK reads RISING or CRITICAL, notches stop backing off entirely, with no time limit;
- the same howl coming back within 5 minutes is notched deep immediately;
- what to report: a click or a "zip" when a notch changes depth (there must be none — every change is ramped over 10 ms); a howl the app never gets on top of; a notch that goes deeper than the DEPTH slider says.

- [ ] **Step 8: `memory/`**

Create `memory/gain-aware-notch-lane-g-2026-09-07.md` with the front-matter shape the other notes use, recording whatever this lane actually taught — at minimum: that a coefficient ramp needs a state-identity assertion because every black-box measurement passes a silent `reset()` (M-7); that the reinforce loop and `runOnce` step 3 both already hold a non-recursive `modelMutex_`, which is why a `*Locked` sibling was the only option (B-1); that `pushClearLocked` leaves every field but `active` intact, so slot reuse must be re-initialised at `setNotchImpl` (B-2); that `ev` — not `kind` — is the log's dispatch key (B-3); and the M-9 consequence that "deepen" and "still howling" are the same test, so the ladder stops at the first rung that works.

Add, from this plan's own two 2026-09-07 revisions — these are lessons about writing the plan, and they are the ones that cost the most:

- **`pushClearLocked` retaining `depthDB` makes "depth < 0" a false liveness test.** Six tests across three tasks were written on it and would all have passed against cleared slots. The accessor `activeForTest` exists only because of this.
- **A ladder's timings must be derived from the rung, not from a round number.** `55000.0` looked like "past the 50 s release" and was 5 s short of the 60 s a −24 notch actually needs. Write the arithmetic in the comment or it drifts.
- **A scoring fixture has to be derived from the scorer's own formula before it is written — and twice, because the first derivation solved the wrong variable.** The spec's illustrative +3 dB/250 ms can never confirm (`rNorm = (rise−1)/0.5` against a ≥ 112.5 ms reference gives 0.35 against a 0.7 floor). But fixing the SLOPE still could not produce a −6 placement: for the first 11 tone blocks the rise reference is a NOISE frame, and a tone that switches on above the floor gives `riseRatio ≈ 64` at any slope — while peakiness, being scale-invariant, confirms within ~4 blocks, i.e. inside that window. The parameter that decides the band is the ramp's **starting amplitude**, which has to equal the noise floor's per-bin magnitude. Two lines of algebra beat a build; three lines beat two.
- **Q13: quantising a ceiling to a rung silently made the shipped `Music` preset 4 dB shallower than 1.1.3.** The defect was invisible in the spec, in the plan and in every test, because no test used a ceiling that was not a multiple of 6. When a constant can take values off the grid your tests use, test one that is.
- **A plan is not verified until someone opens the files it cites.** Three helpers used in Task 9 (`TempDir`, `pumpOneBlockThroughSlotZero`, `notchControllerForTest`) did not exist anywhere in the repo; the plan itself flagged them as unverified and the flag was not acted on until a second reader looked.
- **A one-way clamp guarding a value that has TWO ways to become stale is half a clamp** (M-B, and this is the safety lesson of the lane). The ceiling branch only clamped `deepestDb` when it also emitted a `Set` — i.e. only when the notch was currently deeper than the ceiling. A notch that had RELEASED to a shallower rung skipped the branch entirely and kept a `deepestDb` from under the old ceiling, which the next reclamp then honoured: **12 dB deeper than the operator's slider, on a live PA.** The fix is two lines and both are needed: clamp the remembered value unconditionally every tick, and clamp the reclamp target at the moment it is used. Whenever a "deepest ever" or "high water mark" is kept alongside a limit that can move, ask what happens when the limit moves while the value is not in use.
- **A test that needs a state should build it the way production does.** Rev 2 reached "already released" with `retuneForTest(..., Release)`, which sets `depthDB` and nothing else — so the branch under test was never entered and the test would have read the deepen path's answer instead. The seam looked like it set the state because its argument was named `Release`.

Add one line to `memory/MEMORY.md`'s `## Notes` list, in the same style as its neighbours, linking the new file.

- [ ] **Step 9: Verify and commit**

Run: `cd build && ctest -C Release`
Expected: `100% tests passed` — record the number for the release note and the tester notes.

Run: `build/tools/Release/HandsFreeSnapshot.exe shots 1440 920 --fast`
Then READ `shots/console-live.png` back and check the ACTIVE NOTCHES depth column shows a ladder rung (−6 or −12), not −18. Send it to the owner — GUI-visible behaviour is not reported without a picture (CLAUDE.md).

```bash
rm -f .superpowers/sdd/.gitignore
```
```bash
git add docs/GIOI-THIEU.md docs/KY-THUAT-CHONG-HU.md docs/spec-ring-risk.md docs/release-notes/1.2.0-alpha.md installer/TESTER-NOTES.md memory/MEMORY.md memory/gain-aware-notch-lane-g-2026-09-07.md
```
```bash
git add docs/superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md docs/superpowers/specs/2026-09-05-data-loop-design.md docs/superpowers/specs/2026-09-06-gain-aware-notch-design.md
```
```bash
git commit -m "docs: lane G -- depth ladder, release staircase, room memory, notch_retune schema"
```

- [ ] **Step 10: Release to the alpha testers**

Per CLAUDE.md's standing instruction, a finished change ships. This one is a minor version.

```bash
pwsh -File installer\release-alpha.ps1 -Part minor
```

Then fill the produced installer's SHA-256 and size into `installer/TESTER-NOTES.md`, and commit the version bump `CMakeLists.txt` change plus the notes:

```bash
git add CMakeLists.txt installer/TESTER-NOTES.md
```
```bash
git commit -m "chore: release 1.2.0 to alpha (lane G)"
```

---

## Self-review

Run against the spec with fresh eyes after the plan was written, then **re-run
twice on 2026-09-07 against the real files** by two read-only sessions. The
"Revision 2" table at the top is the first pass; the "Revision 3" table above it is
the second, which re-derived the first pass's own arithmetic and found B-5 still
open, M-A untestable and M-B a Q1 violation carried since spec v2. This section
reflects the plan AFTER both.

### 1. Spec coverage

| Spec § | Requirement | Task |
|---|---|---|
| §2.1 | place at −6, deepen 6 dB, never past the preset ceiling | 5, 6 |
| §2.2 | release ladder −18 → −12 → −6 → Clear, 30 s then 10 s, reclamp immediately | 6 (reclamp branch), 7 (ladder, **and the reclamp's only test** — M-A: `releasedSteps > 0` is a state only the release ladder can build) |
| §2.3 | room memory: same frequency within 5 min restarts at the old depth | 8 |
| §2.4 | release clock freezes at RISING/CRITICAL | 7 |
| §2.5 | every depth change is a state-preserving 10 ms ramp | 1, 2 |
| §3 | the level table — expected change stated per task | every audio-path task's "Mức level dự kiến" line; §3's rows are quoted into 1, 2, 5, 6, 7, 8 |
| §4.1 (Q13) | effective ladder = rungs shallower than the ceiling + the ceiling itself; no quantisation; live ceiling LOWERING (reason `Ceiling`, Detector only); RAISING buys nothing until reinforce; Preset/Manual own ceiling | 4 (`nextDeeperRungDb` two-arg + `TheEffectiveLadderEndsOnTheCeilingItself`), 5 (`AnOffRungCeilingIsItselfTheDeepestPlacement`), 6 (`TheLastStepLandsExactlyOnAnOffRungCeiling`), 7 (`LoweringTheCeilingPullsADetectorNotchUpOnTheNextTick`, `RaisingTheCeilingDoesNotDeepenUntilTheBinRingsAgain`, `AnOffRungPresetDepthSurvivesTheFirstTick`) |
| §4.2 | 5 new `ModelNotch` fields, `setNotchImpl` initialises all of them (B-2), −24 clamp + adopt log (Q12) | 4 |
| §4.2 | `ScoreBreakdown::riseRatio` | 3; carried onto `NotchEvent::riseRatio` in 4 so a test can prove which band a fixture landed in (B-5) |
| §4.3 | placement steps 1–4, Preset/Manual/Soundcheck rules. The block sits BELOW the index lookup (B-2) and the Soundcheck line is LAST | 5 (steps 1, 2, 4 + the final shape), 8 (step 3 filled in, same shape re-shown) |
| §4.4 | `pushRetuneLocked`, deepen with the 300 ms gate, immediate reclamp **clamped to the live ceiling** (M-B), LINKED same rung, M-9 note | 4 (helper), 6 (loop; reclamp target `max(deepestDb, ceilingDbFor(n))`), 7 (`AReturningHowlReclampsImmediatelyToDeepestDb`, `ALoweredCeilingAlsoCapsTheReclampTarget`) |
| §4.5 | freeze from `frameMaxScore_`/`frameScoreValid_`, `quietMs`, thresholds, `releaseFrozen` (published EVERY tick, false when the tap is dead — M-6), test seams | 4 (seams), 7 (loop) |
| §4.6 | `ReleasedMemory`: 16/lane, ring, AutoRelease only, same bin, TTL, one use, LINKED both lanes, wipes; an off-rung remembered depth is clamped to the ceiling on read and NOT quantised (m-8) | 8 |
| §4.7 | `Biquad::rampNotchDepth`, the no-boost proof in the doc string, `setNotchFilter`/`reset` cancel, `clearNotch` note, `NotchChain::setNotch` routing, `kRampMs` | 1, 2 |
| §4.8 | `Kind::Retune`, `RetuneReason`, `fromDepthDb`, `ev: "notch_retune"`, `logstats.py`, `SnapshotNotch::deepestDb`, `SnapshotBuffer::releaseFrozen`, `savePreset` (Q11) | 4 (types, snapshot), 9 (writer, reader, preset) |
| §4.9 | the constant table, one place, not on the GUI | 4 |
| §4.10 inv. 1 | never a Set outside [−24, 0] | 4 (clamp + refusal), 7 (fuzz test) |
| §4.10 inv. 2 | never deeper than the ceiling, **including on the reclamp path** (M-B) | 5 (placement clamp), 6 (reclamp target clamped), 7 (per-tick unconditional `deepestDb = max(deepestDb, ceiling)` + `ALoweredCeilingAlsoCapsTheReclampTarget`) |
| §4.10 inv. 3 | ≤ 6 dB per deepening step (Q13 can make the LAST step smaller, never larger); shallow steps may be larger | 6 (`nextDeeperRungDb`), 7 (ceiling/reclamp comments) |
| §4.10 inv. 4 | a rejected design leaves the slot untouched | 1, 2, 4 |
| §4.10 inv. 5 | ramp only between designs sharing freq/Q/sr | 2 |
| §4.10 inv. 6 | `processSample` allocates nothing, one branch | 1 |
| §4.10 inv. 7 | freeze only when valid; detection off releases as before | 7 |
| §4.10 inv. 8 | Soundcheck exempt from all of it | 5, 6, 7 |
| §5.1 | eight Biquad tests, state identity and gain measurement included | 1 (nine tests) |
| §5.2 | NotchChain ramp / reset routing, ±0.5 dB per rung, `NotchInfo.depthDB` immediate | 2 |
| §5.3 | controller tests | 4, 5, 6, 7, 8 — see the row-by-row list below |
| §5.3 "trần nâng về −18 ⇒ không đào lại cho tới khi reinforce" | the RAISE half of the live-ceiling rule | 7 (`RaisingTheCeilingDoesNotDeepenUntilTheBinRingsAgain`) — **added by rev 2; the v1 plan covered only the lowering half** |
| §5.3 "Preset: adoptPreset −12 ⇒ không đào; nhả thang; kẹp lại về −12" | a preset notch obeys the release ladder and reclamps to its OWN depth | 6 (`APresetNotchNeverDeepens`), 7 (`APresetNotchReleasesDownTheLadderAndReclampsToItsOwnDepth`) — **the release/reclamp half added by rev 2** |
| §5.3 "INDEP: làn kia không đổi" | the INDEP half of the LINKED/INDEP rung rule | 6 (`IndepLeavesTheOtherLaneUntouchedThroughTheWholeClimb`) — **added by rev 2; the v1 plan covered only LINKED** |
| §5.3 "Kẹp lại không vượt trần đã hạ" | the four-step sequence that reaches the unconditional `deepestDb` clamp — the only path where the `Set(ceilingRung)` branch cannot do the clamping for it | 7 (`ALoweredCeilingAlsoCapsTheReclampTarget`) — **added by rev 3 (M-B), together with the spec bullet it tests** |
| §4.9 `kRiskFreezeFraction` | one constant for the RISING band, not two | 4 (declaration), 7 Step 3 (`SpectrumView.h:239` becomes an alias of it — m-D; rev 2 still had two `0.55f` literals) |
| §5.3 "Đào … dừng ở bậc đầu tiên làm bin hết vượt ngưỡng" (Q7) | NOT covered by any test, and cannot be — see item 6 of §2 below | 6 (level note), 10 (tester notes) |
| §5.4 | snapshot image showing a ladder rung | 10 (Step 9) |
| §6 | docs, tester notes, release notes, roadmap, memory | 10 |
| §7 / Q13 | the decisions locked in the spec | honoured throughout; none re-opened. Q13 is the owner's 2026-09-07 addition and is applied in Tasks 4, 5, 6, 7, 8 and 10 |

### 2. Spec requirements I could NOT map to a task

Stated explicitly, as required:

1. **§5.3, "`riseRatio` 1.9 → −6".** The boundary value cannot be produced deterministically from the audio fixture — the rise axis is a ratio between two real FFT frames, and no source in `tests/test_notchcontroller.cpp` can be dialled to land on 1.9 rather than 1.87 or 1.94. What Task 5 pins instead is the two sides of the line by construction: `RampSineSource`, starting AT the noise floor (`kRampStartAmp = 3.0e-4`, rev 3 / B-5) and rising +9.5 dB/250 ms, places at −6 with its logged `riseRatio` asserted to be in **[1.5, 2.0)**, and `SineSource`'s hard start (riseRatio ≫ 2) places at −12. Since rev 2 the band is asserted rather than assumed, so a fixture that drifts out of it fails loudly instead of passing for the wrong reason — but **the exact threshold value 2.0 is still pinned only by the constant, not by a test.** If a reviewer wants the boundary itself covered, the honest way is a pure test on a hand-built `ScoreBreakdown` — which would require extracting the depth choice from `placeConfirmed` into a static helper. That refactor is NOT in this plan; flag it rather than fake it.
   **Rev 3 adds a caveat worth stating out loud:** `kRampStartAmp` is derived from `NoiseSource`'s amplitude and the Hann window's `Σw` / `√(Σw²)`, i.e. from a first-order estimate of two FFT magnitudes, not measured. The derivation fixes the SHAPE of the fixture (no onset step ⇒ rise is always tone-vs-tone) and that part is structural. The exact number is a starting point to be settled by running the test, and the failure messages on both `riseRatio` assertions say which direction to move it.

2. **§5.3, "Slot tái dùng (B-2): … frame reinforce kế KHÔNG Reclamp".** Task 4's `ReusingASlotResetsEveryLadderField` pins the proximate cause (all five fields re-initialised, `releasedSteps == 0`), and Task 6's reclamp branch only fires on `releasedSteps > 0`. But no test drives the full sequence "deepen to −24 → Clear → manual −6 on the same index → one reinforce frame → assert no Reclamp". **Partially mapped only.**

3. **§5.4, "ảnh `console-live.png` gửi owner".** This is a human handoff, not something a test can assert. Task 10 Step 9 names the command and requires the image be READ back before sending, per CLAUDE.md — but nothing in the suite fails if it is skipped.

4. **§4.9, `kDepthStepDb = 6`.** Declared in Task 4 because the spec's constant table names it, but **no code in this plan reads it** — the step size is implicit in `kDepthLadderDb`'s spacing and in `nextDeeperRungDb`. It is documentation in constant form, and under Q13 it is now slightly misleading as well: the last step of an effective ladder can be smaller than 6 dB. A reviewer may reasonably ask for it to be deleted; the plan keeps it because the spec's table is the reference the release note quotes, and the header comment says what it does and does not mean.

5. **§5.3 / §4.4, "the ladder stops at the first rung that quiets the bin" (Q7, M-9).** **Not covered, and not coverable in this suite.** The harness writes the raw tone into `h.tap` (`tests/test_notchcontroller.cpp:344-349`); nothing puts the notch chain between the source and the Detector, so the analyser sees the un-notched spectrum every frame, the bin never goes quiet, and the ladder always reaches the ceiling. Every deepening test in Tasks 6 and 8 therefore asserts "climbs to the ceiling", which is the correct assertion *for this fixture* and says nothing about the rung the real system stops on. **This is the headline user-visible claim of the whole lane and it is verified only on a rig.** Task 6's level note and Task 10's tester notes both say so. The alternative — a source that subtracts a modelled notch from its own output — would test the model, not the loop, and would stay green while the real chain misbehaved; it is deliberately not in this plan.

6. **`NotchController::setSampleRate` still has no production caller.** Task 8 wipes room memory there anyway, for the tests and `tools/snapshot.cpp` that do call it, and because it is where the wipe belongs the day the device path reaches it. Carried over from lane R's parked item, not resolved here.

7. **Task 6's reclamp branch has no red test inside Task 6** (M-A, rev 3). The branch is written there because it lives in the reinforce loop, but the only state that reaches it — `releasedSteps > 0` — is produced by the release ladder, which is Task 7. `retuneForTest` cannot fake it: it forwards to `pushRetuneLocked`, which writes `depthDB` and nothing else. So Task 6's `ctest` run proves the deepen half only, and the reclamp is proven one task later by `AReturningHowlReclampsImmediatelyToDeepestDb`. **The alternative was a `retuneForTest` overload that sets `releasedSteps` directly — rejected**, because a seam that can fabricate that field is a seam that would keep passing if the release ladder stopped setting it. Stated here rather than hidden, so an executing agent does not "fix" Task 6 by adding one.

8. **`releasedSteps` has no test accessor.** `depthDbForTest` / `deepestDbForTest` / `quietMsForTest` / `activeForTest` exist; `releasedSteps` deliberately does not. Its whole observable meaning is "which threshold does the next release use", so the tests assert THAT (after a reclamp, 29 s of quiet must not release; at 30 s it must) instead of reading the counter. If a later task genuinely cannot express something that way, add the accessor then and say why — do not add it speculatively.

9. **§6, "sổ quyết định cập nhật nếu phản biện lật".** Nothing in this plan overturns Q1–Q13, so `docs/superpowers/decisions/2026-09-06-lane-g-gain-aware-notch.md` is left untouched — **Q13 itself came the other way**: the 2026-09-07 cross-check found the spec v2 quantisation making `presets/Music.json` 4 dB shallower than 1.1.3, the owner ruled, and the decision record and spec §4.1/§7 were updated before this plan was. If executing a task forces a further decision to change, that file must be updated in the same commit — it is the reason the spec says "đừng hỏi lại".

Also carried forward unchanged, as the spec's §7 requires: `PresetManager`'s `notchDefaults.depthDB` default of −12 (`PresetManager.h:146`) still disagrees with `NotchController::kDefaultNotchDepthDb` of −18 (`NotchController.h:95`). Task 9 makes `savePreset` write `notchDefaults`, which stops the disagreement mattering for a file this app SAVED, but a preset written by hand with no `notchDefaults` block still silently gets a −12 ceiling. **Not lane G's to fix (M-3); recorded in Task 10's `docs/KY-THUAT-CHONG-HU.md` §4 edit.** Lane R's parked items (I-3, `setSampleRate` having no production caller, A-R7) also remain open — the owner said not to fold them in.

### 3. API names used

**VERIFIED to exist** (read in the worktree on 2026-09-07, at these lines):

| Name | Where |
|---|---|
| `Biquad::setNotchFilter(double,double,double,double)` | `src/dsp/Biquad.h:102`, `.cpp:72` |
| `Biquad::processSample`, `Biquad::reset` | `src/dsp/Biquad.h:103-104`, `.cpp:130,139` |
| `Biquad` members `b0_ b1_ b2_ a1_ a2_ z1_ z2_` | `src/dsp/Biquad.h:108-112` |
| `NotchChain::setNotch`, `clearNotch`, `getNotchInfo`, `NotchInfo`, `NotchState`, `sampleRate_`, `filters_`, `notchInfo_` | `src/dsp/NotchChain.h:41-80`, `.cpp:25,55,129` |
| `CandidateScorer::ScoreBreakdown`, `scoreCandidateDetailed`, `kConfirmScore`, `kDefaultRiseReferenceMs` | `src/dsp/CandidateScorer.h:82-93,49,39`; rise branch `.cpp:76-110` |
| `NotchController::Origin`, `ClearReason`, `kAutoReleaseMs`, `kDefaultNotchDepthDb`, `kChannels`, `kSlots`, `kTotalSlots` | `src/app/NotchController.h:60,65-68,83,95,70-72` |
| `NotchController::ModelNotch`, `slotOf`, `model_`, `outbox_`, `modelMutex_`, `snapshotMutex_`, `latest_`, `liveMs_`, `width_`, `lanes_`, `notchQ_`, `notchDepthDb_` | `src/app/NotchController.h:320-331,453-455,481-482,474,390,415,447-448` |
| `NotchController::frameMaxScore_`, `frameScoreValid_` | `src/app/NotchController.h:433-434` |
| `NotchController::setNotchImpl`, `pushClearLocked`, `pushEventLocked`, `placeConfirmed`, `processSpectrumForDetection`, `firstFreeIndexLocked`, `firstFreeIndexAllLanesLocked`, `PlacementContext` | `src/app/NotchController.h:334-376`; `.cpp:135,195,183,580,702,557,565` |
| `NotchController::runOnce` step 2 `dt`, step 3 auto-release, reinforce loop | `src/app/NotchController.cpp:377-379, 403-416, 737-763` |
| `NotchController::SnapshotNotch`, `SnapshotBuffer`, `copySnapshot`, `ringRiskValid/Score/Threshold` | `src/app/NotchController.h:268-308` |
| `NotchController::setWidth`, `clearAll`, `setSampleRate`, `adoptPreset`, `setNotchDefaults`, `getNotchQ`, `getNotchDepthDb`, `effectiveLinked`, `liveMsForTest` | `.cpp:80,223,864,231,463,472,477`; `.h:142`; `.cpp:423` |
| `NotchCommand`, `NotchCommandType::Set/Clear` | `src/dsp/NotchCommand.h:5-21` |
| `AudioEngine::drain` Set/Clear dispatch | `src/app/AudioEngine.cpp:430-450` |
| `MainComponent::notchEventToVar`, `savePreset`, `originName`, `reasonName` | `src/app/MainComponent.cpp:559,909,54,66`; `.h:103` |
| `SessionLogger::makeEvent` | `src/app/SessionLogger.h:50`, `.cpp:25` |
| `PresetNotch`, `PresetNotchDefaults` (Q 30 / depth −12), `Preset::notchDefaults` | `src/app/PresetManager.h:103,143-147,160` |
| `gui::SpectrumView::kRingRiskRisingFraction` = 0.55f, `riskForScore` | `src/gui/SpectrumView.h:239,258` |
| test fixture: `FakeClock`, `Harness`, `SlotHarness`, `StereoHarness`, `SineSource`, `NoiseSource`, `pump`, `pumpStereo`, `primeAndPlace`, `anyCandidateInSnapshot`, `warmThenDrive`, `Recorder`, `Ev`, `kBlockMs`, `kTestSr`, `kTestPi`, `kWarmupBlocks` | `tests/test_notchcontroller.cpp:12,20,28,39,310,329,344,351,384,362,945,1278` |
| test fixture: `sineWave`, `rms` | `tests/test_biquad.cpp:25,38` and `tests/test_notchchain.cpp:20,31` |
| test fixture: `Rig`, `makeToneInNoise`, `kHop`, `kFrameMs`, `kSampleRate` | `tests/test_candidatescorer.cpp:72,36,33,34,31` |
| the `-18` assert this plan corrects | `tests/test_notchcontroller.cpp:438` |
| `logstats.py` dispatch (an `if/elif` chain with no `else`, so unknown `ev` names are IGNORED, not miscounted — m-7), `--expect-*` flags, `encoding="utf-8"` | `tools/logstats.py:45-63, 124-127, 19` |
| `logstats_fixture` ctest entry | `tests/CMakeLists.txt:113-117` (inside `if(Python3_Interpreter_FOUND)` at 112) |

**VERIFIED by the 2026-09-07 cross-check, and the reason six of its findings exist:**

| Name / fact | Where |
|---|---|
| `pushClearLocked` lowers only `active`; the Clear event reads `n.depthDB` | `src/app/NotchController.cpp:195-212`, depth read at `:208` (B-3) |
| `adoptPreset`'s `firstLane >= width_` skip, above every `setNotch` | `src/app/NotchController.cpp:242-246` (M-5) |
| `placeConfirmed`: the three `const` reads, then the index search | `src/app/NotchController.cpp:585-587`, `:589-596` (B-2) |
| the snapshot publish block, incl. `ringRisk*` at `:368-370` and `++latest_.sequence` at `:371` — NOT where `releaseFrozen` goes | `src/app/NotchController.cpp:337-372` (M-6) |
| `rNorm = (rise − 1) / 0.5`, clamped; reference frame `>= 0.45 × riseReferenceMs` old | `src/dsp/CandidateScorer.cpp:83, 104-106`; `kDefaultRiseReferenceMs = 250.0` at `CandidateScorer.h:39` (B-5) |
| fixture order: `SineSource` `:310-327`, `NoiseSource` `:329-342`, `pump` `:344-349`, `kWarmupBlocks` `:308` | `tests/test_notchcontroller.cpp` (B-6) |
| the armed detector keeps placing fresh notches on a ringing lane | documented in-place at `tests/test_notchcontroller.cpp:1101-1102` (M-2) |
| `pump` writes the RAW tone into `h.tap`; no notch chain in the path | `tests/test_notchcontroller.cpp:344-349` (M-3) |
| the stereo auto-release tests use `kAutoReleaseMs / kBlockMs + 20`; only `EveryClearPathCarriesItsReason` uses `+ 10` | `tests/test_notchcontroller.cpp:1106, 1133` vs `:1325` (m-6) |
| `AdoptedPresetsAutoReleaseLikeDetectorNotches` adopts −12 and pumps 7000 × 5 ms = 35 s | `tests/test_notchcontroller.cpp:267-291` — the existing test the v1 plan missed |
| `test_gui_wiring.cpp`'s real savePreset shape: `ScopedJuceInitialiser_GUI`, `getNotchControllerForTest(0)`, explicit temp `juce::File`, `getAudioEngine().getTapBuffer(0).write(...)` + `runOnce()`; the preset notch is adopted at −9.0 | `tests/test_gui_wiring.cpp:1016-1041`, notch depth at `:1030` (M-1, B-1) |
| `MainComponent::getNotchControllerForTest` public at `:188`; `savePreset` at `:103`; `notchEventToVar` private static at `:256` | `src/app/MainComponent.h` (m-4) |
| `originName` / `reasonName` are FREE functions in an anonymous namespace | `src/app/MainComponent.cpp:54, 66` (m-5) |
| `NotchDefaultsSurviveTheRoundTrip` starts at line **496** | `tests/test_presetmanager.cpp:496` (m-2 — the cross-check said 497) |

**INTRODUCED by this plan** (nothing above defines them today):

`Biquad::rampNotchDepth`, `Biquad::designPeaking`, `Biquad::State`, `Biquad::Coeffs`, `Biquad::stateForTest`, `Biquad::coeffsForTest`, `Biquad::rampRemainingForTest`, `Biquad::target_/delta_/rampRemaining_`; `NotchChain::kRampMs`; `CandidateScorer::ScoreBreakdown::riseRatio`; `NotchController::kDepthLadderDb`, `kDepthLadderSize`, `kDepthStepDb`, `kMaxDepthDb`, `kDeepenAfterMs`, `kSteepRiseRatio`, `kReleaseFirstMs`, `kReleaseStepMs`, `kMemoryTtlMs`, `kMemoryEntriesPerLane`, `kRiskFreezeFraction`, `RetuneReason`, `NotchEvent::Kind::Retune`, `NotchEvent::retuneReason`, `NotchEvent::fromDepthDb`, `NotchEvent::riseRatio`, `nextDeeperRungDb` (two-arg), `nextShallowerRungDb`, `ceilingDbFor`, `pushRetuneLocked`, `MemoryEntry`, `rememberReleaseLocked`, `takeRememberedDepthLocked`, `clearRoomMemoryLocked`, `roomMemory_`, `roomMemoryHead_`, `ringRiskOverrideForTest_`, `depthDbForTest`, `deepestDbForTest`, `quietMsForTest`, `activeForTest`, `retuneForTest`, `setRingRiskOverrideForTest`, `SnapshotNotch::deepestDb`, `SnapshotBuffer::releaseFrozen`, `ModelNotch::{deepestDb, stageChangedAtMs, quietMs, releasedSteps, ceilingDb}`; the free `retuneReasonName` in `MainComponent.cpp`'s anonymous namespace, `MainComponent::notchEventToVarForTest`; test fixture `kRampStartAmp`, `RampSineSource`, `primeAndPlaceSlowly`, `pumpQuietFor`, `firstActiveIndex`, `magnitudeAt`, `noBoostProbeFrequencies`; `logstats.py` `--expect-retunes` and the `notch_retune` branch.

**REDEFINED, not introduced:** `gui::SpectrumView::kRingRiskRisingFraction` already exists at `src/gui/SpectrumView.h:239` as a `0.55f` literal; Task 7 Step 3 changes only its initialiser to `NotchController::kRiskFreezeFraction` (m-D). The name, type and value are unchanged, so nothing that reads it needs touching — but it IS a header edit, so Task 7 reconfigures.

**WITHDRAWN by rev 2** (named in v1, not introduced by this plan any more): `NotchController::ceilingRungDb` — deleted with its test, replaced by the two-arg `nextDeeperRungDb` (Q13); `MainComponent::retuneReasonName` as a member — it is a free function (m-5); the one-arg `nextDeeperRungDb`.

**Names the v1 plan used without verifying, and which do NOT exist:** `TempDir`, `pumpOneBlockThroughSlotZero`, `notchControllerForTest`. The v1 self-review flagged them as unverified and the flag was not acted on; Task 9 now uses the real shape from `tests/test_gui_wiring.cpp:1016-1041` and names no helper that a grep of the repo does not find (M-1). **There are no unverified names left in this plan.**

### 4. Where the cross-checks themselves were wrong

Recorded so the next reader does not "re-fix" these back.

**Rev 3's own verification, corrected while applying it:**

- The B-5 finding said the 150-block cap leaves the ramp at `A ≈ 0.6`. It does not:
  150 blocks × 10.667 ms × 0.038 dB/ms = **60.8 dB**, and `3e-4 × 10^(60.8/20) ≈ 0.33`.
  The plan states 0.33. The finding's other two amplitude figures check out
  (`A ≈ 1.9` at 2 s ⇒ clipping; `A ≈ 0.024` at the ~1.0 s confirm).
- The B-5 finding said the confirm lands "at block ~90–110" without saying what
  eligibility costs. Two separate gates apply and the plan now names both:
  `peakiness > 10` before the scorer will score the candidate at all
  (`CandidateScorer.cpp:51`, ≈ block 49), and `peakiness > 73` for `pNorm > 0.7`
  (≈ block 93, +3 for persistence). The first one is why `riseRatio` is never
  read while the reference frame is still noise — a stronger argument for the fix
  than the one the finding gave.
- The M-A finding offered "extend `retuneForTest` to accept `releasedSteps`" as an
  alternative. The plan **moves the test to Task 7** instead and records why in
  §2 item 7; the seam is not added.

**Rev 2's cross-check, as recorded at the time:**

- **m-1**: the cross-check said `add_test(NAME logstats_fixture …)` is at "112-117 (not 113-117)". `grep -n` in this worktree puts `add_test(NAME logstats_fixture` on line **113**; line 112 is the `if(Python3_Interpreter_FOUND)` that opens the block. The plan keeps **113-117** and notes the enclosing `if`.
- **m-2**: the cross-check said `NotchDefaultsSurviveTheRoundTrip` "starts at 497". `grep -n` puts it at **496**. The plan uses 496.
- **m-3**: the cross-check said the `--expect-*` flags are "at 128-131". They are at **124-127** (`--expect-notches` 124 … `--expect-recurrence-max` 127), which is what the plan already said and still says.

Everything else the cross-check reported was reproduced against the files before being applied.

### 5. Type consistency

Checked across tasks: `deepestDb` (not `deepestDB`) everywhere — `ModelNotch::deepestDb`, `SnapshotNotch::deepestDb`, `deepestDbForTest`, and the log key `deepest_db` in `logstats.py` only. `depthDB` keeps its existing capitalisation everywhere it already exists (`NotchInfo::depthDB`, `ModelNotch::depthDB`, `NotchCommand::depthDB`, `SnapshotNotch::depthDB`, `PresetNotch::depthDB`) and the new accessor is `depthDbForTest` to match the file's existing `getNotchDepthDb` style. `RetuneReason` values are `Deepen/Release/Reclamp/Ceiling` in C++ and `deepen/release/reclamp/ceiling` in JSON, in that one mapping, in `retuneReasonName`. `kRampMs` lives on `NotchChain` only; `kDeepenAfterMs`, `kReleaseFirstMs`, `kReleaseStepMs`, `kMemoryTtlMs` on `NotchController` only. `kRiskFreezeFraction` is declared on `NotchController` and only ALIASED by `gui::SpectrumView::kRingRiskRisingFraction` (m-D) — there is no second literal. `kRampStartAmp` is a `float` in the test fixture's anonymous namespace, never a production constant. `std::max` is used for every ceiling clamp in both directions of the code (placement step 4, the reclamp target, the per-tick `deepestDb` clamp) and always with the same meaning: pick the SHALLOWER, because deeper is more negative. `nextDeeperRungDb (currentDb, ceilingDb)` and `nextShallowerRungDb (currentDb)` are used with those exact spellings and arities in Tasks 4, 6 and 7; `ceilingDbFor (n)` is the only way any task obtains a ceiling, and no task calls a quantiser, because there is none.
