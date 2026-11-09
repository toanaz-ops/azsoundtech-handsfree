# Sổ quyết định — Nhúng AI: LLM trong app và mạng realtime (2026-11-09)

Mỗi mục là một ngả rẽ. Ghi đủ: câu hỏi, các phương án (kèm phương án khuyên
dùng), và lựa chọn.

**Trạng thái:** **Q1–Q6 owner đã chốt 09/11/2026** (trả lời trực tiếp trong
phiên). Q7–Q10, Q12–Q14 điều phối chốt tạm, **owner duyệt 09/11/2026 (Q16)**. **Q11 owner đã chốt
09/11/2026 (phương án 1)**; ba PR lane AI đã merge `main` cùng ngày (Q15).
Spec: [`../specs/2026-11-09-copilot-panel-design.md`](../specs/2026-11-09-copilot-panel-design.md) (lane L2),
[`../specs/2026-11-09-candidate-feature-log-design.md`](../specs/2026-11-09-candidate-feature-log-design.md) (lane C0).
Nghiên cứu nền: [`../../research/2026-11-09-ai-integration-llm-and-realtime.md`](../../research/2026-11-09-ai-integration-llm-and-realtime.md).
Người hỏi: Claude (`claude-fable-5-1`). Người quyết: owner (ToanAZ).
Kế thừa: Q1–Q5 của [`2026-10-20-lane-l-copilot.md`](2026-10-20-lane-l-copilot.md) (chốt tạm, owner chưa duyệt).

## Q1 — LLM tới tay soundman bằng đường nào?

| # | Phương án | |
|---|---|---|
| A | Giữ CLI offline như hôm nay | |
| B | **Tiến trình phụ đóng gói từ `tools/copilot`, app khởi chạy khi bấm nút, đọc file log JSONL sẵn có** | khuyên dùng |
| C | Trong tiến trình app, luồng nền, HTTP thô qua `juce::URL` | |
| D | LLM chạy tại máy (llama.cpp / Ollama) | |

Được/mất từng phương án: nghiên cứu §2.3. Điểm quyết định: B giữ lời gọi mạng
ngoài tiến trình đang ra loa PA; C gọn hơn nhưng đặt chúng chung nhà.
Không phương án nào không đảo ngược được.

**Chọn: B.** Owner, 09/11/2026 ("Q1 B").

## Q2 — Khoá API đặt ở đâu?

| # | Phương án | |
|---|---|---|
| 1 | Người dùng tự nhập khoá của họ | đủ cho thử nội bộ |
| 2 | **Máy chủ trung gian của AZ giữ khoá** | khuyên dùng khi ra ngoài |
| 3 | Nhúng khoá vào file cài | loại: khoá lấy ra được |

Hệ quả của 2: AZ trả tiền theo lượt hỏi (ước lượng $0,10–0,30 mỗi câu với
`claude-opus-5-5`, sàn $0,05, chưa đo), phải vận hành một dịch vụ, và log buổi diễn đi
qua máy chủ AZ nên phải nói rõ với người dùng. Đây là cam kết thường xuyên,
khó rút lại sau khi người dùng đã quen.

**Chọn: 1, kèm yêu cầu mới.** Owner, 09/11/2026: "Q2 1 (cho phép cắm nhiều
model khác nhau)". Người dùng tự nhập khoá, và phải chọn được model. Nghĩa cụ
thể của "nhiều model" ghi ở Q7.

## Q3 — Bước đầu có cần LLM không?

| # | Phương án | |
|---|---|---|
| 1 | **Đưa luật cố định lên Geek mode trước (không LLM, không mạng); "hỏi Claude" là bước sau** | khuyên dùng |
| 2 | Làm cả hai một lần | |

Phương án 1 không cần quyết Q1/Q2 để bắt đầu, và cho biết soundman có đọc lời
khuyên hay không trước khi chi tiền cho LLM.

**Chọn: 1.** Owner, 09/11/2026 ("theo khuyến nghị").

## Q4 — Lane C lấy dữ liệu huấn luyện từ đâu?

| # | Phương án | |
|---|---|---|
| 1 | Chờ ≥ 300 nhãn từ alpha (cổng hiện tại của roadmap) | |
| 2 | **Chế độ bóng ngay: ghi đặc trưng và điểm của mạng cạnh quyết định của scorer, mạng không điều khiển notch; dữ liệu tổng hợp chỉ để khởi động** | khuyên dùng |
| 3 | Huấn luyện hoàn toàn bằng dữ liệu tổng hợp rồi cho mạng quyết định | loại: tín hiệu tổng hợp không đại diện nhạc thật |

Phương án 2 không đổi tiếng và không đổi hành vi notch, nên không cần buổi
nghe riêng; phương án 1 an toàn nhất nhưng đang chờ dữ liệu không ai sinh ra.

**Chọn: 2.** Owner, 09/11/2026 ("theo khuyến nghị").

## Q5 — Thư viện suy luận cho lane C?

| # | Phương án | |
|---|---|---|
| 1 | **RTNeural** (BSD-3, C++ thuần, mạng nhỏ) | khuyên dùng |
| 2 | ONNX Runtime | |
| 3 | anira | để dành cho sản phẩm thứ hai |

Phải tự kiểm tính "không cấp phát bộ nhớ lúc chạy" của RTNeural trên mã nguồn
trước khi chốt. Lớp nạp model là thư viện dùng chung (luật `CLAUDE.md`).

**Chọn: 1.** Owner, 09/11/2026 ("theo khuyến nghị"). Điều kiện kiểm mã nguồn
vẫn giữ: nếu RTNeural cấp phát lúc chạy thì quay lại hỏi owner.

## Q6 — Thứ tự so với buổi nghe 1.4.0?

| # | Phương án | |
|---|---|---|
| 1 | **Giữ đề xuất 21/10: buổi nghe trước, AI sau; riêng việc chạy `--ask` thật một lần thì làm ngay khi có khoá** | khuyên dùng |
| 2 | Bắt đầu lane AI song song | |

**Chọn: 2.** Owner, 09/11/2026: "làm luôn, giả định đã pass soundcheck". Ngược
khuyến nghị. Ghi rõ: buổi nghe được **giả định** là đã qua theo lời owner;
phiên này không có bằng chứng nào về buổi nghe đó. Hệ quả owner chấp nhận: nếu
một bản alpha sau này có lỗi nghe được, khó quy về lane nào hơn.

## Q7 — "Cắm nhiều model khác nhau" nghĩa là gì?

Sinh ra từ câu trả lời Q2. Owner chưa nói rõ; điều phối chốt tạm để không chặn
việc.

| # | Phương án | |
|---|---|---|
| 1 | Chỉ các model Claude, chọn trong danh sách | |
| 2 | **Hai loại kết nối: Claude qua SDK chính thức, và bất kỳ dịch vụ nào nói giao thức tương thích OpenAI (địa chỉ + tên model + khoá) — gồm OpenAI, OpenRouter, Gemini, và Ollama chạy tại máy** | khuyên dùng |
| 3 | Viết kết nối riêng cho từng nhà cung cấp | |

Phương án 2 phủ gần hết thị trường bằng hai đường mã, và cho cả lựa chọn chạy
tại máy không cần mạng. Mất: model yếu trả lời kém và dùng tool kém; ràng buộc
"không khuyên tăng gain" chỉ nằm trong prompt nên với model lạ phải có thêm
một bước lọc câu trả lời bằng mã.

**Chọn: 2 (điều phối chốt tạm, 09/11/2026).** Lật lại khi owner nói khác.

## Q8 — Khoá người dùng nhập được cất ở đâu?

App hôm nay không có chỗ cất bí mật nào.

| # | Phương án | |
|---|---|---|
| 1 | Chữ thường trong `settings.json` | loại: ai mở file cũng đọc được |
| 2 | **Mã hoá DPAPI theo tài khoản Windows, ghi file riêng; tiến trình phụ tự giải mã** | khuyên dùng |
| 3 | Windows Credential Manager | |
| 4 | Không cất, chỉ đọc biến môi trường `ANTHROPIC_API_KEY` | |

Phương án 2: khoá không đi qua dòng lệnh, biến môi trường hay log. Giới hạn phải nói
thật: DPAPI không chặn phần mềm độc hại chạy dưới chính tài khoản đó. Phương án 3 tương
đương về bảo mật nhưng khó đọc từ tiến trình phụ Python hơn. Phương án 4 không có giao
diện nhập nên trái với Q2.

**Chọn: 2 (điều phối chốt tạm, 09/11/2026).** Chỉ áp dụng từ bước L2b.

## Q9 — Bảng copilot nằm ở đâu trong Geek mode?

| # | Phương án | |
|---|---|---|
| 1 | **Nửa dưới cột trái, dưới danh sách notch** (vùng đang trống) | khuyên dùng |
| 2 | Ngăn kéo mở bằng nút trên thanh trên cùng | |
| 3 | Cửa sổ riêng | |

**Chọn: 1 (điều phối chốt tạm, 09/11/2026).** Xem lại trên ảnh render.

## Q10 — Lane C bắt đầu bằng gì?

Q4 và Q5 nói "chế độ bóng" và "RTNeural". Chưa có nhãn thì chưa có model để chạy.

| # | Phương án | |
|---|---|---|
| 1 | **C0 chỉ ghi đặc trưng của ứng viên bị từ chối và bổ sung trường còn thiếu; không suy luận, không thêm thư viện. RTNeural vào ở C1** | khuyên dùng |
| 2 | Thêm RTNeural ngay với một model giả | |

Phương án 1 là phần đầu của đúng thứ owner chọn ở Q4, và không kéo thư viện mới vào
build khi chưa có gì cho nó chạy.

**Chọn: 1 (điều phối chốt tạm, 09/11/2026).**

## Q11 — Bản alpha kế tiếp mang những gì? **OWNER ĐÃ CHỐT: 1**

`main` đang ở version 1.3.0; `docs/release-notes/1.4.0-alpha.md` ghi "chưa đóng gói".
Bản alpha kế tiếp vì vậy sẽ là bản **đầu tiên** mang lane U cùng bốn bản vá audio path
đang nằm trong `docs/release-notes/unreleased/` (BYPASS cộng output dùng chung, NaN heal,
lệnh notch sống qua lúc mở thiết bị, các lỗi mức thấp của engine/detector). Q6 giả định
"đã qua soundcheck", nhưng chưa có bản nào chứa các bản vá đó từng rời máy.

| # | Phương án | Được | Mất |
|---|---|---|---|
| 1 | **Đóng gói 1.4.0 ngay từ `main` hôm nay, trước khi merge lane AI** | tester nhận các bản vá an toàn sớm nhất; lỗi nghe được quy về đúng nhóm bản vá | thêm một lần phát hành |
| 2 | Gộp: merge L2a rồi mới đóng gói | một lần phát hành | bản vá BYPASS tới tay tester muộn hơn; lỗi khó quy hơn |
| 3 | Chưa phát hành gì | không rủi ro mới | tester tiếp tục dùng 1.3.0 có lỗi BYPASS đã biết |

Phát hành là thao tác tới tay người khác và tới loa PA; điều phối không tự chạy.

**Chọn: 1 (owner, 09/11/2026).** Owner trả lời ở phiên khác ("handsfree, không chờ nghe
thử", thẻ HW-188 = A theo handoff chung); 1.4.0 đã đóng gói từ `main` `2628cdf` trước khi
lane AI merge, sau đó 1.4.1. Cả hai bản đều **không** gồm lane AI.

## Q15 — Merge ba PR lane AI (#37, #38, #40) khi nào?

Hỏi sau khi cả ba đã gộp `main` và tích hợp với nhau local (439 test hẹp C++ + 102 test
Python xanh trên cây gộp đủ). Lúc hỏi repo còn phiên khác đang sống.

| # | Phương án | Được | Mất |
|---|---|---|---|
| 1 | **Merge khi CI xanh** | lane AI vào `main`, hết xếp chồng, hết xung đột lặp lại | bản alpha kế tiếp sẽ mang lane AI |
| 2 | Chưa merge | `main` giữ nguyên | mỗi lần `main` đổi lại phải gộp ba nhánh |

**Chọn: 1 (owner, 09/11/2026).** Nguyên văn: "Merge khi CI xanh", rồi "Sau không hỏi câu
này nữa, cứ merge thoải mái." CI `windows` xanh trên cả ba head (12m0s, 15m2s, 15m53s);
merge `66aa063` (#37), `2d189d6` (#38), `8f048a7` (#40). Chưa đóng gói bản nào mang lane AI.

## Q12 — Ô "hỏi AI" và phần cài đặt nằm ở đâu?

Bảng COPILOT chỉ cao tối thiểu 144 px; phần cài đặt (nhà cung cấp, model, địa chỉ, khoá,
ô đồng ý) cần khoảng sáu hàng.

| # | Phương án | Được | Mất |
|---|---|---|---|
| 1 | **Một hàng hỏi ở đáy bảng COPILOT; nút `CÀI AI` mở một lớp phủ che cột trái (danh sách notch + bảng COPILOT) cho tới khi đóng** | không mở cửa sổ mới; chụp ảnh render được; bảng vẫn gọn | lúc đang cài thì không thấy danh sách notch |
| 2 | Cửa sổ hộp thoại riêng | không che gì | hộp thoại async khó test headless, không chụp được bằng snapshot |
| 3 | Đặt vào DevicePanel | cùng chỗ với cài đặt khác | DevicePanel là của thiết bị âm thanh; lẫn hai việc |

Lớp phủ chỉ mở khi người dùng bấm, và nút `ĐÓNG` luôn thấy. Chế độ SOUNDCHECK/AUTO vẫn
chạy bên dưới; lớp phủ không chặn nút BYPASS ở cột phải.

**Chọn: 1 (điều phối chốt tạm, 09/11/2026).**

## Q13 — Bộ lọc câu trả lời nghiêng về phía nào khi không chắc?

Soát độc lập cho thấy bộ lọc cụm từ ban đầu để lọt "tăng âm lượng lên", "đẩy fader lên",
"turn it up", "+3 dB", tiếng Việt không dấu và chữ chèn ký tự ẩn.

| # | Phương án | Được | Mất |
|---|---|---|---|
| 1 | **Chuẩn hoá chữ (NFKC, bỏ dấu, bỏ ký tự ẩn) rồi bắt "động từ tăng + đối tượng mức" trong cửa sổ ngắn; không chắc thì chặn** | lọt ít hơn nhiều | đôi khi chặn oan một câu vô hại; người dùng thấy câu từ chối cố định và phải hỏi lại |
| 2 | Giữ danh sách cụm từ cố định | không chặn oan | lọt các cách nói thường gặp |
| 3 | Dùng một lượt gọi model thứ hai để chấm | bắt được diễn đạt lạ | tốn tiền, chậm, và chính nó cũng sai được |

Chặn oan tốn một câu trên màn hình; để lọt thì lời khuyên tăng gain tới một dàn PA.
Phủ định ("không nên tăng gain", "đừng tăng âm lượng") vẫn được cho qua.

**Chọn: 1 (điều phối chốt tạm, 09/11/2026).** Đây vẫn là bộ lọc, không phải bảo đảm.

## Q14 — Khi hỏi thất bại, app hiện gì?

| # | Phương án | Được | Mất |
|---|---|---|---|
| 1 | **Tiến trình phụ in lý do (đã gỡ khoá) dạng JSON `{"error": …}`; bảng hiện đúng câu đó** | tester phân biệt được sai khoá, mất mạng, sai model | thêm một nhánh hợp đồng giữa hai phía |
| 2 | Một câu chung "không hỏi được" | đơn giản | tester không biết sửa gì |

**Chọn: 1 (điều phối chốt tạm, 09/11/2026).** Kèm theo: địa chỉ `http://` không phải máy
này bị từ chối khi có khoá sẽ gửi đi (khoá không đi dạng chữ thường qua mạng).

## Q16 — Các điểm chốt tạm, khoá thử, và thời điểm làm lane C1

Hỏi qua trang quyết định (https://claude.ai/artifact/FATBschBPYtkNChuK9Jj8A) và trong chat. Ba câu
gộp: (a) giữ hay đổi Q7–Q10, Q12–Q14; (b) thử lượt hỏi AI thật bằng khoá nào; (c) lane C1 (mạng
neuron trên luồng detector) làm khi nào.

| Câu | Phương án | Được | Mất |
|---|---|---|---|
| (a) | **Giữ cả bảy điểm chốt tạm** | không phải làm lại gì | không |
| (a) | Đổi từng điểm | theo ý owner | làm lại phần tương ứng của L2a/L2b |
| (b) | **A — khoá Anthropic của owner** | đo được độ trễ, token, lỗi thật trên đường chính | tốn tiền theo lượt gọi; owner phải tự nhập khoá |
| (b) | B — Ollama tại máy | không tốn tiền, không gửi dữ liệu ra ngoài | chỉ kiểm đường tương thích OpenAI, không kiểm đường Claude |
| (b) | C — chưa thử | không | lượt gọi thật vẫn là chưa biết |
| (c) | Chờ ≥ 300 verdict từ ≥ 3 session rồi mới mở C1 (roadmap cũ) | không viết mã trước khi có dữ liệu | khi dữ liệu về mới bắt đầu làm khung |
| (c) | **Làm khung C1 ngay** | khung suy luận, test đếm cấp phát, chế độ bóng sẵn sàng khi dữ liệu về | model huấn luyện thật vẫn phải chờ dữ liệu; thêm CPU trên luồng detector |

**Chọn (owner, 09/11/2026):** (a) giữ, (b) A, (c) làm ngay. Nguyên văn: "Tất cả chốt theo khuyến
nghị, riêng vụ mạng neuron được tôi muốn làm luôn". Cùng lượt, owner cho đóng gói bản mang lane AI
("Đóng gói 1.5.0"): 1.5.0 đóng gói từ `main` `5d47b9e`, xem `docs/release-notes/1.5.0-alpha.md`.

Hệ quả: khoá do owner tự nhập trong lớp phủ `CÀI AI`; điều phối không nhập, không đọc khoá. C1 vẫn
phải thoả năm điều kiện của Q5 (`docs/research/2026-11-09-rtneural-realtime-audit.md`) và chỉ chạy
ở chế độ bóng: không tác động vào notch, mức ra dự kiến đổi 0 dB.
