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
- TTL "phòng nhớ" sau Clear: **5 phút**, khớp ±1 bin, mục dùng một lần.

**Chọn: giữ cả hai** (owner: "ok, tiếp phần 2").

## Chỉ thị quy trình (owner, cùng phiên)

Mọi ngả rẽ brainstorm của toàn dự án phải ghi kiểu này (câu hỏi + phương án
đề xuất + lựa chọn) để lật lại được. Viết thành skill.
