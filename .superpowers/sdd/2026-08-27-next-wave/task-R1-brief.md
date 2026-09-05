# LANE R — RING RISK data provider (session R)

Toàn bộ contract đã có ở `docs/spec-ring-risk.md` — ĐỌC TRỌN file đó trước
khi viết dòng nào. Tóm tắt các mốc phải theo:

### Amendment lane R — 2026-09-06 (đối chiếu lại sau lane S + lane D)

Plan lane R viết 27/08 trước khi lane S (stereo) và lane D (data loop) hạ
cánh. Agent đối chiếu read-only (opus) tìm ra ba chỗ plan/spec **không còn
thực thi được như viết**; điều phối viên chốt như sau. Mỗi task R1–R3 đọc
khối này TRƯỚC phần task. Khi khối này và spec `docs/spec-ring-risk.md`
mâu thuẫn, khối này thắng; R3 sửa spec theo khối này.

**Mức thay đổi level dự kiến: 0 dB.** Lane R chỉ là readout. Không được
đổi bất kỳ quyết định đặt/xóa notch, `NotchCommand`, `outbox_`, hệ số
filter nào. Verifier cuối sẽ diff riêng các đường đó.

| # | Plan/spec nói | Code hiện tại | Ruling |
|---|---|---|---|
| A-R1 | "giá trị `CandidateScorer::scoreCandidate` dùng để đặt notch" | Placement dùng `scoreCandidateDetailed` rồi `score = breakdown.score * asym` (`NotchController.cpp:759-766`), so với `CandidateScorer::kConfirmScore = 0.7f` | `ringRiskScore` = **max** của chính biến `score` (sau asym) trên mọi candidate của frame, trên **cả hai làn** của slot (max-over-lanes). Không tính lại peakiness. |
| A-R2 | "ghi 3 field trong CÙNG bước publish đang ghi `magnitudes`/`notchCount`" | Khối publish (`NotchController.cpp:287-315`) chạy TRƯỚC `processSpectrumForDetection` (dispatch tại `:330`) → chưa có score tại điểm publish | Gom `frameMaxScore`/`frameScoreValid` vào member detector-thread trong vòng detection, rồi **dời khối publish xuống SAU vòng detection** để score và magnitudes của cùng frame ra cùng một lần khóa `snapshotMutex_` (không lock mới, không thread mới). Chỉ khi có test/đường unwind pin thứ tự cũ vì lý do thật (ghi rõ trong report) mới lùi về: giữ khối publish tại chỗ và thêm một lần ghi ngắn dưới cùng `snapshotMutex_` ngay sau vòng detection, chỉ cập nhật 3 field ring. Cấm phương án "publish frame sau" (trễ một hop ~10.7 ms, Critical đến sau notch). |
| A-R3 | Banding theo `PeakinessAnalyzer::getThreshold()` (5..20, mặc định 10.0) | `score` là tích 0..1 (`CandidateScorer.cpp:145`), ngưỡng là `kConfirmScore` 0.7; `score ≥ 10.0` không bao giờ xảy ra | Band theo `ringRiskThreshold` = **`kConfirmScore`** publish trong snapshot (GUI không hardcode 0.7): `!valid → Unavailable`; `score < 0.55×thr → Low`; `< thr → Rising`; `≥ thr → Critical`. Spec §2 bị thay thế. Acceptance 3 viết lại: frame chỉ có noise (scorer gate dưới ngưỡng peakiness → score 0) đọc **Low**; test bằng `NoiseSource` có sẵn. |
| A-R4 | `ringRiskValid` = "detector đủ history" | Không có accessor; `CandidateScorer::historyCount_` private (`CandidateScorer.h:114`); `processSpectrumForDetection` return sớm khi `!detectionActive_` (`:670`) | `ringRiskValid` ⇔ `detectionActive_` **và** ít nhất một làn đang active của slot có scorer đã commit ≥ 1 block history kể từ lần reset gần nhất (start / đổi device / đổi SR / `setWidth`). Thêm accessor `CandidateScorer::hasHistory() const` (hoặc counter trong `LaneAnalysis`) — không đổi hành vi. Detection tắt → invalid → chip N/A (spec: không được "trấn an sai"). |
| A-R5 | Hysteresis "chọn một trong hai" | chưa ai chọn | **Hold**: sau khi bước LÊN, không được bước XUỐNG trong 750 ms (đồng hồ inject được, cùng mẫu `FakeClock`/clock injectable của test hiện có). Bước lên tức thì. `Unavailable` thắng hold (invalid → N/A ngay). Ghi chú lý do trong code. |
| A-R6 | R3 wiring lambda `copySnapshot` (spec §4) | `SpectrumView` đã giữ `snapshot_` (`SpectrumView.h:314`); lambda copy thêm ~8 KB × 30/s | **Giữ spec §4** (lambda qua controller của `displayedSlot_`, `copySnapshot` mới mỗi tick): chi phí không đáng kể, giữ nguyên ngữ nghĩa test pin `RingRiskReadsUnavailableUntilSomethingProvidesIt` (`tests/test_gui_wiring.cpp:771`). Không tối ưu sớm. |
| A-R7 | — | `CandidateScorer.cpp:51` gate bằng `PeakinessAnalyzer::kDefaultThreshold` compile-time thay vì `analyzer.getThreshold()` sống | **NGOÀI phạm vi lane R** (đổi hành vi đặt notch — spec §Out of scope cấm). Ghi ledger + roadmap, chờ owner. |
| A-R8 | Một detector / slot | Hai `LaneAnalysis` / slot (`NotchController.h:373-383`) sau lane S | Một `ringRiskScore` + một `ringRiskValid` + một `ringRiskThreshold` cho cả slot (max-over-lanes, valid = OR các làn active). Không thêm mảng per-lane. |
| A-R9 | `SpectrumView.h:143` | enum tại `SpectrumView.h:219`, `ringRiskProvider` `:223`, poll `SpectrumView.cpp:654-660`, 30 fps xác nhận (`SpectrumView.cpp:76`) | Sửa số dòng. |
| A-R10 | R3 "Commit + release alpha" | CLAUDE.md: release là gate cuối, sau review toàn nhánh | Implementer R3 **không** chạy `release-alpha.ps1`. Điều phối viên chạy sau final review sạch (patch 1.1.2 → 1.1.3). |

Harness có sẵn cho test R1 (`tests/test_notchcontroller.cpp`): `Harness:19`,
`SlotHarness:27`, `StereoHarness:38`, `FakeClock:11`, `SineSource:308`,
`NoiseSource:326`, `pump():344`, `pumpStereo():351`, `primeAndPlace():392`,
`kWarmupBlocks = 64` (`:306`). R2: `riskForScore` là hàm static thuần trên
một `SnapshotBuffer` tự điền — không cần harness.

Baseline đo 2026-09-06 tại `d58eac0`: `ctest -C Release` **432/432**.



### Task R1: Publish score vào SnapshotBuffer

**Files:**
- Modify: `src/app/NotchController.h` (struct SnapshotBuffer),
  `src/app/NotchController.cpp` (điểm publish snapshot hiện có)
- Test: `tests/test_notchcontroller.cpp`

**Interfaces:**
- Produces (đúng theo spec-ring-risk.md §1):

```cpp
    float ringRiskScore = 0.0f;   // max candidate confidence của frame; 0 khi không bin nào scoreable
    bool  ringRiskValid = false;  // false tới khi detector đủ history
    float ringRiskThreshold = 0.0f; // threshold sống (spec §2: không hardcode 10.0 ở GUI)
```

- [ ] **Step 1: failing test** — publish một frame có candidate confidence
  X → snapshot đọc ra ringRiskScore == X, valid == true; trước frame đầu →
  valid == false, GUI-side phải hiểu là Unavailable.
- [ ] **Step 2: RED.**
- [ ] **Step 3: implement** — ghi cả 3 field trong CÙNG bước publish đang
  ghi `magnitudes`/`notchCount`, dưới cùng mutex snapshot. Không lock mới,
  không thread mới (spec §1). Giá trị lấy từ đúng con số
  `CandidateScorer::scoreCandidate` dùng để quyết định đặt notch — KHÔNG
  tính lại một peakiness thứ hai (spec: "What is missing").
- [ ] **Step 4: GREEN + full ctest.** Commit.

