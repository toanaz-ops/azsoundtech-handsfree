# Bài học từ lane S (stereo-aware detection) — 2026-09-05

**Bối cảnh:** lane S tách detector theo làn (INDEP mặc định, LINK tùy chọn),
10 task, 24 commit, suite 366 → 397. Spec:
`docs/superpowers/specs/2026-09-05-stereo-aware-detection-design.md`.
Quy trình: spec → phản biện độc lập → plan → sub-agent mỗi task → review
mỗi task → review toàn nhánh. Ba lần review bắt được lỗi mà test xanh không
bắt.

## 1. Reset bộ đếm persistence chéo làn suy biến khi ngưỡng = 1

Ở LINK, làn 0 confirm → đặt cặp → reset `persistence[bin]` của làn 1 về 0.
Với `Persist = 1` (AGGRESSIVE, chọn được ở hai combo), làn 1 tăng 0→1 ≥ 1
ngay trong cùng lần lặp và đặt cặp thứ hai: **hai notch −18 dB chồng nhau
ở một tần số, chain đầy nhanh gấp đôi**. Review toàn nhánh mới thấy; review
theo task đã duyệt vì test chỉ chạy ở Persist = 3.

**Cách đúng:** dấu `linkedPlacedAt_[bin±1] = drainIteration_` ghi sau khi
đặt thành công; làn xử lý sau trong cùng lần lặp bỏ qua bin đó. Reset bộ
đếm vẫn giữ cho làn xử lý *trước* (ngưỡng ≥ 2). Hai cơ chế, hai hướng thời
gian, phải ghi rõ quan hệ trong comment.

**Áp dụng cho lane G (nhả dần) và mọi logic "một sự kiện, hai làn":** luôn
test ở cực trị của tham số runtime (persistence 1, threshold min, depth
max), không chỉ ở mặc định.

## 2. Test tắt detection để "đóng băng placement" cũng tắt reinforcement

Hai test auto-release trong brief gọi `setDetectionActive(false)` rồi mong
notch còn ring sống qua 30 s. Không thể: cổng `detectionActive_` đứng trước
cả vòng cập nhật `lastDetectedMs`. Implementer đầu đã *dời cổng* để test
xanh, tức đổi ngữ nghĩa KD-9 (Bypass không được đặt và không được nuôi
notch). Phán quyết: spec thắng, sửa test (giữ detection bật, chỉ đếm
lệnh Clear). **Khi test và code mâu thuẫn, nghi test trước; brief do người
viết cũng sai được.**

## 3. Ảnh render bắt hai lỗi mà 396 test xanh bỏ qua (lần thứ hai)

Header "LANE" vẽ thành "LA…" (cột đo theo ô, không đo theo caption) và
flag notch R bị nuốt vì logic chống chồng flag giả định marker đến theo
thứ tự tần số, điều không còn đúng khi danh sách xếp theo (làn, index).
Lặp lại bài học 2026-08-25: **GUI không báo cáo nếu chưa nhìn ảnh.**

## 4. Reviewer tính toán được thì phải bắt tính

Cột HELD ở 360 px còn 60 px sau khi thêm LANE 30 px; "127m ago" không
vừa; comment nói font 13 px trong khi font thật 14 px. Sửa: hằng số cột
công khai + `statusWidthFor(width)` dùng chung cho `paint()` và test, test
đo bằng font thật với ô rộng nhất mà formatter *có thể* in ("23.9 kHz",
"−150.0 dB"), không phải ô "điển hình". `preallocateSpace` của JUCE đếm
float (3/phần tử), không đếm phần tử; stem đứt nét cao 333 px cần 288 phần
tử, đặt 256 là cấp phát trên paint thread.

## 5. Quy trình

- SendMessage không có trong harness này: fix round = dispatch agent mới
  mang brief + report + finding. Hoạt động tốt; report file là bộ nhớ.
- Tách release khỏi task cuối: build chưa qua review toàn nhánh không
  được tới tay tester. Review cuối tìm ra Important #1 ở trên.
- Ledger `.superpowers/sdd/<plan>/progress.md` là thứ sống sót qua
  compaction; mọi phán quyết ghi vào đó.
