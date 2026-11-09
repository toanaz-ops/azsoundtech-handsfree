# Lane L2b — "hỏi AI" đa model, kho khoá DPAPI (2026-11-09)

**Bối cảnh:** owner chốt 09/11/2026 cho người dùng tự nhập khoá và cắm nhiều model
(Q2). L2b = sidecar biết `--ask` qua hai bộ nối (`anthropic` SDK, `openai_compatible`
qua `urllib`), khoá cất bằng DPAPI, bộ lọc câu trả lời bằng mã, ô hỏi + lớp phủ
`CÀI AI` trong bảng COPILOT. Spec: `docs/superpowers/specs/2026-11-09-copilot-panel-design.md`
§7 và mục "L2b sidecar contract". Quyết định: Q7, Q8, Q12–Q14 trong
`docs/superpowers/decisions/2026-11-09-ai-integration.md`.

## Bài học

1. **Traceback của Python thoát mã 1, và mã 1 đã có nghĩa.** `--has-key` dùng 1 cho
   "chưa có khoá"; một log sai UTF-8 văng traceback cũng ra 1. Mọi sidecar cần một
   lớp bọc ngoài cùng ở `main()` đổi ngoại lệ lạ thành một mã riêng, chỉ in TÊN kiểu
   ngoại lệ (không `str(e)`, vì thông báo có thể mang khoá).
2. **argparse cũng dùng mã 2.** Ghi đè `error()` để lỗi tham số ra mã 5, kẻo trùng
   "log rỗng".
3. **Với `--json`, stdout phải luôn là JSON ở mọi mã thoát mà app đọc.** Bản đầu in
   một câu chữ thường khi bị chặn (mã 4); phía C++ sẽ phải thêm nhánh đặc biệt. Chốt
   hợp đồng một dạng trước khi giao việc phía bên kia.
4. **Bộ lọc cụm từ cố định lọt gần hết.** "tăng âm lượng lên", "đẩy fader lên",
   "turn it up", "+3 dB", tiếng Việt không dấu, chữ chèn ký tự zero-width đều qua.
   Chuẩn hoá trước (NFKC, bỏ ký tự `Cf`, casefold, bỏ dấu, `đ→d`) rồi bắt "động từ
   tăng + đối tượng mức" trong cửa sổ vài từ. `chớ` bỏ dấu thành `cho` nên phải đổi
   trước khi bỏ dấu. Chặn oan được chấp nhận ("gain before feedback tăng 6 dB").
5. **Model gửi `Infinity`, `1e999`, chuỗi thay cho số.** Ép kiểu đối số tool ném
   `OverflowError`, không chỉ `TypeError`/`ValueError`. Bọc mọi ngoại lệ của tool
   thành JSON lỗi trả về cho model, vòng lặp chạy tiếp.
6. **Bản đóng băng phải tự chạy thử đường mới.** Cổng khói cũ chỉ chạy chế độ
   findings; `import anthropic` trong bundle chưa từng chạy. Thêm cổng: hỏi trọn
   vòng với máy chủ giả, và một lượt Claude với địa chỉ chết phải ra "lỗi kết nối"
   chứ không phải "thiếu SDK". PyInstaller tự nhặt `anthropic`, không cần hidden import.
7. **Import SDK trong hàm, không ở đầu file.** Bundle nặng lên 19,4 → 32,1 MB nhưng
   chế độ findings vẫn ~235 ms (đo ấm) vì không nạp pydantic.
8. **Base64 của JUCE không phải RFC 4648.** `MemoryBlock::toBase64Encoding` là biến
   thể riêng; Python `b64decode(validate=True)` không đọc được. Viết bộ mã hoá chuẩn
   và chứng minh bằng test chéo hai chiều (C++ cất → Python đọc, và ngược lại).
9. **Đuôi `\` và dấu `"` trong đối số dòng lệnh Windows.** `juce::ChildProcess` bọc
   đối số có khoảng trắng trong ngoặc kép mà không escape; `\` cuối sẽ nuốt ngoặc
   đóng. Làm sạch câu hỏi trước khi đưa vào argv.
10. **`ComboBox::setSelectedId(.., sendNotification)` là bất đồng bộ.** Test headless
    không bơm message loop nên callback không tới; dùng `sendNotificationSync`.
11. **ToggleButton của theme vẽ một dòng và cắt chữ.** Câu đồng ý dài cần một lớp con
    tự xuống dòng.
12. **CI chỉ chạy cho PR nhắm vào `main`/`develop`.** PR xếp chồng nhắm vào nhánh lane
    khác sẽ không có check nào; nhắm `main` và ghi rõ "xếp chồng trên #N".

## Bẫy quy trình

- Hook chặn `Edit`/`Write` sang worktree khác, kể cả worktree của chính subagent
  mình. Việc sửa nhỏ sau khi agent xong phải giao lại cho agent, hoặc mang nhánh về
  worktree của phiên.
- Test chéo với Python cần Python ≥ 3.10 (`str | None`); thấp hơn thì tự bỏ qua.
- Chưa một lượt gọi nhà cung cấp thật nào tính tới 09/11/2026: máy không có khoá.
  Mọi con số về chi phí và độ trễ vẫn là ước lượng.
