### Task 8: Screenshot with verdict states, docs, release note, roadmap status

**Files:**
- Modify: `tools/snapshot.cpp` (after the placement loop, before the final `pump`)
- Modify: `docs/KY-THUAT-CHONG-HU.md` (new §8 before §7's "Trạng thái", renumber: insert as §7 "Log session và nhãn" and make the status section §8), `docs/GIOI-THIEU.md` (feature table row + new section "Chấm notch để app học" after "Theo dõi khi chạy"), `docs/superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md` (status table: D done, C gate "≥ 300 verdict từ ≥ 3 session")
- Create: `docs/release-notes/1.1.2-alpha.md`
- Modify: `docs/superpowers/specs/2026-09-05-data-loop-design.md` — add a "Trạng thái" line at the top pointing at this plan's amendment table (do not rewrite the spec)

- [ ] **Step 1: Snapshot shows the three verdict states** — in `tools/snapshot.cpp`, after the placement loop and its final `pump`/refresh, before `shoot (... console-live.png)`:

```cpp
    // Lane D: one row unjudged, one GOOD, one FALSE -- the FALSE row is
    // still in the (stale) snapshot because nothing is pumped after the
    // click, which is exactly the frame an operator sees for the ~250 ms
    // before the panel's next refresh drops it.
    auto& list = app.getNotchListPanelForTest();
    list.refreshFromSnapshot();
    if (list.rowCountForTest() >= 3)
    {
        list.goodButtonForTest (0)->onClick();    // 247 Hz L  -> GOOD
        list.falseButtonForTest (2)->onClick();   // 1920 Hz L -> FALSE (clears it in the model)
    }
```

Rebuild `HandsFreeSnapshot`, run `build/tools/Release/HandsFreeSnapshot.exe shots 1440 920 --fast`, READ `shots/console-live.png` and `shots/console-idle.png`. Required in the live shot: VERDICT header, row 1 reads `GOOD`, row 2 shows two buttons `GOOD` `FALSE`, row 3 reads `FALSE`, HELD ages still visible, nothing truncated. Send both PNGs with `SendUserFile`.

- [ ] **Step 2: `docs/KY-THUAT-CHONG-HU.md`** — insert before the "Trạng thái & kiểm chứng" section:

```markdown
## 7. Log session và nhãn (lane D, v1.1.2)

App ghi một file mỗi lần chạy vào `%APPDATA%\AZSoundtech\HandsFree\logs\session-YYYYMMDD-HHMMSS-mmm.jsonl`,
mỗi dòng một object JSON có `t` (ms từ lúc mở app) và `ev`. Giữ 30 file mới nhất.

| `ev` | Khi nào | Mang gì |
|---|---|---|
| `session_start` | mở app, sau khi mở device | phiên bản app, OS, device, sample rate, buffer, cấu hình 8 slot (`width`, kênh, `linked`) |
| `mode` | bấm Bypass / Auto / Soundcheck | `mode` |
| `tuning` | đổi DETECTION toàn cục (`slot: -1`) hoặc tuning riêng của slot | `rise_ms`, `persist`, `q`, `depth_db`, `thr` |
| `notch_set` | detector đặt notch, hoặc notch từ preset / tay | slot, làn, index, Hz, Q, depth, `origin`; với detector thêm điểm số tách trục (`p_norm`, `rise`, `novelty`, `penalty`, `asymmetry`) và `ctx`: phổ 1025 bin lúc quyết định (`now`), phổ mà trục rise đã so (`ref`, kèm `ref_age_ms`), phổ làn kia cùng vòng (`other_lane_now`) |
| `notch_clear` | notch rời model | `reason`: `manual` / `clear_all` / `auto_release` / `width_change` / `verdict_false` / `partial_apply_unwind`, `age_ms` |
| `verdict` | bấm GOOD / FALSE trên bảng ACTIVE NOTCHES | `verdict`, `age_ms` |

Cam kết: **không có audio** trong log — chỉ magnitude phổ (3 chữ số có nghĩa), không tên
người, không gửi đi đâu. Ghi từ thread riêng (`SessionLogger`), không bao giờ từ audio
thread; hàng đợi 4096 dòng, quá thì bỏ và đếm vào `dropped_events` ở dòng `session_end`.
Sự kiện của `NotchController` đi qua một outbox 64 phần tử và chỉ được đẩy ra **ngoài**
`modelMutex_` trên detector thread (spec D-6), nên nút CLEAR ALL không bao giờ chờ I/O.

`FALSE` vừa ghi nhãn vừa xóa notch (đó là điều người vận hành muốn — D-2); `GOOD` chỉ ghi
nhãn. `ref` là **đúng frame scorer đã so** (mới nhất có tuổi ≥ 0,45 × rise), không phải
frame tra lại theo `rise_ms` (D-7). Tóm tắt một file: `python tools/logstats.py <file>`.
Lane C (classifier) mở khi có ≥ 300 verdict từ ≥ 3 session.
```

Then renumber the existing "## 7. Trạng thái" to "## 8." and update its first line to `v1.1.2` with the new test count and a sentence: "Bản 1.1.2 thêm vòng dữ liệu (lane D): nút GOOD/FALSE, log session JSONL, `tools/logstats.py`."

- [ ] **Step 3: `docs/GIOI-THIEU.md`** — add a feature-table row after "Stereo độc lập":

```markdown
| **Chấm notch để app học** | Mỗi dòng ACTIVE NOTCHES có nút **GOOD** / **FALSE**. FALSE xóa notch ngay và ghi nhãn "cắt oan"; GOOD ghi nhãn "cắt đúng". Nhãn + phổ lúc quyết định vào log session trên máy (không audio) — dữ liệu cho bộ phân loại ở bản sau |
```

and a new section after "Theo dõi khi chạy":

```markdown
## Chấm notch để app học

Thấy app cắt oan một nốt nhạc: bấm **FALSE** trên dòng đó — notch nhả ngay và app ghi
lại "đây là cắt oan". Thấy nó cắt đúng tiếng hú: bấm **GOOD**. Không bắt buộc, nhưng mỗi
lần bấm là một mẫu huấn luyện. Log nằm ở `%APPDATA%\AZSoundtech\HandsFree\logs\`, một
file mỗi lần mở app, không chứa audio; gửi file khi được hỏi.
```

- [ ] **Step 4: Release note** — create `docs/release-notes/1.1.2-alpha.md`:

```markdown
# Hands-free 1.1.2 alpha — vòng dữ liệu: nút GOOD / FALSE và log session

**Không đụng đường audio.** Mức thay đổi level: 0 dB. Bản này chỉ quan sát và ghi.

## Có gì trong 1.1.2

- Bảng **ACTIVE NOTCHES** có cột **VERDICT** với hai nút mỗi dòng. **FALSE** = app cắt
  oan (notch nhả ngay, nhãn được ghi). **GOOD** = cắt đúng hú (chỉ ghi nhãn, notch giữ).
- Log session `%APPDATA%\AZSoundtech\HandsFree\logs\session-*.jsonl`, một file mỗi lần
  mở app, giữ 30 file. Ghi: device, mode, tuning, mọi notch đặt/xóa kèm phổ lúc quyết
  định, và verdict. **Không có audio trong log.**
- `tools/logstats.py <file>` in bảng tóm tắt (cần Python 3, không cần thư viện).
- Sửa nhỏ từ 1.1.1: LOAD… cập nhật bảng ROUTING ngay; preset có notch làn R cho slot mono
  được đếm và ghi log thay vì lặng lẽ bỏ; slot mono → stereo không dò lại đuôi audio cũ
  của làn R.

## Việc tester cần làm

Bấm **FALSE** khi app cắt oan nốt nhạc, **GOOD** khi cắt đúng hú. Cuối buổi, copy file
mới nhất trong thư mục logs và gửi kèm mô tả show. Nếu bấm FALSE mà notch không nhả trong
1 giây, báo lại kèm file log.
```

- [ ] **Step 5: Roadmap status** — in the roadmap's status table set D to "**đã làm xong** trên nhánh `claude_desk/lane-d-data-loop-239597` (8 task, suite N/N, review toàn nhánh sạch); release 1.1.2 alpha; chờ owner PR" and C to "chờ D nhãn: mở khi có ≥ 300 verdict từ ≥ 3 session". Add the spec status line.

- [ ] **Step 6: Full suite one last time** — `ctest -C Release`: `100% tests passed`. Paste the count into the docs where "N/N" stands.

- [ ] **Step 7: Commit**

```bash
git add tools/snapshot.cpp docs/KY-THUAT-CHONG-HU.md docs/GIOI-THIEU.md docs/release-notes/1.1.2-alpha.md docs/superpowers/specs/2026-09-04-anti-feedback-v2-roadmap.md docs/superpowers/specs/2026-09-05-data-loop-design.md
git commit -m "docs: lane D — log session, nút GOOD/FALSE, note tester 1.1.2; snapshot shows verdict states"
```

---

## After the last task (orchestrating session, not a task agent)

1. Whole-branch review by the strongest model, read-only agent, diff `main..HEAD`. Fix rounds as new agents with brief + report + finding.
2. `memory/` note if anything non-obvious was learned; index in `memory/MEMORY.md`.
3. Delete `.superpowers/sdd/.gitignore` (the skill script writes it; this repo tracks `.superpowers/`). Commit the ledger.
4. `pwsh -File installer\release-alpha.ps1` (patch → 1.1.2). Then TESTER-NOTES.md gets a new top entry with the SHA-256 and the sentence from the release note. Commit the `CMakeLists.txt` bump.
5. Handoff to `D:\DEV CAVE EP3\shared\handoff\`. No push, no merge unless the owner says so.

## Self-review

**Spec coverage.** §2 goals 1–4 → Tasks 5, 1, 4, 7. §3.1 logger → Task 1 (A-5 amends the ctor). §3.2 events → Task 6 table; `session_start` slots carry `linked` (S merged). §3.2 `ScoreBreakdown` → Task 2. §3.3 sink/outbox/reasons/unwind sites → Task 3; `ctx` → Task 4; `setWidth` reason → Task 3 (A-1). §3.4 GUI → Task 5; the screenshot with three states → Task 8. §3.5 tool → Task 7. §3.6 docs → Task 8. §4 tests 1–4 → T1, 5–6 → T2, 7 → T4, 8–11 → T3, 12–13 → T5, 14–15 → T6, 16 → T7. §5 release note + roadmap C gate → Task 8 + closing steps. Lane-S loose ends (A-9) → T3 (widen reset, skipped count), T6 (refresh, skipped log).

**Placeholders.** None: every step carries code. Two places tell the implementer to read a neighbouring file before relying on a detail (`Rig::cycle`, `PresetManager` slot keys, `getTuningPanelForTest`); each names the file and the symbol.

**Type consistency.** `NotchEvent`/`SpectralContext`/`ClearReason`/`EventSink` are defined once in Task 3 and used by name in Tasks 4 and 6. `scoreCandidateDetailed`/`ScoreBreakdown` (Task 2) feed `PlacementContext` (Task 4). `SessionLogger::{start(directory, header), stop, log, makeEvent, currentFile, droppedEvents, defaultDirectory}` (Task 1) are the only logger calls Task 6 makes. `NotchListPanel::{onVerdict (slot, lane, index, hz, good, ageMs), goodButtonForTest, falseButtonForTest, verdictForTest, Verdict}` (Task 5) match Task 6 and Task 8. `adoptPreset (notches, int*)` (Task 3) matches Task 6. `stop (int)` keeps its signature; only its body changes.
