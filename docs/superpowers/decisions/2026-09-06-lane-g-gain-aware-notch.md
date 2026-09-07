# Sổ quyết định — Lane G: Gain-aware notch (2026-09-06)

Mỗi mục là một ngả rẽ thiết kế đã hỏi owner trong brainstorm. Ghi đủ: câu hỏi,
các phương án đã đề xuất (kèm phương án khuyên dùng), và lựa chọn của owner.
Lật lại khi cần đổi hướng; đừng hỏi lại câu đã có đáp án ở đây.

Spec đích: `docs/superpowers/specs/2026-09-06-gain-aware-notch-design.md`.
Người hỏi: Fable (điều phối). Người quyết: owner (ToanAZ).

## Bối cảnh nêu trước khi hỏi

- Depth hôm nay là số cố định từ preset (`notchDepthDb_`, mặc định −18, kẹp
  [−24, −6]) gán lúc đặt notch; nhả cụt sau 30 s không reinforce.
- `Biquad::setNotchFilter` gọi `reset()` mỗi lần đổi hệ số → đổi depth trên
  notch đang chạy = click. Nhả dần bắt buộc có đường đổi hệ số giữ state.
- Công thức τ → loop gain vượt X dB cần round-trip delay (lane M đo); app
  chỉ có `rise` = mag_now / mag_250ms_ago.
- Quyết định treo lane R (I-3, `setSampleRate` không caller, A-R7) đã nêu;
  owner không gộp vào G.

## Q1 — Vai trò slider depth preset (−24..−6) khi có lane G?

| # | Phương án | |
|---|---|---|
| 1 | Preset depth = **trần**; G bắt đầu nông hơn, chỉ đào sâu tới mức preset khi còn hú | khuyên dùng |
| 2 | Preset depth = điểm bắt đầu; G được đào sâu hơn tới −24 | |
| 3 | Bỏ qua preset; G tự chọn trong [−24, −6] | |

**Chọn: 1.**

## Q2 — Depth khởi điểm và cách đào sâu?

| # | Phương án | |
|---|---|---|
| 1 | Bắt đầu −6, bước 6 dB mỗi ~300 ms còn hú, tối đa tới trần; `rise` dốc lúc confirm → nhảy thẳng −12 | khuyên dùng |
| 2 | Depth khởi điểm map từ `rise` bằng bảng (chậm −6 / vừa −12 / dốc −18), rồi vẫn bước 6 dB | |
| 3 | Bắt đầu bằng trần như hôm nay, chỉ thêm nhả dần | |

**Chọn: 1.** Khoảng chờ giữa bước giữ 300 ms (owner không đổi).

## Q3 — Tốc độ nhả?

| # | Phương án | |
|---|---|---|
| 1 | 30 s yên → nông một bậc, rồi 10 s mỗi bậc sau (−18 →30s→ −12 →10s→ −6 →10s→ Clear); hú quay lại → kẹp **ngay** về bậc sâu nhất đã đứng, đồng hồ về 0 | khuyên dùng |
| 2 | 10 s mỗi bậc kể cả bậc đầu | |
| 3 | 30 s mỗi bậc | |

**Chọn: 1.** Kèm: sau Clear, hú quay lại cùng tần số → đặt lại từ bậc sâu cũ
("phòng nhớ"), không từ −6. **Owner đồng ý.**

## Q4 — Ring-risk (lane R) tham gia G thế nào?

| # | Phương án | |
|---|---|---|
| 1 | **Chỉ điều tiết nhả**: `ringRiskScore` ≥ 0.55×ngưỡng (băng RISING) → đóng băng đồng hồ nhả của mọi notch; không dùng để đào sâu | khuyên dùng |
| 2 | Điều tiết nhả và rút ngắn bước đào (150 ms khi RISING) | |
| 3 | Chưa dùng ring-risk trong G | |

**Chọn: 1.**

## Q5 — Cơ chế đổi depth không click trên notch đang chạy?

| # | Phương án | |
|---|---|---|
| 1 | **Giữ state + ramp hệ số theo mẫu ~10 ms** khi cùng freq/Q (0.6 dB/ms là invariant); đặt mới / Clear / đổi freq vẫn reset | khuyên dùng |
| 2 | Giữ state, không ramp (bỏ `reset()` thôi) | |
| 3 | Crossfade hai biquad song song ~20 ms | |

**Chọn: 1.**

## Q6 — Hai số Fable tự chốt khi trình design (owner gật "ok")

- Rise dốc để nhảy thẳng −12: **`riseRatio ≥ 2.0`** (tăng ≥ 6 dB trong cửa
  sổ rise). Ghi chú: `rNorm` bão hòa ở 1.5 nên phải lộ tỉ số thô.
- TTL "phòng nhớ" sau Clear: **5 phút**, khớp ±1 bin (dung sai bin bị Q10
  lật thành ±0), mục dùng một lần.

**Chọn: giữ cả hai** (owner: "ok, tiếp phần 2").

---

**Q7–Q12 — từ phản biện read-only vòng 1 (opus, 2026-09-06). Owner trả lời 2026-09-07, tất cả đã có `Chọn`.**

## Q7 — Thang kẹt ở bậc nông (M-9): khi −6 hoặc −12 đã đủ làm bin hết vượt ngưỡng, notch dừng ở đó và không bao giờ tới trần. Chấp nhận, hay ép về trần?

| # | Phương án | |
|---|---|---|
| 1 | **Để nguyên ở điểm cân bằng.** Đó chính là "depth theo nhu cầu": −12 đủ thì −18 chỉ là mất tone. Hú bùng lại thì reinforce → đào tiếp theo cổng 300 ms | khuyên dùng |
| 2 | Ép về trần theo đồng hồ: nếu ring-risk còn ≥ RISING sau 2 s dù bin đã yên → sâu thêm một bậc | |
| 3 | Bỏ thang lúc đặt, chỉ giữ nhả dần (quay về Q2 phương án 3) | |

**Chọn: 1.**

## Q8 — Kéo slider depth (trần) xuống giữa show có retune notch Preset/Manual không, hay chỉ notch Detector? (M-2)

| # | Phương án | |
|---|---|---|
| 1 | **Chỉ Detector.** Preset/Manual có trần riêng = depth người dùng đã ghi rõ; slider không chạm | khuyên dùng |
| 2 | Slider kéo tất cả trừ Soundcheck | |

**Chọn: 1.**

## Q9 — Đóng băng nhả khi RING RISK ≥ RISING: có trần thời gian không, và có báo cho người vận hành khi slot không hiển thị đang đóng băng? (M-8)

| # | Phương án | |
|---|---|---|
| 1 | **Không trần thời gian; publish cờ `releaseFrozen` per-slot vào `SnapshotBuffer`** để GUI/log dùng sau. 1.2.0 không thêm widget | khuyên dùng |
| 2 | Trần 60 s: sau 60 s đóng băng liên tục, đồng hồ chạy lại | |
| 3 | Trần 60 s + cờ | |

**Chọn: 1.**

## Q10 — Dung sai tần số của "phòng nhớ": ±1 bin là ±21.5 Hz @44.1k/2048, một partial nhạc cụ cạnh bên có thể kế thừa depth sâu cũ ngay block đầu. (M-11)

| # | Phương án | |
|---|---|---|
| 1 | **Cùng bin (±0).** Hú quay lại cùng bin là chuyện thường; lệch một bin coi như hú mới, bắt đầu −6 | khuyên dùng |
| 2 | ±1 bin như spec ban đầu | |
| 3 | Bỏ phòng nhớ khỏi 1.2.0 | |

**Chọn: 1.**

## Q11 — `savePreset` giữa show hiện lưu bậc ĐANG ĐỨNG (có thể −6 lúc yên) và không lưu trần (`notchDefaults`). Lưu gì? (M-10)

| # | Phương án | |
|---|---|---|
| 1 | **Lưu `deepestDb`** (phòng đã cần bao nhiêu) **và** round-trip trần vào `notchDefaults.depthDB` | khuyên dùng |
| 2 | Lưu bậc đang đứng như hôm nay, thêm round-trip trần | |
| 3 | Lưu trần cho mọi notch | |

**Chọn: 1.**

## Q12 — Preset ghi depth vượt −24 (ví dụ −40) hôm nay được adopt nguyên xi. (M-1)

| # | Phương án | |
|---|---|---|
| 1 | **Kẹp về −24 lúc adopt**, ghi log; invariant "không Set nào < −24" giữ cho mọi Origin | khuyên dùng |
| 2 | Thu hẹp invariant về Origin::Detector, preset giữ nguyên hành vi | |

**Chọn: 1.**

---

**Q13 — từ đối chiếu plan (opus read-only, 2026-09-07), M-4.**

## Q13 — Trần không phải bội của 6 (preset Music ship sẵn `depth: -10`, Speech −18): thang dừng ở đâu?

Spec v2 §4.1 (sửa theo M-3 vòng 1) lượng tử về bậc nông nhất không sâu hơn
trần ⇒ Music: −6 là hết, nông hơn 1.1.3 (−10) tới 4 dB, và docs không nói.

| # | Phương án | |
|---|---|---|
| 1 | **Thang = các bậc nông hơn trần, cộng chính trần làm bậc cuối.** Music: −6 → −10; trần −13.7: −6 → −12 → −13.7. Preset được tôn trọng đúng số; Detector đứng ở giá trị lẻ chỉ ở bậc cuối | khuyên dùng |
| 2 | Giữ spec v2: lượng tử về bậc nông hơn. Music kẹt −6; phải sửa `presets/Music.json` thành −12 và ghi release note | |
| 3 | Lượng tử về bậc gần nhất. Music thành −12, **vượt trần 2 dB** — phá Q1 | |

**Chọn: 1.**

---

**Q14 — từ review Task 8 (opus, 2026-09-07), I-1.**

## Q14 — Phòng nhớ ghi đè depth khởi điểm cả hai chiều: một notch −6 chưa từng đào (hoặc Manual −3 để tự nhả) ghi nhớ "−6"/"−3", rồi hú bùng quay lại cùng bin bị KẸP nông hơn bậc −12 lẽ ra được chọn. Sửa thế nào?

| # | Phương án | |
|---|---|---|
| 1 | **Phòng nhớ chỉ được làm SÂU hơn, không bao giờ nông hơn**: `depth = min(depth chọn theo Q2, remembered)`, rồi kẹp trần như cũ. Nhớ −6 thành vô hại; nhớ −24 vẫn đặt −24 | khuyên dùng |
| 2 | Giữ spec: ghi đè nguyên xi (chấp nhận under-cut khi nhớ nông) | |
| 3 | Chỉ ghi nhớ khi `deepestDb` sâu hơn −6 (không nhớ notch chưa đào) và chỉ từ Origin Detector; vẫn ghi đè nguyên xi | |

**Chọn: 1** — owner: "chờ phản biện xong rồi hấp thụ, sang plan luôn" (2026-09-07): điều phối lấy phương án khuyên dùng.


## Chỉ thị quy trình (owner, cùng phiên)

Mọi ngả rẽ brainstorm của toàn dự án phải ghi kiểu này (câu hỏi + phương án
đề xuất + lựa chọn) để lật lại được. Viết thành skill.
