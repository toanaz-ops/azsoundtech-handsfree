### Task 12: release 1.3.0 to the alpha testers — **BLOCKED on the nine owner confirmations**

**Mức level dự kiến:** everything in spec §3 reaches a real PA. This is the point of the gate.

- [ ] **Step 0: The gate. Do not skip it, and do not answer it yourself.**

Open `docs/superpowers/decisions/2026-09-15-lane-m-active-soundcheck.md` and check that **each of the nine items** listed at the top of this plan has an owner answer recorded as a **new** `Q<n> (lật lại <ngày>)` entry. Old entries are never edited — that is the `recording-design-decisions` rule and the decision file's own closing note says so.

If any answer is missing, **stop here and report which ones**. An agent's own judgement is not the owner's approval, and neither is a coordinator's. This is a signal going into a live PA.

If an answer comes back NO, this is the task it lands on:

| Item | If the answer is NO |
|---|---|
| Q2 (level) | Task 1: `kSoundcheckMaxPeak`, plus every level figure in Tasks 5, 9, 11 |
| Q15 (mute the whole channel) | Tasks 5 and 6 — the mute rule and the duration; and spec §7 risk 3 becomes live, since Q15 PA 3 (mute EVERY output) removes both the cross-lane and the other-channel loops |
| Q6 (propose vs place) | Tasks 7 and 9: an automatic apply is a different flow and a different set of GUI states |
| Q3 (the abort set) | Task 6 |
| Q16 (a separate `ĐO`) | Task 9 |
| Q7 + Q9 (preset asymmetry) | Task 11 at minimum; making preventive notches survive a reload would change `Origin` handling in `NotchController`, which is outside this plan's two additive changes |
| `kResultsTimeoutMs` | Task 6 |
| `ClearReason::SoundcheckReplace` | Task 4, and Task 7's replace pass loses its label |
| `Origin origin` on `SnapshotNotch` | Task 4, and Task 7's replace pass becomes **impossible** — §4.6 has no fallback |

- [ ] **Step 1: Release**

CLAUDE.md's standing instruction: a finished change ships. This one is a minor version.

```bash
pwsh -File installer\release-alpha.ps1 -Part minor
```

The script bumps the PATCH/MINOR in `CMakeLists.txt`, reconfigures, builds Release, runs `ctest` **as a gate**, packages with NSIS and copies the installer to `Z:\My Drive\RELEASE\ALPHA TEST`, then prunes the drop folder to the newest three builds. **A red suite publishes nothing and rolls the version back.** Never pass `/DPRODUCT_VERSION`: `handsfree.nsi` reads the version out of the `project(HandsFree VERSION x.y.z)` line with `!searchparse`, and a build stamped with a number the source does not carry is untraceable.

- [ ] **Step 2: Fill in the installer's SHA-256 and size, then commit the bump**

```bash
git add CMakeLists.txt installer/TESTER-NOTES.md docs/release-notes/1.3.0-alpha.md
```
```bash
git commit -m "chore: release 1.3.0 to alpha (lane M active soundcheck)"
```

The version bump **is a source change** — commit `CMakeLists.txt` or the next run bumps from the same number again.

- [ ] **Step 3: Close the phase in public (global rule 12)**

Three things, before moving on:

1. **The roadmap's lane M rows say so** (Task 11 Step 3), and every statement this lane made false is fixed. A phase still marked "mở" that shipped last week is not a stale note, it is a lie the next session will act on.
2. **Name what the owner can run, and what they should SEE**: the installer in `Z:\My Drive\RELEASE\ALPHA TEST`, the `ĐO` button, `shots/console-soundcheck-results.png`, and `python tools/logstats.py <session>.jsonl` on a log from a real run.
3. **Write the handoff for lane A now**, while the context is still here: `D:\DEV CAVE EP3\shared\handoff\handoff-<YYYYMMDD>-lane-a-afc.md`, per `D:\DEV CAVE EP3\shared\README.md`. It must state the baseline acceptance numbers, what is done vs in progress vs waiting on a human, and the known pitfalls — including, explicitly, that **lane M does NOT supply round-trip delay** (spec §4.4, §6): lane A has to measure it itself or open the deconvolution branch Q5 PA 3 describes.

---

## Người đọc thứ hai: soundcheck đo chủ động làm gì trên sân khấu (1.3.0)

Cho tới 1.2.0, app **hoàn toàn thụ động**: nó chỉ ngồi nghe tín hiệu người khác
đưa vào và trừ gain đi khi phát hiện hú. Nghĩa là **notch đầu tiên luôn tới sau
tiếng hú đầu tiên** — khán phòng nghe khoảng 0,3–0,9 giây tiếng hú trước khi
filter đủ sâu. Lane G làm phần đó *nông hơn*. Lane M là lane duy nhất có thể làm
cho nó *không xảy ra*.

**Cách nó làm.** Bấm nút `ĐO`. App chọn **từng kênh ngõ ra một**, và với mỗi
kênh:

1. **Tắt tiếng kênh đó hoàn toàn 0,5 giây** và nghe xem phòng đang ồn cỡ nào —
   đó là "nền nhiễu". Tắt **cả kênh**, không phải một đường: nhiều slot cộng dồn
   lên cùng một kênh ngõ ra, nên tắt một đường thôi thì vòng hú của kênh đó vẫn
   đóng.
2. **Phát một tiếng "vút" 3 giây** đi từ 100 Hz lên 10 kHz, ở mức 20 dB dưới
   toàn thang, vào đúng kênh ấy và không kênh nào khác.
3. **Nghe tiếp 0,7 giây** để bắt phần ngân của phòng.
4. So tín hiệu thu được với tín hiệu đã phát, **theo từng tần số**. Tỉ số đó
   chính là **độ lợi vòng**: nó nói ở tần số nào tiếng từ loa quay lại mic mạnh
   tới mức nào.

Một tần số có độ lợi vòng **bằng hoặc hơn 0 dB là một tần số sẽ hú**. Còn thiếu
bao nhiêu dB nữa mới hú thì gọi là **margin**. App vẽ đường margin lên analyser,
đánh dấu những chỗ sát nhất, và **đề xuất** notch ở đó — sâu −6, −12, −18 hoặc
−24 dB, đúng thang lane G đang dùng, và không bao giờ sâu hơn trần của preset.

**App không tự đặt gì.** Đề xuất nằm đó chờ; bấm `ÁP DỤNG` thì mới có filter
thật, bấm `BỎ` hoặc để yên 20 giây thì bỏ hết. Đây là chỗ lệch với chỉ thị ban
đầu của chủ dự án (prompt viết "đặt"), và là một trong chín mục chủ dự án phải
chốt.

**Phải biết trước ba điều, vì chúng đổi cách làm việc chứ không chỉ thêm tính
năng:**

- **Kênh đang đo im 4,5 giây.** Bấm `ĐO` giữa lúc MC đang nói thì kênh đó mất
  tiếng 4,5 giây. Hệ 8 slot stereo = 16 kênh ⇒ **khoảng 72 giây**. Hộp thoại xác
  nhận in đúng con số ấy trước khi bạn bấm.
- **Vẫn có thể hú trong lúc đo — chỉ là không qua kênh đang đo.** Vòng hú đi qua
  kênh *đang đo* bị **mở** (kênh đó không mang tiếng mic nào). Vòng đi qua các
  kênh *khác* vẫn **đóng**: loa của chúng vẫn phát tiếng mic. Nếu phòng đang sát
  ngưỡng ở một kênh khác, lần quét **có thể** kích nó. Vì thế có nút `DỪNG`, có
  bộ tự hủy, và app **từ chối chạy** khi chip `RING RISK` đã ở `RISING` trở lên.
- **Hạ master trước.** App không biết SPL tuyệt đối và không có phép quy đổi nào
  đúng nếu không biết gain của bàn và amp. Câu trung thực duy nhất là: sweep phát
  ở **20 dB dưới toàn thang của hệ, tại vị trí master hiện tại của bạn**. Master
  mở hết thì 20 dB dưới toàn thang **vẫn rất to**. Mức phát chỉ chỉnh được
  **xuống**.

**Trên 6 kHz bản này chỉ vẽ, không đề xuất.** Tiếng "vút" chạy đều theo octave,
nên năng lượng nó gửi vào mỗi vạch tần số ở 10 kHz thấp hơn ở 100 Hz đúng 20 dB —
chưa đủ tin để đặt notch. Dải 6–10 kHz vẫn được vẽ, có nhãn "độ tin cậy thấp".
Người đo trên dàn thật cho biết có đáng nới không.

**Preset lưu sau khi đo thì hơi khác một chút.** File **có** mang notch phòng
ngừa, nhưng khi nạp lại chúng vào model như **notch preset bình thường**, nên
**sẽ tự nhả sau 30 giây yên tĩnh**. Muốn bảo vệ phòng ngừa quay lại đầy đủ thì
chạy lại `ĐO`.

---

## Self-review

Run against spec rev 4 with fresh eyes after the plan was written, then **re-run
on 2026-09-15 against the real files by an independent read-only session** —
exactly the round spec §8 asked for: *"the next round should not be another read
of the spec: it should be someone checking the PLAN against the real code before
dispatch."* That pass found **10 BLOCKER / 12 IMPORTANT / 22 MINOR**, two of them
production defects; the "Revision 2" table at the top of this plan is its record.
This section reflects the plan **after** it.

The honest summary of what that round proves: **rev 1's self-review named four
unverified helpers and that was not enough.** Flagging a name as unverified and
dispatching anyway is the same failure lane G's M-1 recorded, one lane later.
Everything in §3 below now says where it was read, and §3's last block lists what
is still unread — which, after the cross-check, is nothing but `src/app/SlotConfig.h`.

### 1. Spec coverage

| Spec § | Requirement | Task |
|---|---|---|
| §3 | the level table, stated per task | every audio-path task's `Mức level dự kiến` line; §3's rows are quoted into 5, 6, 7, 11, 12 |
| §3 | the safety-mechanism table (clamp at source and at use, tail clamp, ramps, `DỪNG`, self-abort, refusals, detection off, bounds check, safe-dead defaults) | 1 (clamps, ramps), 5 (tail clamp, bounds check, safe defaults), 6 (aborts, refusals, detection off), 9 (`DỪNG`) |
| §3 | the worst-case stop-latency table | 5 (`AbortRampsDownInTheCallbackAlone`), 11 (tester notes), 12 (owner gate item 4) |
| §4.1 | the seven atomics, `micCapture_`, `micCaptureDrops_`, `kCaptureCapacity` | 5 |
| §4.1 | snapshot-once beside `bypass` (inv 7) | 5 (`AtomicsAreSnapshottedOnce`) |
| §4.1 | per-callback bounds check (inv 3) | 5 (`OutOfRangeChannelIsIgnored`) |
| §4.1 | the four insertion points in order — point 1 hoisted out of the lane loop (B-6) | 5 |
| §4.1 | tap suspension keyed on `scSuspendTaps_`, held across Gaps, ≤ ~420 ms drift (inv 10, 10b) | 5 (`TapsStaySuspendedAcrossTheGap`) |
| §4.1 | the snapshot is frozen during a run ⇒ the overlay uses lane M's own data; `index` is read only after the taps resume | 7 (re-read before each `setNotch`), 9 (`SoundcheckOverlayCarriesLaneMData`) |
| §4.2 | `SoundcheckSignal`: closed-form log sweep, `n < 0` ⇒ 0, raised-cosine window, separate `rampOut` | 1 |
| §4.3 | the state machine, phase by phase | 6 |
| §4.3 | `RunParams`, read once at `Arm`, immutable | 6 (`NoiseFloorGateIsReadAtArm`, `NoiseFloorGateIsStableWithinARun`) |
| §4.3 | `Results` runs with detection restored; `kResultsTimeoutMs = 20 s` | 6 (`DetectionIsRestoredBeforeResults`, `ResultsTimeoutIsTwentySeconds`) |
| §4.3 | ring risk read at `Preflight` and `Arm` only; the RISING identity; `ringRiskValid == false` does not refuse | 6 (`RefusesWhenRingRiskIsRising`, three branches) |
| §4.3 | the noise-floor gate against the LIVE peakiness threshold | 6 (three tests: quiet noise does not abort, a tone does, the gate follows the threshold) |
| §4.3 | mic RMS self-abort with a hold | 6 (`HotMicAbortsOnlyAfterTheHold`) |
| §4.3 | device restart: abort + join before the `NotchController`s; `micCapture_.clear()`; record and re-check rate + channel counts | 5 (`DeviceRestartDrainsTheCaptureRing`), 6 (`SampleRateChangeAborts`, `ChannelCountChangeAborts`), 10 (`DeviceRestartAbortsAndJoinsTheSoundcheckFirst`) |
| §4.3 | `Preflight` validates the specific channel pair (F26) | 6 (`RefusesOnInvalidChannelPair`) |
| §4.4 | the four formulas, the no-delay-compensation argument and its boundary | 2 (`FlatRoomMeasuresFlatResponse`, `DelayDoesNotChangeTheAnswer` at 5 / 900 ms, `DecayLongerThanTheTailIsUnderRead`) |
| §4.4 | trusted band narrower than swept band, 6–10 kHz drawn not proposed | 2 (`AboveTrustedHighHzIsDrawnButNeverTrusted`), 3 (`UntrustedBinIsNeverACandidate`), 9 (the dimmed band), 11 (tester notes) |
| §4.4 | the seven pick steps | 3 |
| §4.4 | the depth rule and all six worked examples | 3 (seven depth tests, each an exact number) |
| §4.4 | saturation from BOTH causes, with `residualDb` | 3 (`SaturatesAtMinusTwentyFourAndReportsResidual`, `SaturationByTheCeilingIsAlsoReported`) |
| §4.4 | the mark/propose split | 3 (`MarkedButNotProposedBelowMinUsefulCut`) |
| §4.5 | `OutputResult`, including `measured` vs `routingInvalid` | 6 (`UnmeasurableChannelIsAValidResultNotAFlatLine`), 9 (two separate sentences) |
| §4.6 | `Origin origin` on `SnapshotNotch` | 4 (`SnapshotCarriesOrigin`) |
| §4.6a | re-read before each `setNotch`, top-down allocation, the residual race acknowledged | 7 (`ApplyReReadsTheSnapshotBeforeEachSetNotch`) |
| §4.6b | clear the previous run with `SoundcheckReplace` | 4 (the enumerator), 7 (`ARerunReplacesItsOwnPreviousProposals`, `ReplacingLeavesEveryOtherOriginAlone`) |
| §4.6c | LINKED: derived from `laneCount`, one index both lanes, all-or-nothing; INDEP stops without rollback | 7 (four tests) |
| §4.6d | a preventive notch is its own ceiling | 7 (`APreventiveNotchIsItsOwnCeiling`) |
| §4.6e | apply on the message thread; the gate handed in via `RunParams` | 7 (`ApplyRunsOnTheMessageThread`), 6 (`RunParams`), 10 (`RestoreDetectionCallbackOnlyTouchesAtomics`) |
| §4.6f | room memory untouched | 6 (`RoomMemoryIsUntouched`) |
| §4.7 | five events, `ev` as the dispatch key, 3 s.f. rounding, `ring_risk: null` | 8 |
| §4.7 | logstats needs NO branch for the five names; the real work is the clear reason | 4 (the reason branch + fixture), 8 (Step 3 proves the totals do not move) |
| §4.8 | the preset asymmetry, in BOTH docs, in the same change | 11 (Steps 1 and 2) |
| §4.9 | `ĐO` as its own button (Q16) | 9 |
| §4.9 | lane M's own countdown, not `getSoundcheckRemainingMs` | 9 |
| §4.9 | overlay, `DỪNG`, focus, `Esc` best-effort | 9 |
| §4.9 | controls locked, `PRESET LOAD` locked in `Results` too | 10 |
| §4.9 | margin curve + markers + dimmed band; two sentences for unmeasured vs mis-routed | 9 |
| §4.9 | a rendered screenshot, read back | 9 Step 6, 10 Step 4 |
| §4.10 | all 22 constants, and `noiseFloorGate` kept OUT of the constant table | Global Constraints table; 1, 2, 3, 6 define them; 6 aliases them |
| §4.11 inv 1–20 | see the invariant list in Global Constraints | 1 (1, 2, 9), 5 (3, 4, 5, 6, 7, 8, 10, 10b), 6 (11, 12, 18, 19, 20), 7 (13, 14, 15, 17), 3 (14, 15) |
| §4.11 inv 16 | **review-enforced, not test-enforced** | 5 Step 10, and the reviewer brief of every `AudioEngine` task |
| §5.1 | every named test | 1–10, listed in the task list below |
| §5.2 | the reviewer's one-line checklist | 5 Step 10 |
| §5.3 | the rig-only list | 11 Step 6 (tester notes), in the spec's own priority order |
| §6 | the two "CHƯA" items from other lanes | 4 |
| §7 | the eleven risks | risks 1, 2, 3, 7, 8 are stated in the tasks that carry them; 4, 5, 6, 9, 10, 11 are in Task 11's docs and tester notes |
| §8 | the deferred item (pre-emphasis) | 11 Step 6, as a question for the first rig measurement — not implemented |

### 2. Spec requirements I could NOT map to a task

Stated explicitly, as the skill requires.

1. **Invariant 16 has no test and cannot have one.** "No lock, no allocation, no logging in the callback" is a property of the source text, not observable behaviour. Spec §4.11 says so outright and §5.2 makes it a reviewer line. Task 5 Step 10 is that read, and it is required in the reviewer brief of every task that touches `AudioEngine`. **Nothing in the suite fails if it is skipped** — say so rather than adding a test that pretends to cover it.

2. **The residual `index` race is narrowed, not closed** (§4.6a, §7 risk 8). Between the snapshot read and the `setNotch` there is a microsecond window in which the detector can take the slot and be silently overwritten (`src/app/NotchController.cpp:226-245` never checks `n.active`). The detector's cadence is ≥ 300 ms so the probability is tiny, but it is not zero, and **no test in this plan can produce it deterministically**. Closing it needs a second write API, which Q7 forbids. Documented, not faked.

3. **"Is the routing physically right?" is not checkable** (§7 risk 1). `Preflight` catches an **invalid** pair; it cannot catch a **valid pair wired to the wrong loudspeaker**. The SNR gate hides part of it and does not hide "the other loudspeaker is also loud enough". Rig only.

4. **Cross-lane loops are not measured** (§2, §7 risk 2). Out → L, mic → R is a real loop and v1 is blind to it. No test; stated in the docs.

5. **Whether preventive notches raise gain-before-feedback** (§5.3 item 3) is the headline claim of the lane and **only a rig can produce the number**. Task 11's tester notes put it first in the report list. No synthetic test can stand in: the synthetic room in Task 2 would be testing the model, not the loop.

6. **The `~420 ms` and `~0.72 s / 11.5 s` figures are derived, not measured.** They come from `kTapSilenceTimeoutMs = 250 ms` (`src/app/NotchController.h:82`) plus `kTapCapacity = 8192` ≈ 170 ms at 48 kHz (`src/app/AudioEngine.h:275`). `TapsStaySuspendedAcrossTheGap` pins the **behaviour** (no tap during a Gap); the **milliseconds** are arithmetic in a comment. If a reviewer wants the number itself pinned, that is a `liveMsForTest()` delta test across a full simulated run — not in this plan, flagged rather than faked.

7. **`kSoundcheckMinPeak = 0.01f` is declared and never read.** Spec §4.10 lists it (Q2: "the level can only be adjusted DOWN"), but v1 exposes no level control at all, so nothing clamps against it. It is documentation in constant form. A reviewer may reasonably ask for it to be deleted; the plan keeps it because §4.10's table is the reference the release note quotes. This is precisely lane G's `kDepthStepDb` situation.

8. **`NotchController::setSampleRate` still has no production caller.** Carried over from lane R and lane G unresolved; lane M does not touch it and does not fix it.

9. **§4.10's literal wording, "one place, `SoundcheckController.h`", is not followed literally.** The constants are defined on the class that computes with them and **aliased** in `SoundcheckController.h`, because `src/dsp/` may not include `src/app/` and `AudioEngine.cpp` cannot include `app/SoundcheckController.h`. One definition, no second literal, one file to look them up in — but a reviewer comparing the plan against §4.10 word for word will see a difference, so it is declared in Global Constraints and Task 4 Step 6 greps for violations.

10. **The restore-detection callback is the one place this plan INTERPRETS the spec.** Invariant 17 says `SoundcheckController` never calls `NotchController`; invariant 12 says detection is restored *before* `Results`. A message-thread hop satisfies 17 and breaks 12's ordering. The plan's answer — an injected `std::function` whose body in `MainComponent` is one relaxed atomic store per slot (`src/app/NotchController.cpp:936-939`) — keeps inv 17's letter (no pointer is held) and inv 12's ordering. **This is the first thing a reviewer should challenge.** Task 10's `RestoreDetectionCallbackOnlyTouchesAtomics` pins the lambda's whole observable effect. **Rev 2 adds the half the cross-check found missing (I-10):** a bare public `std::function` invoked from another thread is a data race if it is ever reassigned, so the header now carries a lifetime contract — assigned once, in `MainComponent`'s constructor, before `start()`, and never again while the thread can run.

11. **The residual `index` race is still not closable, and rev 2 changed what protects against it.** B-1's `takenThisCall` bitmap removes the *self*-collision entirely — six proposals now land on six indices regardless of when the snapshot refreshes. What remains is only the cross-thread race with the detector, narrowed by the per-`setNotch` re-read and by top-down vs bottom-up allocation. **No test in this plan can produce it deterministically**, and closing it needs a second write API Q7 forbids.

12. **`SoundcheckCandidates::smooth` has no test of its own** (m-19). It is exercised through `pick` by `SpeakerRolloffIsNotACandidate` — a smoother that returned its input unchanged makes every prominence zero and turns that test red, which is the coverage that matters. Rev 1 claimed it was "exposed for the tests", which was not true; the header comment now says what it actually is.

13. **`firstFreeIndexTopDown` takes a `laneCount` it does not read.** The scan is driven by `lane < 0` ("free on every lane") versus a specific lane, and `laneCount` is kept in the signature so a future per-lane rule has somewhere to go. A reviewer may reasonably ask for it to be dropped; flagged rather than quietly left.

14. **"The logstats branch"** (§4.6b, F21) is real but thin. `reasonName` must gain a case — that is mandatory and tested. On the Python side, a reason is already echoed verbatim into the `cleared by` column (`tools/logstats.py:136`), so nothing *breaks* without a change; the plan adds a `soundcheck_replaced` tally and `--expect-soundcheck-replaced` so a **fixture can pin that the name survives the whole C++ → JSON → reader path**. That is a choice, not a spec requirement, and it is declared here.

### 3. API names used

**VERIFIED to exist** — read in this worktree on 2026-09-15, at these lines. Rows marked **(xc)** were corrected by the read-only cross-check after rev 1 cited them wrongly; every one of those was re-opened before being written here.

| Name | Where |
|---|---|
| `AudioEngine::audioDeviceIOCallbackWithContext` | `src/app/AudioEngine.h:240-245`, `.cpp:458` |
| `AudioEngine::audioDeviceAboutToStart`, the tap/command drain block | `src/app/AudioEngine.h:247`, `.cpp:686`, drain at `.cpp:729-738` |
| `kMaxOutputLevel = 1.0f`, applied | `src/app/AudioEngine.cpp:15`, `:631-647` (the `jlimit` at `:643`) |
| the `bypass` snapshot line and its "snapshotted ONCE" comment | `src/app/AudioEngine.cpp:509-514` |
| `struct LaneRef`, `lanes[]`, `tapSource[][]` | `src/app/AudioEngine.cpp:516`, `:520`, `:526`, assigned `:561` |
| the lane-loop bounds check | `src/app/AudioEngine.cpp:546-548`, `:555-556` |
| output clearing before accumulation; `out[n] +=` | `src/app/AudioEngine.cpp:565-577` (clear at `:575`), `:621` |
| the tap-write loop and `tapDropCounts_` | `src/app/AudioEngine.cpp:657-683` |
| `AudioEngine::kTapCapacity = 8192`, `kMaxSlots`, `kMaxSlotLanes` | `src/app/AudioEngine.h:275`; `src/app/SlotConfig.h` |
| `AudioEngine::getTapBuffer`, `getTapDropCount`, `getCommandQueue`, `getSlotConfig`, `setSlotConfig`, `isRunning`, `getCurrentSampleRateHz`, `getNumInputChannels`, `getNumOutputChannels`, `getLastDeviceError`, `setMode` | `src/app/AudioEngine.h:181-183`, `:194-196`, `:211-212`, `:220-221`, `:75`, `:142`, `:148-149`, `:155`, `:158` |
| `LockFreeRingBuffer::write / read / clear / getCapacity / getAvailableRead`, and `clear()`'s "no producer, no consumer" precondition | `src/dsp/LockFreeRingBuffer.h:54`, `:85`, `:145`, `:151`, **`:115`** (xc, m-8), precondition at `:131-141` |
| `Detector::kFftSize / kHopSize / kNumBins / kFftOrder` | `src/dsp/Detector.h:66-69` |
| `AudioEngine::isRunning()` is set ONLY by `start()`; `audioDeviceError()` clears it; `audioDeviceAboutToStart()` does not touch it — **(xc, B-4)** | `src/app/AudioEngine.cpp:86`, `:753`, `:686-739` |
| `numInputChannels_` / `numOutputChannels_` are written only from the callback, and a test already relies on that — **(xc, B-4)** | `src/app/AudioEngine.cpp:506-507`; `tests/test_audioengine.cpp:409-410` |
| `PeakinessAnalyzer::peakinessAt`, `kDefaultThreshold = 10.0f`, the rig figures 7.35 / 131.70 | `src/dsp/PeakinessAnalyzer.h:166`, `:124`, `:60-65` |
| `CandidateScorer::kConfirmScore = 0.7f` | `src/dsp/CandidateScorer.h:49` |
| `ClockSource`, `JuceMonotonicClock` | `src/dsp/ClockSource.h:8-13`, `:17-21` |
| `NotchController::Origin { Detector, Preset, Manual, Soundcheck }` | `src/app/NotchController.h:62` |
| `NotchController::ClearReason` (six values today) | `src/app/NotchController.h:67-70` |
| `kSoundcheckDurationMs`, `kDepthLadderDb`, `kMaxDepthDb`, `kRiskFreezeFraction` | `src/app/NotchController.h:91`, `:109`, `:114`, `:140` |
| `effectiveLinked()`, `analysedLanes()` | `src/app/NotchController.h:217`, `:655` |
| `setNotch`, `clearNotch`, and their "message thread" declaration | `src/app/NotchController.h:221`, `:224`, comment at `:219-220` |
| `failNextSetNotchOnLaneForTest` | `src/app/NotchController.h:298` |
| `depthDbForTest` `:303`, `deepestDbForTest` `:304`, `activeForTest` `:310`, `ceilingDbForTest` `:324`, `rawCeilingDbForTest` `:328`, `detectionActiveForTest` **`:388`** (xc, m-5) | `src/app/NotchController.h` |
| `NotchController::runOnce` **`:350`** (xc, m-6), `kSlots = 16` `:73`, `kTapSilenceTimeoutMs` **`:83`** (xc, m-3) | `src/app/NotchController.h` |
| `setDetectionActive` (one relaxed store) | `src/app/NotchController.h:340`, `.cpp:936-939` |
| `getSoundcheckRemainingMs` `.h:346` / **`.cpp:1030`** (xc, m-7), `startSoundcheck` `.cpp:1005-1011` | `src/app/NotchController.h`, `.cpp` |
| `getNotchQ`, `getNotchDepthDb`, `getPeakinessThreshold` | `src/app/NotchController.h:362-365` |
| `SnapshotNotch` (6 fields today) and `SnapshotBuffer` (`laneCount`, `linked`, `ringRiskScore/Valid/Threshold`, `releaseFrozen`) | `src/app/NotchController.h:394-406`, `:408-445` (`laneCount` `:412`, `linked` `:413`, `ringRiskThreshold` `:436`) |
| `copySnapshot` | `src/app/NotchController.h:447`, `.cpp:1547` |
| `setNotchImpl` and its five predicates, incl. `if (! (depth <= 0.0)) return false;` | `src/app/NotchController.cpp:195-256`, the depth refusal at `:214`, the ceiling ternary at `:243-245` |
| `pushClearLocked` (lowers only `active`) | `src/app/NotchController.cpp:270` |
| `adoptPreset`'s `PartialApplyUnwind` | `src/app/NotchController.cpp:528-530` |
| the snapshot fill site (the aggregate initialiser) | `src/app/NotchController.cpp:568-582`, the init at `:578-580`, `lastDataMs_` at `:572` |
| `latest_.laneCount` / `latest_.linked` and the "operator's own switch" comment | `src/app/NotchController.cpp:636`, `:637-640` |
| `latest_.ringRiskThreshold = CandidateScorer::kConfirmScore` | `src/app/NotchController.cpp:648` |
| `tapAlive = (nowPolled - lastDataMs_) < kTapSilenceTimeoutMs` | `src/app/NotchController.cpp:662`; the constant at `NotchController.h:82` |
| the auto-release Soundcheck exemption | `src/app/NotchController.cpp:729`, `:1322` |
| `firstFreeIndexLocked` (private, bottom-up) and `firstFreeIndexAllLanesLocked` | `src/app/NotchController.cpp:1064`, `:1066-1068`, `:1072-1083`, used `:1105` |
| the room-memory gates excluding Soundcheck — **both**: `:1116` stops a Soundcheck placement CONSUMING an entry, `:1169` stops a remembered depth DECIDING a Soundcheck depth. Both inside `placeConfirmed`, reachable only through a real detector placement — **(xc, I-6/m-4)** | `src/app/NotchController.cpp:1116`, `:1169` |
| `roomMemory_` is written on the auto-release path only | `src/app/NotchController.cpp:360-405` |
| `placeConfirmed`'s `PartialApplyUnwind` | `src/app/NotchController.cpp:1264` |
| `SessionLogger::makeEvent` | `src/app/SessionLogger.h:50`, `.cpp:25` |
| `MainComponent::notchEventToVar` and the `ev` ternary | `src/app/MainComponent.cpp:578-588` |
| `originName` `:54` / `reasonName` `:66` (its `PartialApplyUnwind` case at **`:75`**, xc m-2) / `retuneReasonName` `:84` — free functions, anonymous namespace | `src/app/MainComponent.cpp` |
| `MainComponent::applyModeGating`, `startSoundcheck` call, `loadPreset`, `savePreset` | `src/app/MainComponent.cpp:783`, `:802`, `:830`, `:1038` |
| `devicePanel_.onBeforeRestart` / `onAfterRestart` | `src/app/MainComponent.cpp:351`, `:358` |
| `modeRail_.onClearAllConfirmed` and `modeRail_.getSoundcheckRemainingMs` lambdas | `src/app/MainComponent.cpp:241`, `:247` |
| the two preset chooser lambdas | `src/app/MainComponent.cpp:258`, `:275` |
| `engine_.setSlotConfig` inside `changeSlotConfig` | `src/app/MainComponent.cpp:814` |
| `modeBar_` is never made visible | `src/app/MainComponent.cpp:211` (comment: "statusBar_ / modeBar_ stay alive but hidden") |
| `MainComponent::getNotchControllerForTest` `:188`, `getAudioEngine` `:67`, `loadPreset` `:89`, `showMessage` `:118`, `notchEventToVarForTest` `:194`, and the accessor shape the new ones copy: `getSlotPanelForTest` `:176`, `getSpectrumViewForTest` `:182` — **(xc, I-5)** | `src/app/MainComponent.h` |
| `panelMessage_`, written by `showMessage` — what `lastMessageForTest()` returns | `src/app/MainComponent.cpp:688-692` |
| `notchEventToVarForTest` is used from `tests/test_gui_wiring.cpp` and nowhere else; `makeClearEvent` **does not exist anywhere** — **(xc, B-3)** | `tests/test_gui_wiring.cpp:1337, 1360, 1365, 1383`; `SetAndClearKeepTheirOwnEventNames` at `:1356-1369` |
| `gui::ModeRail::soundcheckButton { "SOUNDCHECK" }` `:81`, `autoButton` `:82`, `bypassButton` `:83`, `clearAllButton` `:84`, `countdownLabel` `:85`, `onSoundcheck` `:45`, `setDisplayedMode` `:64`, `updateCountdown` `:74`, and **`enum class Orientation { Vertical, Horizontal }` `:23`** — **(xc, B-9)** | `src/gui/ModeRail.h` |
| the existing tests construct a rail with parentheses and the capitalised enumerator | `tests/test_moderail.cpp:28`, `:41` |
| `gui::ModeBar::soundcheckButton { "Run Soundcheck (15s)" }` | `src/gui/ModeBar.h:43` |
| `gui::SpectrumView::setDisplayLane` `:202`, `setController` `:113`, `refreshFromSnapshot` `:88`, `kMinHz/kMaxHz` `:64-65`, `kRingRiskRisingFraction = NotchController::kRiskFreezeFraction` `:246`, `riskForScore` `:265`, `tickForTest` `:339`, `paint` `:341` | `src/gui/SpectrumView.h` |
| `spectrumPointSizeForTest` `:122`, `spectrumPointCapacityForTest` `:123`, `dashedStemPathElementCountForTest` **`:145`**, `kDashedStemReserveFloats` **`:185`** (both xc, m-10), `snapshotNotchCountForTest` `:191`; `markerPath_` `:439` | `src/gui/SpectrumView.h` — the shape the new overlay accessor copies |
| `presets/Music.json` ships `"depth": -10.0` | `presets/Music.json:8` |
| `tools/logstats.py`: `load` with `encoding="utf-8"` **`:19`**, the `if/elif` chain with no `else` **`:46-87`** (the "an unknown ev falls through" comment at **`:62-66`**), the reason column **`:132`**, the `--expect-*` flags `:153-157` — **(xc, m-9)** | `tools/logstats.py` |
| `tools/snapshot.cpp` has **no scene registry**: `main()` `:156` is a linear sequence of `shoot()` calls at `:212`, `:357`, `:395` — **(xc, m-11)** | `tools/snapshot.cpp` |
| `tests/fixtures/session-sample.jsonl` is 15 lines and `session_end` is the **last** one — **(xc, m-12)** | `tests/fixtures/session-sample.jsonl:15` |
| `logstats_fixture` and its argument list | `tests/CMakeLists.txt:113-117`, inside `if(Python3_Interpreter_FOUND)` at `:112` |
| `HANDSFREE_CORE_SOURCES` (absolute paths, shared by app / tests / snapshot) | `CMakeLists.txt:84` |
| `gtest_discover_tests(HandsFreeTests)` and the 23-file source list | `tests/CMakeLists.txt:107`, `:20-42` |
| `project(HandsFree VERSION 1.2.0)` | `CMakeLists.txt:3` |
| test fixtures: `FakeClock` `:12`, `Harness` `:20`, `SlotHarness` `:28`, `StereoHarness` `:39`, `SineSource` `:316`, `NoiseSource` `:335`, `pump` `:350`, `pumpQuietFor` `:359`, `firstActiveIndex` `:370`, **`probeMemoryAt` `:400`** (xc, I-6), `RampSineSource` `:468`, `pumpStereo` `:517`; constants `:311-314` | `tests/test_notchcontroller.cpp` |
| `Recorder` — has **`sink()`, no `operator()`**, and its own comment `:1466-1470` requires it to be declared BEFORE the `Harness` it is wired to, because the controller's destructor flushes through the sink — **(xc, B-2)** | `tests/test_notchcontroller.cpp:1471-1486` |
| `tests/test_gui_helpers.h` exists but is GUI-only: `namespace gui_test`, and its only include is `<juce_gui_basics/juce_gui_basics.h>` — **(xc, B-2; this is why the recorder is not promoted into it)** | `tests/test_gui_helpers.h` |
| the "pump before you read a snapshot" precedent | `tests/test_gui_wiring.cpp:1033-1035` |
| test fixture: `CallbackDriver` | `tests/test_audioengine.cpp:36-61`, anonymous namespace closing at `:62` |
| `HandsFreeSnapshot` target | `tools/CMakeLists.txt:6-11` |

**INTRODUCED by this plan** (nothing above defines them today):

`SoundcheckSignal` and every member (`kSweepLowHz`, `kSweepHighHz`, `kSweepSeconds`, `kRampMs`, `kRampOutMs`, `kSoundcheckMaxPeak`, `kSoundcheckMinPeak`, `clampPeak`, `Params`, `sampleAt`, `instantaneousHz`, `totalSamples`, `rampSamples`, `clampedPeak`, `rampOut`, `rampOutSamples`); `LoopGainEstimator` and every member (`kNumBins`, `kTrustedHighHz`, `kMinBandSnrDb`, `kMinBinSnrDb`, `reset`, `pushNoiseFloor`, `pushReference`, `pushCapture`, `Result`, `finish`, `binToHz`, `hzToBin`, `Stream`, `pushInto`, `analyseFrame`); `SoundcheckCandidates` and every member (`kCandidateMarginDb`, `kMinUsefulCutDb`, `kMinProminenceDb`, `kTargetMarginDb`, `kMaxPreventivePerLane`, `Ladder`, `Depth`, `depthFor`, `Input`, `Candidate`, `Output`, `pick`, `smooth`); `SoundcheckController` and every member (`kTailSeconds`, `kGapMs`, `kNoiseFloorMs`, `kMicAbortDbfs`, `kMicAbortHoldMs`, `kResultsTimeoutMs`, `kPollMs`, the fifteen aliases, `State`, `Refusal`, `AbortReason`, `Target`, `RunParams`, `OutputResult`, `preflight`, `arm`, `applyRequested`, `dismissRequested`, `requestStop`, `abortAndJoin`, `getState`, `getCurrentTargetIndex`, `getTargetCount`, `getElapsedMsInRun`, `getRemainingMsInRun`, `copyResults`, **`copyResultsForSlot`** (I-4), `setDetectionActiveOnAllSlots`, `logEvent`, `onStateChanged`, `runOnce`, `start`, `stop`, **`worstPeakinessForTest`** (cross-check "Also"), **`abortReasonNameForTest`** (I-7), **`roundToThreeSignificantFiguresForTest`** as a static member (I-8), `enterTarget`, `enterGap`, `finishRun`, `beginAbort`, `checkDeviceUnchanged`, `checkMicNotHot`, `noiseWindowIsRinging`, `drainCapture`, `analyseCurrentTarget`, `buildPickInput`, `noiseMagnitudes_`, `worstPeakiness_`; and `OutputResult::marked` (I-3)); the free `applySoundcheckResults`, `SoundcheckApplyStats`, and the file-local `firstFreeIndexTopDown`, `round3sf`, `abortReasonName`; on `AudioEngine`: `scOutChannel_`, `scSuspendTaps_`, `scCaptureInChannel_`, `scCaptureActive_`, `scSampleIndex_`, `scPeak_`, `scRampOutAtSample_`, **`scGainUnclampedForTest_`**, `kCaptureCapacity`, `micCapture_`, `micCaptureDrops_`, `setSoundcheckOutputChannel`, `setSoundcheckCaptureChannel`, `setSoundcheckCaptureActive`, `setSoundcheckTapsSuspended`, `setSoundcheckSampleIndex`, `setSoundcheckPeak`, `requestSoundcheckRampOut`, `getSoundcheckOutputChannel`, `getSoundcheckSampleIndex`, `soundcheckIsEmitting`, `getMicCaptureBuffer`, `getMicCaptureDropCount`, **`setRunningForTest`** (B-4), **`setSoundcheckGainUnclampedForTest`** (B-5, replacing rev 1's `setSoundcheckPeakUnclampedForTest`, which is WITHDRAWN); on `NotchController`: `ClearReason::SoundcheckReplace` and `SnapshotNotch::origin`; `gui::ModeRail::measureButton`, `onMeasure`, `setMeasureEnabled`; `gui::SoundcheckPanel` entirely; `gui::SpectrumView::setSoundcheckOverlay`, `clearSoundcheckOverlay`, `hasSoundcheckOverlay`, `kLowConfidenceAboveHz`, `soundcheckOverlayPathElementCountForTest`; on `MainComponent`: `soundcheck_`, `soundcheckPanel_`, `setSoundcheckControlsLocked`, `buildSoundcheckTargets`, `soundcheckRefusalMessage`, `confirmSoundcheck`, `logSoundcheckApply`, `getSoundcheckControllerForTest`; test helpers `MultiDriver`, `routeMono`, `Rig` (with `setRunning`), `NotchRig` (with `pump` and `snapshot`), **`EventRecorder`** (B-2), `oneCandidate`, `whiteNoise`, `noisePlusTone`, `evOf` (returns **`juce::String`**, B-10), `sawEvent`, **`abortReasonInLog`**, `renderSweep`, `flatRoom`, `resonantRoom`, `runRoom`, `meanMidBandDb`, `Field`, `shippedLadder`, `measuredHzAround`, `defaultParams`, and the local `closedForm` lambda (B-8); `logstats.py`'s `soundcheck_replaced` and `--expect-soundcheck-replaced`.

**WHAT REV 1 LEFT UNVERIFIED, AND WHAT BECAME OF IT.** Lane G's M-1 is the precedent: three helpers its plan relied on existed nowhere in the repo, its self-review flagged them, and nobody acted on the flag. Rev 1 of this plan flagged four names the same way — and the cross-check found that **three of the four were wrong**, which is the argument for opening files rather than flagging them.

| Rev 1 flagged | What opening the file showed |
|---|---|
| `makeClearEvent` in `tests/test_sessionlogger.cpp` | **Does not exist anywhere in the repo.** `notchEventToVarForTest` is used only from `tests/test_gui_wiring.cpp`. The test moved there and builds its event inline (B-3) |
| `SpectrumView`'s marker-capacity accessor | The existing accessors are `spectrumPointSizeForTest` (`:122`) and `spectrumPointCapacityForTest` (`:123`); the Path-element proxy is `dashedStemPathElementCountForTest` (`:145`). Rev 2's new `soundcheckOverlayPathElementCountForTest` copies the latter, caveat included: `juce::Path` has no public capacity getter, so an element count proves "did not grow" only as a proxy (m-10) |
| Five `MainComponent` test accessors | **None of the five exist.** All are declared by Task 10 now, modelled on `getSlotPanelForTest` (`:176`) / `getSpectrumViewForTest` (`:182`); `lastMessageForTest()` returns `panelMessage_` (`src/app/MainComponent.cpp:688-692`) — I-5 |
| `SlotConfig`'s field names | **Still not opened.** They are used exactly as `src/app/AudioEngine.cpp:530-544` and `src/app/MainComponent.cpp:807-827` use them, and `routeMono` (Task 5 Step 1) is the first new user — **its step says to open `src/app/SlotConfig.h` there.** This is the ONE unread name left in the plan, and it is named here rather than buried |

### 4. Type consistency

- **`Origin` and `ClearReason` spellings.** `Origin::Soundcheck` (`src/app/NotchController.h:62`) and `ClearReason::SoundcheckReplace` / `::PartialApplyUnwind` are used with those exact spellings in Tasks 4, 7 and 8. The JSON strings are `"soundcheck"`, `"soundcheck_replace"`, `"partial_apply_unwind"` — lower snake case, mapped in exactly one place each (`originName`, `reasonName`).
- **`depthDb` vs `depthDB`.** The repo already mixes them, by history: `NotchInfo::depthDB`, `ModelNotch::depthDB`, `NotchCommand::depthDB`, `SnapshotNotch::depthDB`, `PresetNotch::depthDB` keep the capital-B form; the accessor style is `depthDbForTest` / `getNotchDepthDb`. This plan follows the **accessor** style for everything it introduces: `SoundcheckCandidates::Depth::depthDb`, `Candidate::depthDb`, `OutputResult::Candidate::depthDb`, `kMaxDepthDb`, `kMicAbortDbfs`. It never introduces a `depthDB`.
- **`marginDb` is `-hDb`, everywhere.** `LoopGainEstimator::Result` carries `hDb` (loop gain); `SoundcheckCandidates::Candidate` and `OutputResult` carry `marginDb`. The sign flip happens in exactly one place, `SoundcheckCandidates::pick`, and `MarginIsTheNegativeOfLoopGain` pins it. No function takes "margin" and computes as if it were "gain".
- **`std::max` for every ceiling clamp, in both directions**, with one meaning: pick the SHALLOWER, because deeper is more negative. Same convention lane G uses at every clamp site.
- **`kNumBins` is one number.** `Detector::kNumBins` (1025) is aliased by `LoopGainEstimator::kNumBins` and again by `SoundcheckCandidates::kNumBins`. No task declares a bin count of its own.
- **`std::int64_t` for every sample index** — `scSampleIndex_`, `scRampOutAtSample_`, `sampleAt`, `rampOut`, `totalSamples`, `rampSamples`, `rampOutSamples`. Never `int` (a 3 s sweep at 192 kHz is 576 000 samples, which fits, but the ramp anchor accumulates for the whole run and does not).
- **`float` on the wire, `double` in the maths.** The engine's atomics and `OutputResult` are `float` (they cross threads and go into `juce::var`); `LoopGainEstimator`'s accumulators and `SoundcheckCandidates::depthFor` are `double`.
- **The constants are defined once and aliased**, never re-declared: `SoundcheckSignal::k*`, `LoopGainEstimator::k*`, `SoundcheckCandidates::k*` are the definitions; `SoundcheckController::k*` are aliases. Task 4 Step 6 greps for a second literal.
- **`runOnce()`** means the same thing on both controllers: one synchronous step of the loop the thread would otherwise run, callable from a test with no thread started. `NotchController::runOnce` (`src/app/NotchController.h:349`) is the precedent.
- **Test naming.** New suites are `SoundcheckSignal`, `LoopGainEstimator`, `SoundcheckCandidates`, `SoundcheckController`, `SoundcheckApply`, `SoundcheckPanel`; additions to existing files use `AudioEngineSoundcheck`, `NotchControllerSoundcheck`, `SessionLoggerSoundcheck`, `MainComponentSoundcheck`, and one lands in `GuiWiring` (B-3, because that is where `notchEventToVarForTest` is used). `ctest -R Soundcheck` therefore selects the lane **except** `GuiWiring.SoundcheckReplaceReachesTheLogAsItsOwnReason` and `NotchControllerSoundcheck.APreventiveNotchNeitherWritesNorConsumesRoomMemory`; the full run is the gate either way.

**Signatures that CHANGED in rev 2, checked against every call site in the plan:**

| Name | Rev 1 | Rev 2 | Why |
|---|---|---|---|
| `AudioEngine::setSoundcheckPeakUnclampedForTest (float)` | existed | **withdrawn** | B-5: it clamped anyway and proved nothing |
| `AudioEngine::setSoundcheckGainUnclampedForTest (float gain)` | — | new, default `1.0f` | B-5: the seam has to sit past the signal |
| `AudioEngine::setRunningForTest (bool)` | — | new | B-4: `isRunning_` is otherwise unreachable headless |
| `SoundcheckController::copyResultsForSlot (int slot)` | consumed, never declared | declared | I-4 |
| `SoundcheckController::OutputResult::marked` | absent | `std::array<bool, kNumBins>` | I-3: the GUI needs per-bin flags, not a count |
| `SoundcheckController::abortReasonNameForTest (AbortReason)` | — | `static const char*` | I-7 |
| `SoundcheckController::roundToThreeSignificantFiguresForTest (double)` | a free shim with no home | `static` member forwarding to `round3sf` | I-8 |
| `SoundcheckController::worstPeakinessForTest()` | — | `float`, last noise window's max | flake guard |
| `firstFreeIndexTopDown (snap, lane, laneCount)` | 3 args | **`(snap, takenThisCall, lane, laneCount)`** | B-1: the bitmap is the allocator |
| `evOf (const juce::var&)` | `const char*` | **`juce::String`** | B-10: rev 1 returned a dangling pointer |
| `gui::ModeRail::Orientation::vertical` | — | **`Vertical`** | B-9: that is the real enumerator |
| five `MainComponent` test accessors | assumed to exist | **declared by Task 10** | I-5 |

Everything else in §5 above is unchanged, and every call site of the twelve rows was rewritten in the same pass that changed the declaration — `firstFreeIndexTopDown` in Task 7's implementation and in its two tests; `evOf` at all five call sites; `Orientation::Vertical` in all three `ModeRail` tests; the gain seam in `OutputClampStillCoversTheSweepPath` and in insertion point 3.

### 6. Where the cross-check itself was wrong

Recorded so the next reader does not "re-fix" these back. Both were verified by opening the file before writing the correction, which is the point of the section.

- **m-11 said "a third `shoot()` block".** `tools/snapshot.cpp` has **three** today — `:212` (`console-idle.png`), `:357` (`console-live.png`), `:395` (`console-preset-music.png`) — so lane M's is the **fourth**. m-11's substance (there is no scene registry; `main()` at `:156` is linear) is correct and is what Task 9 follows.
- **m-4 said the room-memory gate is at `:1169-1170`, "NOT `:1116`".** Both lines exist and do different jobs. `:1116` is `if (index >= 0 && origin != Origin::Soundcheck) remembered = takeRememberedDepthLocked (...)` — the gate that stops a Soundcheck placement **consuming** an entry, which is what §4.6(f) and invariant 18 describe and what the spec's `:1116` citation meant. `:1169` is `if (origin == Origin::Soundcheck) depthDb = ceiling;` — the override that stops a remembered depth **deciding** a Soundcheck depth. The plan now cites **both**, each with its job. I-6's substantive finding — that rev 1's test called only `setNotch`/`clearNotch` and so could not reach either line — is correct, and the test was rebuilt on `probeMemoryAt`.

And one place the cross-check's own ruling was not followed, already flagged at the top of this plan: **B-2's recorder is defined locally in `tests/test_soundcheckcontroller.cpp` rather than promoted into `tests/test_gui_helpers.h`.** That header exists, so the coordinator's ruling pointed at promotion — but it is GUI-only (`namespace gui_test`, sole include `<juce_gui_basics/juce_gui_basics.h>`), so promoting a `NotchController::NotchEvent` recorder would pull `app/NotchController.h` into every GUI test TU and force an edit to `tests/test_notchcontroller.cpp` to consume the promoted copy. B-2's own fix text offers the local recorder as the alternative; this plan takes it.
