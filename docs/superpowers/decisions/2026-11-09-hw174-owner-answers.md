# Sổ quyết định: owner trả lời thẻ HW-174 và HW-188 (2026-11-09)

Nguồn: Decision Box (https://claude.ai/artifact/D4527GQnW7vNh2NQHSBta8), thẻ HW-174.Q1–Q5
và HW-188. Điều phối (phiên MASTER DEV0) đánh dấu "đã trả lời" theo luật owner ngày
09/11/2026: **"thấy có task chưa làm, cần làm thì làm luôn"** — mỗi thẻ nhận phương án
được khuyên. Riêng HW-188 owner viết thẳng: **"handsfree, không chờ nghe thử"**.

Người chốt: owner (ToanAZ), chuyển qua điều phối. Người ghi: phiên fix-round (Opus).

## HW-188 (Q11 handoff 09/11): đóng gói 1.4.0 ngay hay chờ nghe rig?

| # | Phương án | |
|---|---|---|
| A | Đóng gói 1.4.0 từ `main` ngay, `-Keep 0` (không xoá bản cũ) | chọn |
| B | Chờ buổi nghe rig HW-173 | |

**Chọn: A.** Đã làm: `release-alpha.ps1 -Part minor -Keep 0` trên `main` `2628cdf`,
ctest 833/833, PR #42.

## HW-174.Q1: làn quay lại sau mono→stereo có reset đủ lịch sử?

**Chọn: A — không đổi** (giữ như 1.1.1).

## HW-174.Q2: detector nghe từng làn riêng hay tổng của kênh ra?

| # | Phương án | |
|---|---|---|
| A | Nghe từng làn riêng (sửa code cho khớp tài liệu `AudioEngine.h`) | chọn |
| B | Giữ nghe tổng, chỉ sửa tài liệu | |

**Chọn: A, làm NGAY** — owner bỏ điều kiện "sau buổi nghe rig" của khuyến nghị gốc.
Nguồn lỗi: audit 2026-10-21 "[SEV med] Per-lane tap on a shared output carries the SUM".

## HW-174.Q3: sửa một slot thì reset slot đó hay cả 8?

| # | Phương án | |
|---|---|---|
| A | Chỉ reset slot vừa sửa; lưu settings một lần mỗi lô | chọn |
| B | Giữ reset cả 8 (luật A-R4 hiện tại) | |

**Chọn: A.** Đổi luật A-R4 trong `docs/spec-ring-risk.md`: ranh giới reset của MỌI slot
vẫn là đổi thiết bị / đổi sample rate; sửa routing một slot chỉ reset slot đó. Gộp review
2026-11-09 MEDIUM-3 và LOW-5 (dừng 8 detector ngay trong `audioDeviceStopped()`).

## HW-174.Q4: neo độ nhạy theo ngưỡng đang đặt (PR #17)?

**Chưa chốt — vẫn ở owner.** PR #17 giữ nguyên.

## HW-174.Q5: chấp nhận cả lô quyết định điều phối tháng 9–10?

**Chọn: A — chấp nhận cả lô.**
