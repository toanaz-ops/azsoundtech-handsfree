# Sổ quyết định: review toàn dự án và nghiên cứu chống hú (2026-10-20)

Mỗi mục ghi một ngả rẽ: câu hỏi, các phương án, và lựa chọn.

**Trạng thái:** đang chạy. Owner (20/10/2026) yêu cầu: "Spawn 1 agent fable review toàn dự án. Spawn thêm sub agent nghiên cứu về defeedback nổi tiếng gần đây và các project tương tự (ví dụ isovoice…), cần thiết thì dùng REA repo để nghiên cứu. Làm cho tới khi xong không hỏi tôi, tự quyết định + ghi lại trong sổ quyết định của dự án."

Mọi mục dưới đây do điều phối **tự quyết** theo lệnh đó. Owner dò lại và đổi nếu cần.

Người chốt: Claude (điều phối). Người quyết cuối: owner (ToanAZ).

## Q1: "REA repo" là repo nào?

| # | Phương án | |
|---|---|---|
| 1 | **`toanaz-ops/rta-tool`**: tài khoản không có repo tên "REA"; `list_repos` trả 13 repo, gần nhất về tên và nội dung là `rta-tool` (real-time analyzer, public) | chọn |
| 2 | `project008-soundtech-assist` (trợ lý soundtech) | |
| 3 | Bỏ qua | |

**Chọn: 1.** Đọc **chỉ đọc**, chỉ khi nghiên cứu thấy cần. Nếu owner muốn nói repo khác thì lật lại mục này.

## Q2: Review chạy trên gì, có được sửa code không?

| # | Phương án | |
|---|---|---|
| 1 | **Review chỉ đọc trên `origin/main`** (worktree tách rời); báo cáo trước, sửa sau trong PR riêng do điều phối làm | chọn |
| 2 | Agent review tự sửa luôn | |

**Chọn: 1.** Đây là DSP âm thanh trực tiếp: một sửa chữa sai sẽ ra loa PA. Người review
và người sửa phải tách nhau. Nhánh `feat/lane-l-copilot` (PR #10) chưa lên main nên
nằm ngoài phạm vi.

## Q3: Xử lý phát hiện của review thế nào?

| # | Phương án | |
|---|---|---|
| 1 | **BLOCKER/IMPORTANT có đường lỗi cụ thể → sửa, mỗi nhóm một PR, kèm test và mức level dự đoán. MINOR → gom vào sổ việc treo, không sửa vội.** Sửa DSP thì vẫn theo luật CLAUDE.md: nêu mức thay đổi, chạy full build + ctest | chọn |
| 2 | Sửa tất cả trong một PR | |
| 3 | Chỉ báo cáo | |

**Chọn: 1.**

## Q4: Kết quả nghiên cứu để ở đâu?

| # | Phương án | |
|---|---|---|
| 1 | **`docs/research/2026-10-20-defeedback-landscape.md`** cùng một ghi chú `memory/`; khuyến nghị nào đổi roadmap thì ghi thêm ở đây (Q5+) | chọn |
| 2 | Chỉ trả lời trong chat | |

**Chọn: 1.**

## Kết quả nghiên cứu (2026-10-20)

Tài liệu: `docs/research/2026-10-20-defeedback-landscape.md`. Proxy của phiên chặn mọi
trang ngoài GitHub, nên số liệu web lấy từ bản tóm tắt của công cụ tìm kiếm và đã gắn
nhãn mức bằng chứng. Phải kiểm lại trên toàn văn trước khi đưa vào spec. "isovoice"
**không tìm thấy**; owner gửi link nếu muốn so sánh.

Điều phối đã tự kiểm phát hiện §7 của tài liệu: `PeakinessAnalyzer.cpp:144` đặt tần số
notch đúng tâm bin (23,4 Hz/bin ở 48 kHz). Bảng sau là độ cắt thực tế tại tần số hú khi
tâm notch lệch nửa bin (11,7 Hz), cùng thiết kế RBJ đó, −18 dB, Q 30:

| Tần số | Độ cắt thực tế |
|---|---|
| 150 Hz | −1,4 dB |
| 300 Hz | −3,9 dB |
| 600 Hz | −8,0 dB |
| 1 kHz | −11,4 dB |
| 3 kHz | −16,4 dB |

## Q5: Làm khuyến nghị nào trước?

| # | Phương án | |
|---|---|---|
| 1 | **P1 trước: ước lượng tần số dưới-bin cho notch (lane F).** Không cần phần cứng; giảm chi phí tone dưới ~1 kHz, đúng hướng ràng buộc "không méo" | chọn |
| 2 | P2 (notch hẹp, Q thích nghi) trước | |
| 3 | P3 (đặc trưng phân biệt nhạc) trước | |

**Chọn: 1.** P2 chỉ an toàn khi tần số đã chính xác, nên phải đi sau P1. P3 cần nhạc
thu sẵn và nhãn D để đo tỉ lệ báo nhầm. P4 cần rig. P4 còn cần owner xác nhận cấp lại
giấy phép AGPL-3.0 cho phần dùng lại của `rta-tool`. Vì vậy P4 **chờ owner**, điều
phối không tự quyết.

Mức level dự kiến của P1:
- Ngoài tần số hú: không đổi (cùng Q, cùng depth).
- Tại tần số hú: độ cắt thực tế tiến về depth đã đặt. Ví dụ ở 300 Hz, khi tâm lệch nửa
  bin, độ cắt đi từ −3,9 dB về khoảng −18 dB.
- Thang lane G có thể dừng ở bậc nông hơn, tức ít cắt tone hơn.
- Phải kẹp độ lệch |Δf| ≤ 0,5 bin và có NaN-guard.

Sau review, BLOCKER/IMPORTANT (Q3) **đi trước P1**.

## Kết quả review (2026-10-20)

Báo cáo: `docs/superpowers/audits/2026-10-20-full-project-review.md`. Review chạy trên
`origin/main` @ `f2cc11e`, chỉ đọc, với model Fable. Kết quả: 1 BLOCKER, 4 IMPORTANT,
10 MINOR. Điều phối đã tự kiểm lại từng mục B1, I1, I2 trước khi sửa: đọc code, đọc
mã nguồn JUCE ASIO cho I1, và viết test đỏ trước khi sửa.

## Q6: Sửa mục nào, theo nhóm nào?

| Mục | Quyết định | PR |
|---|---|---|
| B1 detector cứng 48 kHz | sửa: `setSampleRate` cạnh `setWidth` | #13 |
| I2 Bypass ghi đè | sửa: cộng dồn như Auto | #14 |
| I1 restart do driver | sửa: hook `onDeviceStopping/onDeviceStarted` trên engine | #16 |
| I3 scorer dùng hằng 10 | **sửa**, xem Q7 | riêng |
| I4, M2, M9 docs | sửa | docs |
| M1 Q preset không chặn, M6 NaN-heal làm kẹt ramp | sửa | an toàn |
| M4d grep-guard `src/dsp`, M7 comment stale, M10 curl Linux, logstats bản ghi mồ côi | sửa | hygiene |
| M5 CI Linux + sanitizer | sửa: thêm job | CI |
| M3 test tự gọi `setSampleRate(48000)` | để nguyên; test mới của B1 đi qua đường wiring thật | — |
| M8 `tools/afc` nằm trong gate | để nguyên: test của lõi nghiên cứu vẫn xanh, và tách ra không làm gate an toàn hơn | — |

## Q7: I3 có sửa không? (đổi hành vi dò)

| # | Phương án | |
|---|---|---|
| 1 | **Sửa: scorer dùng ngưỡng đang đặt cho cả gate lẫn `pNorm`** | chọn |
| 2 | Giữ nguyên, đổi nhãn/slider cho khớp (ngưỡng < 10 = 10) | |
| 3 | Chờ owner | |

**Chọn: 1.** Owner dặn "tự quyết định". Lý do:
- Ngưỡng mặc định 10 không đổi: ở ngưỡng ≥ 10, gate cho kết quả giống hệt.
- Chỉ người chọn AGGRESSIVE (ngưỡng 8), hoặc kéo slider xuống dưới 10, mới thấy khác. Đó đúng là điều họ đã chọn mà trước đây không có tác dụng.

Mức thay đổi dự kiến:
- Ngưỡng ≥ 10: 0 dB.
- Ngưỡng < 10: nhiều notch hơn, tức nhiều chỗ bị cắt hơn, ứng với hú yếu hơn.

Phải ghi vào release note để tester dùng AGGRESSIVE thử ở âm lượng nhỏ.

## Q8: Sau P1, làm tiếp P2 ngay hay dừng chờ merge?

| # | Phương án | |
|---|---|---|
| 1 | **Dừng ở P1, chờ merge.** P2 (notch hẹp, Q thích nghi) đổi Q của mọi notch và đụng ramp của lane G: đổi Q là "thiết kế mới", nên state bị reset. P2 phải xây trên P1 đã merge, và trên cả B1/I1/I2/I3/M1/M6, nếu không sẽ thành 7 PR DSP chồng lên nhau chưa ai nghe | chọn |
| 2 | Làm luôn P2 trên nhánh của P1 | |

**Chọn: 1.** Tám PR đang mở đều xanh hoặc đang chạy CI. Owner nói "merge" thì
điều phối gộp release note 1.4.0 (nhiều PR cùng chạm vào), rồi làm P2 trên `main`
mới.

## Trạng thái cuối đợt (2026-10-20)

| Mục | PR | Trạng thái |
|---|---|---|
| Sổ quyết định, khảo sát, báo cáo review | #12 | chỉ docs |
| B1: detector theo sample rate của card | #13 | CI xanh |
| I2: Bypass cộng như Auto | #14 | CI xanh |
| I1: restart do driver | #16 | CI xanh |
| I3: scorer dùng ngưỡng sống | #17 | CI xanh |
| I4, M2, M9: docs | #19 | chỉ docs |
| M1, M6: kẹp Q, NaN-heal | #23 | CI xanh |
| M4d, M7, M10, logstats | #25 | CI đang chạy |
| M5: CI Linux + ASan/UBSan | #26 | local 795/795 dưới sanitizer; CI đang chạy |
| P1: tần số notch dưới-bin | #27 | CI đang chạy |
| Còn treo, chờ owner | | P4 (giấy phép `rta-tool`), link "isovoice", nghe tất cả ở alpha |
