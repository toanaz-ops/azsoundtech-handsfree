# Sổ quyết định — Lane L: LLM copilot (2026-10-20)

Mỗi mục là một ngả rẽ thiết kế. Ghi đủ: câu hỏi, các phương án đã đề xuất (kèm
phương án khuyên dùng), và lựa chọn.

**Trạng thái:** **L1 xong ở dạng công cụ offline** (PR lane L1). Q1–Q5 do điều phối
chốt tạm theo chỉ thị owner 21/10/2026, được chuyển qua phiên DEV MASTER: "God mode:
làm tất bớt hỏi bớt chờ human", "Làm tiếp những task khác cho tới khi xong". Owner
**CHƯA duyệt** từng mục; lật lại khi nghe tester dùng.

Spec đích: `docs/superpowers/specs/2026-10-20-llm-copilot-design.md`.
Người hỏi/chốt: Claude (điều phối). Người quyết cuối: owner (ToanAZ).

## Bối cảnh

- Roadmap: L = "LLM qua tool call đọc telemetry, gợi ý cho soundman", cần D. Lane D đã
  hạ cánh 05/09/2026: log session JSONL, nút oan/đúng, `tools/logstats.py`.
- Nghiên cứu 04/09/2026: LLM **không bao giờ** ở tầng realtime; copilot ở tầng giây–phút.
- App là insert âm thanh trực tiếp; owner cấm mọi đánh đổi làm xấu tiếng (lane A).
- Chưa có ghi âm hay log thật từ rig (p005-hw = later).

## Q1 — Copilot sống ở đâu?

| # | Phương án | |
|---|---|---|
| 1 | **Công cụ offline `tools/copilot/`** (Python, cạnh `logstats.py`) đọc file log sau buổi diễn hoặc trong giờ nghỉ; app không đổi một dòng nào | khuyên dùng |
| 2 | Bảng chat trong Geek mode, gọi API trong lúc show | |
| 3 | Dịch vụ chạy nền đọc log đang ghi | |

**Chọn: 1.** Lý do: phương án 2 đưa một lời gọi mạng vào tiến trình đang xử lý âm
thanh trực tiếp. Nó cũng cần lưu khoá API trên máy tester, và cần ảnh GUI cùng một vòng
phản biện riêng. Phương án 1 giữ app đúng như cũ mà vẫn thử được giá trị của copilot
trên log thật từ alpha.

## Q2 — Phần nào cần LLM?

| # | Phương án | |
|---|---|---|
| 1 | **Luật cố định tính phát hiện (lặp lại, chạm trần, chồng notch, dồn dập, tỉ lệ oan, thiếu nhãn, log mất sự kiện), chạy không cần mạng; LLM chỉ giải thích và trả lời câu hỏi qua tool đọc-chỉ** | khuyên dùng |
| 2 | Toàn bộ do LLM đọc log thô | |

**Chọn: 1.** Có thể test được, chạy không cần khoá API, và LLM không thể "bịa" một phát
hiện mà luật không thấy. Lý do là tool `get_findings` trả đúng danh sách luật đã tính.

## Q3 — LLM được làm gì?

| # | Phương án | |
|---|---|---|
| 1 | **Chỉ đọc**: 4 tool (`session_summary`, `get_findings`, `list_notches`, `notch_context`), không tool nào ghi | khuyên dùng |
| 2 | Cho LLM đề xuất preset để app nạp | |

**Chọn: 1.** Đề xuất preset là một đường ghi vào cấu hình âm thanh. Đó là lane riêng,
và chỉ làm khi đã có số liệu từ L1.

## Q4 — Ràng buộc nội dung lời khuyên?

| # | Phương án | |
|---|---|---|
| 1 | **Không bao giờ khuyên tăng gain, tăng trần, tắt chống hú hay bỏ bảo vệ**. Khi nghi ngờ thì khuyên hạ gain và thử ở âm lượng nhỏ. Ràng buộc này nằm trong system prompt và được ghim bằng test trên mọi luật | khuyên dùng |
| 2 | Không ràng buộc | |

**Chọn: 1.**

## Q5 — Mô hình và ngôn ngữ?

| # | Phương án | |
|---|---|---|
| 1 | **`claude-opus-5-5`**, adaptive thinking, effort `medium`, bật refusal fallback phía server (`fallbacks: "default"`); trả lời tiếng Việt | khuyên dùng |
| 2 | Mô hình rẻ hơn | |

**Chọn: 1.** Mỗi lần hỏi chỉ là một lượt nhỏ với log vài trăm dòng, nên chi phí không
đáng kể. Đổi mô hình là việc của owner.
