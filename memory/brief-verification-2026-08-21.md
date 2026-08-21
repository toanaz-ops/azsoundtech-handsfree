# Brief của orchestrator phải được kiểm chứng lại, kể cả bản "đã sửa"

**Ngày:** 2026-08-21 (phát hiện trong Task 10)

## Bài học

`task-10-brief.md` đã là bản **v2** — v1 sai tên API FFT của JUCE 9, orchestrator
đã grep header để sửa lại và tự tin đánh dấu "verified". Nhưng v2 vẫn còn 3 lỗi,
và cả 3 đều nằm ngoài phạm vi thứ mà v2 đi kiểm chứng (tên hàm):

1. **Sai hợp đồng kích thước buffer.** `juce_FFT.h` ghi rõ mảng truyền vào phải
   là `2 * getSize()` float. Brief cấp phát đúng `kFftSize` → tràn 4 KB mỗi lần
   gọi. Grep *tên hàm* không bao giờ phát hiện ra điều này; phải đọc docstring.
2. **Mâu thuẫn với plan.** Brief khai báo `kHopSize = 512` rồi đọc thẳng 1024
   mẫu mỗi lần (hop thật = 1024), trong khi plan dòng 114 yêu cầu overlap 50%.
3. **Assertion test sai số học.** Test Hann của brief là `1024 < 256` — không
   bao giờ pass được. Không ai tính thử trước khi viết.

## Vì sao quan trọng

Brief sai theo cách *trông có vẻ đúng* thì implementer Tier-1 sẽ code theo và
tạo ra lỗi tràn bộ nhớ im lặng. Việc "đã sửa một lần rồi" khiến người ta tin
tưởng bản v2 hơn mức đáng tin.

## Áp dụng thế nào

Trước khi code theo bất kỳ brief nào, kiểm chứng **3 lớp**, không chỉ lớp 1:

1. **Tên API** — grep header đã vendor.
2. **Hợp đồng ngữ nghĩa** — đọc docstring: kích thước mảng, đơn vị, quyền sở
   hữu, điều kiện tiền/hậu.
3. **Số học của assertion** — tính tay giá trị kỳ vọng (Python là đủ) TRƯỚC khi
   viết test. Nếu không tính ra được thì test đó chưa chứng minh điều gì cả.

Và khi brief mâu thuẫn với plan/spec thì **plan thắng** — brief chỉ là bản diễn
giải của plan.

Liên quan: [[build-verification-2026-08-21]]
