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

## Lane C1 — Q17–Q22 (điều phối chốt tạm 09/11/2026, owner chưa duyệt; trừ Q22 chờ owner)

Spec: [`../specs/2026-11-09-candidate-classifier-design.md`](../specs/2026-11-09-candidate-classifier-design.md).
Owner đã bảo làm C1 ngay (Q16); các câu dưới là ngả rẽ bên trong lane.

### Q17 — Chưa có dữ liệu thật thì C1 mang model nào?

| # | Phương án | Được | Mất |
|---|---|---|---|
| 1 | Không trọng số: đường suy luận chỉ chạy trong test với model mẫu | không rủi ro ngoài hiện trường | không biết CPU và độ ổn định trên máy tester |
| 2 | Model `synthetic-v0` (nhãn tổng hợp) chạy bóng trong app | chứng minh runtime trên máy tester (p99, không sập); chấm lại offline được | score không có nghĩa về nhạc thật, dễ bị đọc nhầm |
| 3 | Trọng số đặt tay bắt chước `score` hiện tại | không | tương quan hoàn toàn với `score`, không thêm thông tin |

**Chọn: 1 ở bước C1a, 2 ở bước C1b.** 2 chỉ làm kèm việc ghi score vào cả `notch_set`.

**Sửa 09/11/2026 (sau khi C1a merge, điều phối chốt tạm):** ở C1b dùng phương án 4 thay cho 2 — chạy bóng bằng chính bộ trọng số mẫu **chưa huấn luyện**, mang tên `untrained-fixture-v1`. Lý do: model tổng hợp và model chưa huấn luyện cho cùng một thứ đo được (CPU p99, không sập, chấm lại offline được), còn model tổng hợp đòi một nguồn "nốt nhạc giả" mà `memory/peakiness-sweep-2048-2026-09-04.md` đã ghi là không đại diện cho nhạc thật, và cái tên `synthetic-v0` dễ bị đọc thành "đã huấn luyện". Script huấn luyện để tới khi có `candidate_features` + verdict thật.

### Q18 — `CandidateClassifier` nằm ở app hay thư viện dùng chung?

Q5 ghi "lớp nạp model là thư viện dùng chung"; `CLAUDE.md` cấm tách khi chưa có sản phẩm thứ hai nêu tên.

| # | Phương án | Được | Mất |
|---|---|---|---|
| 1 | **Ở `src/app` bây giờ** | đúng luật "không tách phòng xa"; ít file | khi có sản phẩm thứ hai phải dời |
| 2 | Tách `libs/` ngay | khớp câu chữ Q5 | thư viện chỉ một nơi dùng |

**Chọn: 1.** Lệch câu chữ Q5 một cách có ghi lại; owner lật được.

### Q19 — Tên trường log

| # | Phương án | Được | Mất |
|---|---|---|---|
| 1 | **`nn_score`** | vẫn đúng khi sau này mạng được quyền quyết định | khác tên spec C0 đã viết |
| 2 | `shadow_score` (spec C0) | khớp spec cũ | phải đổi tên khi hết chạy bóng, vỡ công cụ đọc log |

**Chọn: 1.**

### Q20 — Đổi model khi app đang chạy?

| # | Phương án | Được | Mất |
|---|---|---|---|
| 1 | **Chỉ nạp khi mọi luồng detector đang dừng** | không model đang chạy nào bị ghi; không thêm cơ chế đồng bộ | muốn đổi model phải dừng / khởi động lại |
| 2 | Hộp thư nguyên tử (publish / adopt / retire) | đổi giữa buổi diễn | ~80 dòng đồng bộ + test TSan; chưa ai cần |

**Chọn: 1.** Thoả ý của điều kiện 4 trong bản soát RTNeural (không ghi vào model đang chạy).

### Q21 — Tên bước

Spec C0 gọi "C1 = soát + huấn luyện + lớp nạp, C2 = chạy bóng". **Chọn:** lane này là C1, chia C1a
(runtime + cổng Q5) và C1b (chạy bóng, tức "C2" cũ); huấn luyện trên dữ liệu thật để sau.

### Q22 — File ghi công bên thứ ba trong bộ cài **CHỜ OWNER**

Bộ cài hôm nay không kèm file ghi công nào. BSD-3 của RTNeural đòi kèm thông báo bản quyền khi mã
nằm trong binary (từ C1a); melatonin_blur (MIT) và các gói Python trong `HandsFreeCopilot.exe` đã ở
tình trạng này từ trước.

| # | Phương án | Được | Mất |
|---|---|---|---|
| 1 | **Thêm `THIRD-PARTY-NOTICES.txt` vào bộ cài trước bản phát hành kế tiếp** | đúng điều khoản giấy phép | một PR nhỏ đụng `handsfree.nsi`; phải liệt kê cả gói Python |
| 2 | Để sau | không việc thêm | bản phát hành kế tiếp mang RTNeural mà không có thông báo |

**Chọn:** chờ owner. Khuyên 1.

## Lane C1c — Q23–Q25 (điều phối chốt tạm 09/11/2026, owner chưa duyệt)

Owner bảo (qua phiên điều phối MASTER DEV0, 09/11/2026) làm tiếp lane C1: đường huấn luyện, bộ đánh giá
có tập giữ lại, test cổng; giữ chế độ bóng tới khi cổng đạt. Spec §8 của
[`../specs/2026-11-09-candidate-classifier-design.md`](../specs/2026-11-09-candidate-classifier-design.md).

### Q23 — Lấy nhãn từ đâu?

| # | Phương án | Được | Mất |
|---|---|---|---|
| 1 | **Chỉ notch có verdict** (TRUE = hú thật, FALSE = đặt oan) | nhãn do người chấm; đúng bài toán "lọc notch oan" | ít dữ liệu; không học được hú bị bỏ sót |
| 2 | Thêm ứng viên suýt đạt làm nhãn âm | nhiều dữ liệu hơn | suýt đạt có thể là hú thật bị bỏ sót: nhãn sai, dạy mạng bỏ qua hú |

**Chọn: 1.**

### Q24 — Ngưỡng của cổng để một model được nhúng vào app (vẫn chạy bóng)

| # | Phương án | Được | Mất |
|---|---|---|---|
| 1 | **Dữ liệu thật ≥ 300 verdict từ ≥ 3 session; AUC ≥ 0,80 và ≥ AUC của `score` + 0,05; giữ ≥ 95 % hú thật thì bắt ≥ 30 % đặt oan** | khớp mốc 300 verdict đã ghi ở roadmap; đòi mạng hơn hẳn luật hiện tại | các con số là ước đoán khi chưa có dữ liệu |
| 2 | Chỉ đòi AUC ≥ mốc `score` | dễ đạt | model không hơn gì luật hiện tại vẫn được nhúng |
| 3 | Không cổng, người xem báo cáo rồi quyết | linh hoạt | không có gì chặn một model tổng hợp lọt vào app |

**Chọn: 1.** Các ngưỡng xem lại khi có lứa dữ liệu thật đầu tiên. Qua cổng chưa phải là được quyết
định notch: bước đó cần buổi nghe và owner chốt.

**Sửa 09/11/2026** (sau kiểm chứng độc lập của C1c; thêm điều kiện, không nới điều kiện nào): (a) tập giữ lại cần ≥ 10 dòng **mỗi nhãn**;
(b) AUC và mọi tỉ lệ trong [0, 1], bộ đếm là số nguyên không âm; (c) ngưỡng "giữ 95 % hú thật" cũng được chọn trên các session huấn luyện rồi áp lên tập giữ lại,
và ở đó phải giữ ≥ 90 % hú thật và bắt ≥ 30 % đặt oan (con số chọn ngay trên tập giữ lại chỉ là số lạc quan, vẫn phải ≥ 95 % / ≥ 30 %);
(d) manifest và metadata huấn luyện bắt buộc, băm dataset (báo cáo = manifest = metadata) và băm trọng số phải khớp; (e) log không có `session_start` là nguồn
`unknown`; (f) `untrained-*` chỉ được phép khi id và băm file nằm trong danh sách ghim. Lưu ý: `source` là nhãn tự khai, cổng chống tai nạn chứ không chống giả mạo.

**Sửa 09/11/2026 (sau Fable review)** — **chốt tạm, owner chưa duyệt**; thêm sàn thống kê, không nới điều kiện nào. Lý do: với 10 dòng mỗi nhãn ở tập giữ lại, sai số chuẩn của AUC
(Hanley–McNeil) khoảng 0,09 nên "mốc + 0,05" nằm trong nhiễu, và với 3 session phép chia là fit 1 / kiểm định 1 / giữ lại 1. Các sàn mới, ở `gate.THRESHOLDS`:
(a) tập giữ lại ≥ **30** dòng mỗi nhãn (thay 10); (b) tập giữ lại phủ ≥ **2 session**; (c) tổng ≥ **6 session** (thay 3); (d) cận dưới của khoảng tin cậy 95 % của AUC model
(`AUC ± 1,96·SE`) ≥ AUC của `score` hiện tại. Còn nguyên: ≥ 300 hàng thật, ≥ 60 dòng giữ lại, AUC ≥ 0,80, AUC ≥ mốc + 0,05, và hai cặp ngưỡng 95 % (lạc quan và trung thực).
Cùng lượt: (e) `real` cần bằng chứng dương từ `session_start` (không còn "thiếu `source` = `real`"); (f) danh tính session theo nội dung, bản trùng bị bỏ;
(g) một lần đặt LINK là một hàng; (h) test cổng kiểm đúng các file CMake nhúng và **trượt** khi máy không có Python. Các con số (a)–(d) là ước đoán trước khi có dữ liệu thật,
cần owner xem lại khi có lứa log thật đầu tiên: 30 dòng/nhãn và 6 session có thể đòi nhiều hơn số tester hiện có.

### Q25 — Chưa có log thật thì khởi động bằng gì?

Đo 09/11/2026: máy build có 9 file log, không notch, không verdict.

| # | Phương án | Được | Mất |
|---|---|---|---|
| 1 | **Tập tổng hợp có ghi rõ cách sinh, đúng định dạng log; model `synthetic-bootstrap-v0` không nhúng vào app** | đường ống chạy trọn vẹn ngay; khi log thật về chỉ cần đổi đầu vào | số đo trên tập này không nói gì về hiện trường |
| 2 | Chỉ viết mã, chờ log thật mới chạy | không có số đo dễ gây hiểu nhầm | lỗi đường ống chỉ lộ khi dữ liệu về |

**Chọn: 1.** Cổng từ chối mọi báo cáo có `source` khác `real` (sửa lại Q17: model tổng hợp chỉ sống
trong `tools/nn/`, app vẫn chạy `untrained-fixture-v1`).

## Sau Fable review phase AI (L2a, L2b, C0) — Q26–Q28 (điều phối chốt tạm 09/11/2026, owner chưa duyệt)

Luật owner 09/11/2026: Fable review toàn bộ diff trước khi đóng phase lớn. Review `b6321e6..5d47b9e` (đã phát
hành trong 1.5.0) ra 2 mục High, 3 mục Medium ở L2b; C0 và phần tiến trình của L2a không có lỗi. Ghi chú
tester trong thư mục phát hành đã được đính chính ngay trong đêm.

### Q26 — Bộ lọc câu trả lời bỏ lọt 27/32 câu nguy hiểm viết tay: sửa theo hướng nào?

| # | Phương án | Được | Mất |
|---|---|---|---|
| 1 | **Mở rộng bộ lọc (tắt/gỡ bảo vệ, so sánh "to hơn", đặt giá trị, phủ định tu từ, ngôn ngữ khác, chữ giãn/đồng dạng) + một dòng cảnh báo cố định dưới MỌI câu trả lời AI + sửa lời trong tài liệu** | chặn được các lớp đã biết; người dùng luôn thấy đây là lời khuyên chưa kiểm chứng | vẫn là bộ lọc cụm từ, không phải bảo đảm; chặn nhầm nhiều hơn |
| 2 | Chỉ thêm dòng cảnh báo, giữ bộ lọc | ít mã | lời khuyên nguy hiểm vẫn hiện nguyên văn |
| 3 | Dùng một lượt LLM thứ hai để chấm câu trả lời | bắt được cách nói lạ | tốn thêm lượt gọi, chậm gấp đôi, và chính nó cũng không phải bảo đảm |
| 4 | Tắt hẳn ô hỏi AI tới khi có cách chặn chắc chắn | không rủi ro | mất tính năng owner đã chọn ở Q2 |

**Chọn: 1.** Giữ nguyên tắc Q13 (không chắc thì chặn). Tài liệu không được viết "bị thay bằng thông báo cố định"
như một cam kết nữa.

### Q27 — Khoá nào được gửi đi, và gửi tới đâu?

Review: khoá trong biến môi trường `OPENAI_API_KEY` được gửi dù giao diện báo "Chưa có khoá"; khoá đã lưu đi
theo bất kỳ địa chỉ nào người dùng đổi sang; proxy HTTP nhận được khoá khi địa chỉ là `http://localhost`.

| # | Phương án | Được | Mất |
|---|---|---|---|
| 1 | **App chỉ dùng khoá đã lưu (bỏ biến môi trường khi chạy từ app); khoá đã lưu gắn với máy chủ của nó, đổi địa chỉ phải nhập lại khoá; địa chỉ nội bộ không đi qua proxy; địa chỉ có `user:pass@` bị từ chối** | thứ giao diện hiển thị khớp với thứ được gửi; khoá không tới nhầm nhà cung cấp | người đổi địa chỉ phải nhập lại khoá; file khoá của 1.5.0 cho "Tương thích OpenAI" phải lưu lại một lần |
| 2 | Giữ biến môi trường nhưng hiện nó trong trạng thái khoá | tiện cho người đã có biến sẵn | khoá vẫn có thể tới nhầm nhà cung cấp |

**Chọn: 1.** Dòng lệnh dành cho nhà phát triển (chạy `copilot.py` tay) vẫn đọc biến môi trường.

### Q28 — Gỡ cài đặt và file khoá

| # | Phương án | Được | Mất |
|---|---|---|---|
| 1 | **Lời nhắc gỡ cài nêu rõ file khoá API và hỏi có xoá không (mặc định giữ, như preset)** | người dùng biết còn một file khoá đã mã hoá | trình gỡ chạy quyền cao có thể trỏ nhầm `%APPDATA%` của người khác: lời nhắc phải nói rõ đường dẫn |
| 2 | Luôn xoá file khoá khi gỡ | sạch | gỡ để cài lại thì mất khoá; cùng rủi ro trỏ nhầm hồ sơ |

**Chọn: 1.**

### Q29 — Đóng gói 1.5.1 gồm các bản sửa và file ghi công bên thứ ba khi CI xanh? (và Q22)

Hỏi owner ở ô bước kế tiếp sau khi báo H1/H2/M1–M3. Điều phối khuyên: có, và Q22 phương án 1.

| # | Phương án | Được | Mất |
|---|---|---|---|
| 1 | **Đóng gói 1.5.1 ngay khi PR sửa xanh và đã merge, kèm `THIRD-PARTY-NOTICES.txt`** | tester thôi dùng bản có bộ lọc yếu và đường khoá sai; đúng điều khoản giấy phép | thêm một bản trong thư mục phát hành; bộ lọc mới chặn nhầm nhiều hơn |
| 2 | Chờ gom thêm thay đổi | ít bản hơn | 1.5.0 với các lỗi đã biết tiếp tục là bản mới nhất |

**Owner 09/11/2026 (nguyên văn):** "Cứ làm liên tục cho tới khi xong, còn hiện tại các câu hỏi kiểu này bạn tự
chọn, tôi theo đề xuất của bạn." → **Chọn 1; Q22 chọn 1** (theo khuyến nghị đã ghi). Cách hiểu của điều phối:
các câu hỏi cùng loại (đóng gói bản sửa, lựa chọn đã có khuyến nghị ghi sẵn) tự chốt theo khuyến nghị, ghi vào
sổ này; thao tác phá huỷ và việc khoá API vẫn hỏi riêng.

### Q30 — App dùng JUCE và ASIO SDK theo điều khoản nào? **CHỜ OWNER**

Nảy ra khi soạn `installer/THIRD-PARTY-NOTICES.txt` (Q22): repo không ghi ở đâu app dùng JUCE 9 theo AGPLv3
hay giấy phép thương mại của JUCE, và ASIO SDK theo GPLv3 hay thoả thuận riêng của Steinberg. Tình trạng này
có từ bản đầu, không do phase AI sinh ra. File ghi công chép nguyên `LICENSE.md` hai lựa chọn của JUCE và dòng
nhãn hiệu ASIO, không khẳng định lựa chọn nào.

| # | Phương án | Được | Mất / hệ quả |
|---|---|---|---|
| 1 | **AGPLv3 cho JUCE + GPLv3 cho ASIO SDK; ghi giấy phép của chính app là AGPLv3 trong repo và bộ cài** | không tốn phí; repo đã công khai nên nghĩa vụ đưa mã nguồn đã đáp ứng phần lớn | toàn bộ app là AGPLv3: ai nhận bản cài có quyền lấy mã nguồn và phát hành lại; khó đảo ngược với các bản đã phát hành; phần mã license (kích hoạt trả phí) về sau phải tính lại |
| 2 | Giấy phép thương mại JUCE (gói miễn phí nếu doanh thu dưới ngưỡng của JUCE, hoặc trả phí) + thoả thuận ASIO ký với Steinberg | giữ được quyền đóng mã, bán bản quyền sau này | phải đăng ký/ký với hai bên; có thể tốn phí; cần owner tự làm |
| 3 | Để nguyên, chưa ghi gì | không việc | bản phát hành tiếp tục không nói rõ điều khoản — rủi ro pháp lý nằm ở owner |

**Chọn:** chờ owner (quyết định pháp lý/kinh doanh, không thuộc loại điều phối tự chốt). Không chặn 1.5.1: file
ghi công không làm tình trạng xấu hơn. Điều phối nghiêng về 1 nếu sản phẩm ở lại freeware, 2 nếu định bán.

### Q26 — bổ sung 10/11/2026: ba lượt đo độc lập

"Đo độc lập" = một agent chỉ-đọc viết câu **trước khi đọc mã bộ lọc**, câu không nằm trong fixture nào.

| Lượt | Bộ lọc | Lọt (câu nguy hiểm) | Chặn nhầm (câu an toàn) |
|---|---|---|---|
| review | 1.5.0 | 27/32 (84%) | 7/18 (39%) |
| 1 | vòng 1 (động từ + đối tượng theo mệnh đề) | 36/153 (23,5%) | 2/53 (3,8%) |
| 2 | vòng 2 (default-deny theo danh từ, mang đối tượng, cổng ngôn ngữ) | 38/183 (20,8%) | 14/78 (17,9%) |
| 3 | vòng 3 (từ vựng của app, "lên/to hơn" trần, nông = nguy hiểm, sửa phủ định) | 30/175 (17,1%); tự nhiên 29/145 (20,0%) | 7/74 (9,5%) |

Corpus của chính người viết bộ lọc báo 0 lọt ở cả ba vòng — không phải số đo.

### Q31 — Bộ lọc không qua ngưỡng đặt trước: làm gì?

Ngưỡng đặt trước lượt 3 (điều phối, theo tư vấn của advisor): lọt tự nhiên ≤ 10%, chặn nhầm ≤ 12%; trượt
cũng phát hành và không có vòng 4. Kết quả: lọt 20,0% (trượt), chặn nhầm 9,5% (đạt).

| # | Phương án | Được | Mất |
|---|---|---|---|
| 1 | **Phát hành 1.5.1 với bộ lọc vòng 3, tài liệu dẫn bằng số đo độc lập; dòng cảnh báo cố định là hàng rào chính; dừng vá danh sách từ** | tester thôi dùng bản lọt 84% và đường khoá sai; không hứa điều không đo được | 1/5 lời khuyên nguy hiểm vẫn hiện nguyên văn (kèm dòng cảnh báo) |
| 2 | Vòng 4, 5… tới khi qua ngưỡng | có thể giảm thêm | ba vòng cho 23,5 → 20,8 → 20,0%: không hội tụ; 1.5.0 tiếp tục là bản mới nhất |
| 3 | Tắt ô hỏi AI tới khi có cách chặn chắc | không rủi ro từ lời văn | mất tính năng owner chọn ở Q2 |
| 4 | Đổi kiến trúc: model tự gắn cờ "có khuyên tăng mức / gỡ bảo vệ" trong đầu ra có cấu trúc, chặn theo cờ cộng bộ lọc | bắt được ý chứ không phải chữ | không đo được khi chưa có lượt gọi nhà cung cấp thật (chưa có khoá trên máy build) |

**Chọn: 1** (theo uỷ quyền owner 09/11: tự chốt theo khuyến nghị). **4 là việc kế tiếp** cho bộ lọc, làm khi
owner đã nhập khoá và có lượt gọi thật để đo. Lane tự lái (L3) không dựa vào bộ lọc này: lệnh của LLM đi bằng
tool call có kiểu, C++ kiểm. Bài học: `memory/answer-filter-plateau-2026-11-10.md`.
