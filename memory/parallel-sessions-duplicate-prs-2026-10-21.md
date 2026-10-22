# Hai phiên làm trùng việc (2026-10-20/21)

**Chuyện gì:** owner bật "God mode". Phiên dự án và DEV MASTER mỗi bên tự chạy một
audit trên cùng `main` rồi tự sửa. Kết quả là 6 cặp PR trùng nhau (`fix/*` và
`claude/*`): Bypass cộng, NaN-heal, ngưỡng scorer, docs, restart thiết bị.

**Mất gì:** khoảng 6 lần chạy CI thừa (mỗi lần ~15 phút Windows), một vòng đóng,
rút gọn và gộp PR, release note bị năm PR cùng sửa một chỗ, và owner phải tự chọn
giữa hai bản.

## Bài học

1. **"Tự quyết, đừng hỏi" là quyền với nội dung việc, không phải quyền tự sinh
   việc.** Trước khi bắt đầu một audit hay một lane, phải báo DEV MASTER.
2. **Thiếu một chỗ ghi "đã nhận việc".** Luật mới ở `docs/GIT-WORKFLOW.md` §5b:
   GitHub Issue cho mỗi việc, label `claimed`, mã finding nằm trong tiêu đề PR, và
   kiểm danh sách PR đang mở trước khi tạo nhánh.
3. **Release note phải là các mảnh rời** (`docs/release-notes/unreleased/`), không
   sửa thẳng file của phiên bản.
4. **Merge một chuỗi PR thì dựng nhánh tích hợp local trước.** Lần này các PR được
   CI test riêng; bản tích hợp đủ 10 PR (813/813) mới chứng minh được tổ hợp, và
   nó lộ ra conflict ở #24 trước khi merge thật.
