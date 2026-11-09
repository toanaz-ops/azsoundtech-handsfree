# AI tự lái điểm cắt — thiết kế (lane L3 + C2)

Ngày 2026-11-09. Quyết định: [`../decisions/2026-11-09-ai-drive.md`](../decisions/2026-11-09-ai-drive.md)
(D0–D12, điều phối chốt theo uỷ quyền của owner). Nền: lane C1 (mạng nơ-ron chạy bóng,
[`2026-11-09-candidate-classifier-design.md`](2026-11-09-candidate-classifier-design.md)) và lane L2 (tiến trình
phụ hỏi LLM). Số dòng `file:line` dưới đây đo trên `main` `9abd93b` cộng bản sửa phase AI; kiểm lại trước khi dùng.

## 1. Mục tiêu và giới hạn

Owner muốn AI (LLM và mạng nơ-ron) **tự lái notch liên tục** thay vì chỉ khuyên, có chế độ, và biểu đồ phải
chỉ ra thao tác nào do AI làm.

Bất biến an toàn — mọi pha:

- AI **chỉ cắt trong trần độ sâu hiện có**, hoặc trả một tần số về mức chưa-notch. Không đường nào cho mức
  vượt 0 dB: `depth = max(depth, kMaxDepthDb)` và `depth <= 0` (`NotchController.cpp:412, 422`), không đường
  nào nâng trần.
- Detector là thứ duy nhất bắt được cú hú (dựng trong 100–500 ms). LLM trễ 2–60 s; nó giám sát, không cứu.
- AI hỏng kiểu gì (mất mạng, hết giờ, JSON sai, tiến trình chết, thiếu model) thì detector chạy y như hôm nay:
  đường detector chỉ đọc đúng một atomic của AI.
- Lời văn tự do của LLM **không** chạm biểu đồ hay audio. Lệnh đi qua tool call có kiểu, C++ là ranh giới tin
  cậy. Bộ lọc cụm từ (Q26) không phải hàng rào của lane này.

## 2. Chế độ

Một công tắc `AiMode { Off, Shadow, Suggest, Act }`, hiển thị **TẮT / BÓNG / GỢI Ý / TỰ LÁI**, đặt ở đầu bảng
COPILOT (`SegmentedControl`), **không** nằm trong `ModeRail` (`ModeRail.h:47-53` cấm nấc thứ tư; AUTO đã là
mode của engine). Lưu trong `AppSettings`; mỗi lần mở app, `Act` bị hạ về `Suggest` (D2). Preset không mang
chế độ AI. Dumb mode chỉ có nhãn đọc "AI TỰ LÁI" (D9).

Mỗi tay lái bị kẹp riêng: `effective(actor) = min(requested, ceiling(actor))`.

| Tay lái | Trần hôm nay | Lý do |
|---|---|---|
| Mạng nơ-ron | `Shadow` | model nhúng là `untrained-fixture-v1`; cổng chưa qua |
| LLM | `Suggest` ở L3a/L3b, `Act` từ L3c | cần sổ hoàn tác và bộ kiểm lệnh trước |

Ở BYPASS, SOUNDCHECK hoặc khi lane M đang đo: không tay lái nào được hành động.

## 3. Tác giả không phải Origin

`Origin { Detector, Preset, Manual, Soundcheck }` (`NotchController.h:63`) quyết **chính sách**: trần nào, có
leo thang không, có tự nhả không (`.cpp:952, 974, 1579, 1663`). Bài học lane M §14: không thêm Origin. Thêm
trường riêng, chỉ để ghi công:

```cpp
enum class Author : uint8_t { Detector, Human, Network, Llm };
```

- `ModelNotch`: `placedBy`, `lastChangedBy`, `nid` (uint32, cấp trong `setNotchImpl`), `aiHoldUntilLiveMs`,
  `supervisorRelease`. Cùng các trường trên `SnapshotNotch`; `NotchEvent` thêm `author`, `nid`.
- `NotchCommand` (`src/dsp/NotchCommand.h:5-21`) **không đổi** — tác giả không xuống audio thread.
- `Network` nghĩa là "notch detector đặt, mạng đã đổi độ sâu khởi điểm". Origin vẫn là Detector.
- Không quyết định nào của detector đọc các trường này.

## 4. Mạng nơ-ron ở TỰ LÁI (pha C2)

Hai núm, cả hai bị `std::max(depthDb, ceiling)` kẹp sau (`.cpp:1385`):

- **K1 — độ sâu khởi điểm (làm trước, D4).** Trong `placeConfirmed`, dời lời gọi `shadowScore` của site B lên
  trên bước chọn độ sâu, thêm bước 2b sau `.cpp:1356`:
  `if (networkActs_ && origin == Detector && nn >= threshold) depthDb = std::min(depthDb, kDepthLadderDb[1]);`
  Tập notch, block đặt và index không đổi. Mức: sâu thêm tối đa 6 dB ở tần số detector đã chọn, trong ≥ 300 ms
  đầu; notch sống lâu thêm ~10 s mỗi bậc. Ghi đè của Soundcheck (`.cpp:1396`) vẫn là bước cuối.
- **K2 — xác nhận sớm một block (sau, tuỳ chọn).** Site C trong `if (score > kConfirmScore)` (`.cpp:1777-1781`).
  Lợi ≤ ~21 ms, bằng 0 ở AGGRESSIVE. Có thể sinh notch detector không đặt — chiếm chỗ chuỗi (16/làn) và thêm
  phạt harmonic (`CandidateScorer.h:15-17,46`). Nhánh `else { persistence = 0 }` (`.cpp:1800-1803`) là đường
  phủ quyết: mạng **không bao giờ** chạm.
- Mạng không được dùng dưới `kConfirmScore`: không có nhãn ở đó (Q23).

**Khoá.** Thêm `"authority": "shadow"|"assist"` và `"assist_threshold"` vào JSON trọng số. Chỉ đường xuất sau
khi `gate.py --for assist` đạt mới được ghi `assist` (D5: precision ≥ 0,95 trên tập giữ lại, ≥ 60 dòng boost).
`check_embedded_models.py` (ctest `nn_embedded_model_gate`) đánh rớt mọi model `assist` không kèm báo cáo đạt
khớp sha; `untrained-*` và model tổng hợp luôn là `shadow`. `MainComponent` (chỗ gắn classifier, `:308-321`)
tính `nnMayAct_`; `NotchController::setNetworkActs(bool, float)` là atomic relaxed, tính lại mỗi lần đổi chế
độ AI hoặc mode engine.

**Chứng minh: trội, không phải bằng.** Mở rộng harness `ShadowClassifier`
(`tests/test_notchcontroller.cpp:4422-4530`):

- `NetworkAct.ForcedLowIsIdenticalToNoClassifier` — chuỗi lệnh và sự kiện y hệt.
- `NetworkAct.CutDominatesBaselineEveryBlock` — sau mỗi `runOnce`: tập (làn, index, Hz) đang sống y hệt,
  `depth_act <= depth_off`, và có ít nhất một block sâu hơn thật.
- `NetworkAct.LockedForUntrainedModel`, `.NeverTouchesSoundcheckOrigin`, `.CeilingBindsBoost`,
  `.BoostAddsAtMostOneReleaseRung`.
- K2: `NetworkAct.ExtraPlacementCanFillChain`, `.ExtraNotchHalvesHarmonicCandidate` — phơi rủi ro bằng test.
- Phạm vi: chứng minh vòng hở (fixture bơm tone chưa-notch, bài học lane G M-3). Vòng kín thuộc về buổi nghe alpha.

## 5. LLM giám sát

### 5.1 Bộ lệnh (D11)

Chỉ nhắm notch Origin Detector ở TỰ LÁI (D3). Địa chỉ bằng `nid` + `expect_hz`.

| Tool | Tham số | Hiệu ứng C++ |
|---|---|---|
| `propose_deepen` | `nid` uint32; `expect_hz` Hz (±1 bin); `target_db` dB: một bậc thang hoặc trần, sâu thêm tối đa một bậc, `>= ceilingDbFor(n)` | `retuneNotch(..., Author::Llm)`, ramp 10 ms, `deepestDb = min(deepestDb, target)`, nạp lại `stageChangedAtMs` |
| `propose_hold` | `nid`; `expect_hz`; `hold_s` 30..600 | `aiHoldUntilLiveMs`; thang bỏ qua `quietMs += dt` (`.cpp:1032`). 0 dB lúc áp dụng |
| `propose_release` | `nid`; `expect_hz`; `reason` ∈ {`judged_false`, `stale_no_recurrence`} | `requestRelease`: đặt `quietMs` tới ngưỡng bậc đầu + cờ; thang lane G đi lên một bậc mỗi 10 s; Clear cuối dùng `ClearReason::SupervisorRelease`, bỏ `rememberReleaseLocked` |
| `no_action` | `reason` enum | chỉ ghi log |

Không có: đặt notch mới, Q (`NotchChain::setNotch` reset bộ lọc khi Q đổi, `NotchChain.cpp:39-67`), trần,
bypass, clear-all, `clearNotch` trực tiếp (bước nhảy tới 24 dB, `:78-86`). Mỗi lệnh mang `reason` dạng enum;
C++ đổi nó thành một câu tiếng Việt cố định.

Hôm nay **không có đường "người sửa notch"** để mượn kẹp: `Origin::Manual` không có caller GUI. Kẹp dùng chung
là các vị từ trong `setNotchImpl` (`.cpp:405-422`) và `pushRetuneLocked` (`:513-518`). Cần entry point công
khai mới (`retuneNotch`, `holdNotch`, `requestRelease`); gọi `setNotch` lên notch đang sống sẽ đóng lại
`lockedAtMs` và reset thang (`:438-453`).

### 5.2 Bộ kiểm lệnh

`src/app/AiAction.{h,cpp}`: hàm thuần trên `SnapshotBuffer` + `AiPolicyState`, test headless. Từ chối khi:

- chế độ hiệu dụng của LLM dưới `Act`; engine ở BYPASS / SOUNDCHECK; lane M đang chạy;
- thế hệ yêu cầu đã cũ (tăng khi restart thiết bị, nạp preset, CLEAR ALL, đổi mode, đổi chế độ AI);
- `nid` không sống, hoặc `|hz - expect_hz| > 1 bin`; Origin không phải Detector;
- trường NaN / inf / ngoài miền / sai kiểu;
- đào sâu mà không sâu hơn thật, hoặc vượt trần;
- nhả khi `ringRiskValid && ringRiskScore >= kRiskFreezeFraction * kConfirmScore` (cả hai vế là tích 0..1);
  hoặc đã có một lần nhả AI đang chạy trên slot đó; hoặc lần nhả trước cách chưa tới 120 s (toàn app); hoặc
  là lệnh nhả thứ 3 trở đi trong một lượt.

Sau đó `NotchController` kiểm lại dưới `modelMutex_` (không đệ quy, `.h:797`) trên message thread, như `setNotch`.

### 5.3 Đường đi, nhịp, chi phí (D6, D10)

Timer (message thread) → ghi `supervisor-state.json` vào thư mục log → một `SidecarRunner` **thứ hai** chạy
`HandsFreeCopilot.exe <log> --supervise --state=<f> --json --credentials-only ...` → kết quả về message thread
→ parse `{"actions":[...],"usage":...}` → kiểm → GỢI Ý: sổ đề xuất (dấu mờ + dòng); TỰ LÁI: áp dụng + ghi sổ.

- Chỉ gọi theo sự kiện: detector đặt notch ở một bin đã có ≥ 3 lần đặt trong 10 phút; một notch chạm trần
  ≥ 30 s; nhịp 10 phút nếu có gì đổi. Cách nhau ≥ 120 s, ≤ 12 lượt/giờ, 40 lượt/buổi (chỉnh được).
- Chi phí ước 1,2–3,6 USD/giờ ở mức trần. **Chưa đo**: chưa có lượt gọi nhà cung cấp thật nào.
- Watchdog 60 s, `--timeout=50`. Đang có lượt hỏi của người thì lùi nhịp giám sát.
- Huỷ khi: BYPASS, chế độ AI dưới GỢI Ý, restart thiết bị, đóng app. Kết quả thế hệ cũ bị bỏ.
- Đang chờ: chip `dim` "AI · đang xét 23 s" ở đầu bảng COPILOT. Biểu đồ không đổi tới khi kết quả qua kiểm.

### 5.4 Hoàn tác

Sổ trên message thread, mỗi mục kèm thao tác ngược: đào sâu → retune về `from_db` nếu notch còn ở độ sâu AI
đặt (không thì "đã bị thay"); hold → bỏ hold; nhả đang chạy → huỷ cờ, retune về độ sâu trước; **nhả đã xong →
không hoàn tác được**, chỉ mời đặt lại (notch mới, Origin Manual, Author Human). Thao tác của mạng nơ-ron
không hoàn tác được — giới hạn của nó là thiết kế. "HOÀN TÁC HẾT AI" đi ngược sổ.

## 6. Đánh dấu (Sodium Rack)

Ice (`settled`) đã có đúng một nghĩa — notch đã trụ (`AzTheme.h:13-19, 60`); thân đứt + hậu tố "R" đã nghĩa là
làn 1 (`SpectrumView.cpp:1508-1549`). Không thêm màu; tác giả đọc được không cần màu.

- **Notch AI đã áp dụng:** màu theo tuổi như mọi notch; chữ trên cờ `N` (mạng) hoặc `A` (LLM — không dùng `L`,
  dễ đọc thành Left cạnh `R`); thêm một ô vuông nhỏ có viền ở đỉnh nêm, còn thấy khi cờ bị bỏ vì chồng (`:1554`).
- **Đề xuất (dấu mờ):** nêm chỉ có viền, màu `dim`, **không thân**; cờ rỗng `+A` / `+N`.
- **Bảng ACTIVE NOTCHES:** cột hẹp `BY`, 3 ký tự mono (`D`, `H`, `N`, `A`, `D>A`). Đề xuất nằm dưới nhãn
  `ĐỀ XUẤT` với nút `ÁP DỤNG` / `BỎ`. Đo bề rộng bằng font thật.
- **Snapshot** (`tools/snapshot.cpp`): seam `setNotchAuthoredForTest`, `setAiProposalsForTest`,
  `setAiModeForTest`; ảnh mới `console-ai-suggest.png`, `console-ai-act.png`, `console-ai-locked.png`.
- GỢI Ý của mạng nơ-ron chỉ là chú thích (nó quyết trong ~10,7 ms, không ai kịp duyệt) và **ẩn** khi model
  chưa có quyền `assist`.

## 7. Log — schema 4

`session_start`: `log_schema: 4`, `ai_mode`, (C2) `nn_authority`. `notch_set` / `notch_retune` / `notch_clear`:
thêm `author`, `nid`; (C2) `nn_assist: "depth"|"persist"`. Sự kiện mới: `ai_mode`, `ai_call` (trigger, gen, ms,
status, usage), `ai_proposal`, `ai_decision`. `tools/logstats.py`, luật của `copilot.py` và
`tools/nn/dataset.py` chỉ đếm `author == "detector"` trừ khi nói khác (D8).

## 8. Pha

| Pha | Nội dung | Mức thay đổi dự kiến |
|---|---|---|
| **L3a** | công tắc chế độ, `Author` + `nid`, đánh dấu, schema 4, `--supervise`, GỢI Ý chỉ xem (`ÁP DỤNG` tắt, chỉ `BỎ`) | **0 dB ở mọi ngõ ra** |
| **L3b** | `retuneNotch` / `holdNotch` / `requestRelease`, bộ kiểm, người bấm `ÁP DỤNG`, sổ hoàn tác | chỉ khi người bấm: đào sâu ≤ 6 dB ở một tần số; nhả trả lại tối đa độ sâu notch trong ≥ 30 s |
| **L3c** | LLM TỰ LÁI (tự áp dụng) | như L3b, không cần bấm; ≤ 12 lượt/giờ, một lần nhả mỗi slot |
| **C2** | mạng nơ-ron TỰ LÁI (K1) — **khoá tới khi có model thật qua cổng assist** | sâu thêm ≤ 6 dB trong ≥ 300 ms ở tần số detector đã chọn |

Test bắt buộc theo pha:

- L3a: chạy lại `ShadowClassifier.DecisionsAreIdentical...`; `AiMode.SuggestEmitsNoCommand`;
  `AiMode.FailureLeavesDetectorUntouched` (thiếu exe, hết giờ, JSON sai, bị kill); `AiAction.ParseRejects*`;
  `NotchEventToVar.AuthorAndNidOnEveryNotchLine`; test enum→chuỗi lặp qua mọi enumerator; schema 4 ở
  `logstats` và `test_copilot.py`; ảnh render.
- L3b: `AiAction.Refuses*` (một test mỗi lý do); `SupervisorApply.NeverEmitsShallowerSetOrClearDirectly` (fuzz
  10k lệnh); `SupervisorRelease.RingingBinCancelsRelease`, `.NoRoomMemory`; `Hold.FreezesQuietClockOnly`; `Undo.*`.
- L3c: `LlmAct.AppliesOnlyWhenEffectiveAct`, `.BypassCancelsInFlight`, `.RateLimits`, `.StaleGenerationDropped`.
- C2: mục 4.

Mỗi pha cập nhật `docs/GIOI-THIEU.md`, `docs/KY-THUAT-CHONG-HU.md`, roadmap (dòng C và L), release note,
`installer/TESTER-NOTES.md`, và một note `memory/`.

## 9. Thư viện dùng chung

Không tách gì mới. Bộ lệnh, bộ kiểm, `Author` và sổ hoàn tác là đặc thù notch. `SidecarRunner` và
`CopilotCredentialStore` là chung về bản chất nhưng chưa có sản phẩm thứ hai nào được nêu tên dùng chúng, nên ở
lại trong app (luật `CLAUDE.md`). Hằng theme mới (ví dụ alpha của dấu mờ) vào `AzTheme.h` với tên trung tính.

## 10. Điều không làm, và vì sao

- LLM đặt notch phòng ngừa: dữ liệu của nó cũ hàng giây tới hàng chục giây.
- Đổi Q, clear trực tiếp: gây "cạch" / bước nhảy mức.
- Phụ thuộc Wi-Fi hội trường cho bất cứ thứ gì bảo vệ PA.
- Đổi model giữa buổi diễn (Q20).
- Mạng nơ-ron phủ quyết notch của detector: gỡ bảo vệ dựa trên một model chưa có nhãn dưới ngưỡng xác nhận.
