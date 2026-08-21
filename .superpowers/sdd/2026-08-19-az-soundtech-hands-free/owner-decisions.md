# Nhật ký quyết định của chủ dự án (Owner Decision Log)

**Mục đích.** Ghi lại *nguyên văn* mọi câu hỏi đã đặt ra cho chủ dự án, **đầy đủ
các phương án đã đưa ra** (kể cả phương án bị loại), và câu trả lời đã chọn.

Lý do tồn tại của file này: khi cần **đổi ý**, chủ dự án phải thấy lại được
những gì mình đã *không* chọn và lý do đánh đổi lúc đó — chứ không phải chỉ thấy
kết quả cuối cùng. Một quyết định chỉ ghi kết quả là một quyết định không thể
xét lại một cách có cơ sở.

**Quy tắc bảo trì:**

- Không bao giờ **sửa** hay **xoá** một mục đã có. Muốn đổi ý → thêm một mục
  **SUPERSEDED BY** ở dưới, và đánh dấu mục cũ là `⟲ ĐÃ THAY ĐỔI`.
- Mỗi mục phải trả lời được: *đổi quyết định này thì phải sửa lại những gì?*
  Đó là cột **Ràng buộc kéo theo**.
- File này nằm trong `.superpowers/`, đã được track từ 2026-08-22 (xem D-02).

---

## Bảng tra nhanh

| # | Ngày | Chủ đề | Quyết định | Trạng thái |
|---|---|---|---|---|
| D-00 | 2026-08-21 | Đổi sample rate thì notch ra sao | Giữ notch, chặn Nyquist | ✅ Đã thi hành |
| D-01 | 2026-08-22 | Push `main` lên remote | Có, push | ✅ Đã thi hành |
| D-02 | 2026-08-22 | Track `.superpowers/` | Có, track | ✅ Đã thi hành |
| D-03 | 2026-08-22 | Xoá 4 thư mục build cũ | Xoá cả bốn | ✅ Đã thi hành |
| D-04 | 2026-08-22 | `shared/` + file diagram | Commit cả hai | ✅ Đã thi hành |
| D-05 | 2026-08-22 | Notch từ preset khi đang chạy Auto | Detector nhận nuôi (adopt) | 🔵 Đã chốt, chưa code |
| D-06 | 2026-08-22 | Timer khi mất tín hiệu input | **CHƯA TRẢ LỜI** | ⏳ Đang mở |

---

## D-00 — Đổi sample rate thì notch đang có ra sao?

**Ngày:** 2026-08-21 · **Trạng thái:** ✅ Đã thi hành

**Bối cảnh.** Spec §8 dòng 182 viết: *"Sample rate thay đổi: clear notch,
re-init."* Nhưng Task 9 lại code **ngược lại**, và đã có một test xanh khoá hành
vi ngược đó lại. Hai reviewer độc lập đều báo đây là xung đột spec-vs-code, phải
để chủ dự án phân xử.

**Quyết định:** **Giữ notch, chặn Nyquist.**

Notch sống sót qua một lần đổi sample rate. Notch nào có tần số không còn nhỏ
hơn Nyquist mới thì chuyển sang `Idle` **nhưng vẫn giữ nguyên tham số đã lưu**
(để nếu rate tăng lại thì phục hồi được).

**Vì sao quan trọng:** nếu retarget mù một notch vượt Nyquist, pole của biquad
văng ra ngoài đường tròn đơn vị và mạch **phân kỳ**. Đo thực tế: notch 30 kHz đặt
hợp lệ ở 96 kHz, rồi đổi về 44.1 kHz → đỉnh output **4.11e18**. Sau khi chặn: 1.0.

**Ràng buộc kéo theo:**

- Spec §8 dòng 182 **đã được sửa tại nguồn** cho khớp với quyết định này.
- `NotchChain::setSampleRate()` và `Biquad::setNotchFilter()` thi hành nó.
- Đổi ý → phải sửa spec §8, `NotchChain.cpp`, và các test trong
  `tests/test_notchchain.cpp` khoá hành vi này.

> **Ghi chú.** Đây là quyết định của phiên trước, được tái dựng từ
> `progress.md:177-179` và `fix-pass-1-brief.md:136`. Các phương án đưa ra lúc
> đó **không được ghi lại nguyên văn** — đây chính là lý do file này ra đời.

---

## D-01 — Push `main` lên remote?

**Ngày:** 2026-08-22 · **Trạng thái:** ✅ Đã thi hành

**Câu hỏi nguyên văn:**

> `origin/main` is at `cabe715` — 22 commits behind local. The CI in
> `.github/workflows/build.yml` exists and has never run once. Push?

**Các phương án:**

| | Phương án | Mô tả đưa ra |
|---|---|---|
| ✅ | **Yes, push main** *(khuyến nghị)* | Fast-forward 22 commit. CI chạy lần đầu tiên — nó đã có thể bắt được lỗi thiếu `juce_generate_juce_header` ngay từ Task 3. Đây là hành động hướng ra ngoài nên cần chủ dự án đồng ý rõ ràng. |
| | Not yet | Giữ `main` ở máy local. CI vẫn chưa từng chạy; một lần hỏng ổ cứng là mất 22 commit. |

**Đã chọn:** ✅ **Yes, push main**

**Kết quả thực tế:** đẩy `cabe715..9eabb52`, **24 commit** (không phải 22 — đã
có thêm 2 commit trong phiên). CI run `32510208372` được kích hoạt — lần chạy CI
**đầu tiên trong lịch sử dự án**.

**Ràng buộc kéo theo:**

- Từ giờ `main` là công khai với bất kỳ ai có quyền truy cập repo (repo đang ở
  chế độ **private**).
- Không thể "hoàn tác" một lần push một cách sạch sẽ. Muốn gỡ phải force-push,
  và điều đó viết lại lịch sử của người khác nếu có ai đã fetch.
- Mọi commit sau này nên push đều đặn để CI còn có ý nghĩa.

---

## D-02 — Có track `.superpowers/` không?

**Ngày:** 2026-08-22 · **Trạng thái:** ✅ Đã thi hành

**Câu hỏi nguyên văn:**

> `.gitignore` line 59 excludes `.superpowers/`, so progress.md and every task
> report — the project's whole audit trail — is untracked and unpushed. Track it?

**Các phương án:**

| | Phương án | Mô tả đưa ra |
|---|---|---|
| ✅ | **Track it** *(khuyến nghị)* | Bỏ dòng ignore, commit ledger và các task report. Dấu vết kiểm toán sống sót qua hỏng ổ cứng và đi theo repo. |
| | Keep it ignored | Ledger chỉ nằm ở local. Chấp nhận việc lịch sử **và** bản ghi về lịch sử đó cùng nằm trên một ổ đĩa. |

**Đã chọn:** ✅ **Track it**

**Kết quả thực tế:** 28 file brief/report + `progress.md` đã vào repo. Dòng
`.superpowers/` trong `.gitignore` được thay bằng một comment giải thích **vì
sao** không được thêm lại — để người sau không "dọn dẹp" nó về như cũ.

Đã quét bảo mật trước khi push: không có credential nào trong các file này
(các kết quả khớp `secret`/`key` đều là nội dung `.gitignore` được trích trong
review package).

**Ràng buộc kéo theo:**

- Mọi task report **sau này** cũng sẽ được commit. Không viết credential, token,
  hay dữ liệu khách hàng vào brief/report.
- File này (`owner-decisions.md`) tồn tại được là nhờ D-02.

---

## D-03 — Xoá 4 thư mục build cũ?

**Ngày:** 2026-08-22 · **Trạng thái:** ✅ Đã thi hành

**Câu hỏi nguyên văn:**

> Four stale build directories (`build-review/`, `build-task8/`,
> `build-task8-msvc/`, `build-verify/`, ~1 GB) hold contradictory state —
> `build-task8-msvc/` still has the stale JuceHeader.h that let Task 8 claim a
> verified build over a broken HEAD. Delete them?

**Các phương án:**

| | Phương án | Mô tả đưa ra |
|---|---|---|
| ✅ | **Delete all four** *(khuyến nghị)* | Giữ `build/`. Giải phóng ~1 GB và xoá cái header cũ đã từng gây ra một lời khai "build xanh" sai sự thật. Không khôi phục được, nhưng CMake tạo lại được tất cả. |
| | Leave them | Đĩa vẫn bị chiếm; `JuceHeader.h` cũ vẫn nằm đó chờ đánh lừa một lời khai clean-build trong tương lai. |

**Đã chọn:** ✅ **Delete all four**

**Kết quả thực tế:** đã xoá. Dung lượng thật là **~553 MB** (không phải ~1 GB):
`build-review` 114M, `build-task8` 2.0M, `build-task8-msvc` 435M,
`build-verify` 2.0M. `build/` (466M) được giữ lại.

**Ràng buộc kéo theo:**

- Không có. Mọi thư mục build đều tái tạo được bằng `cmake -B <dir>`.
- `.gitignore` dòng 3 (`build-*/`) vẫn còn, nên thư mục build tạm sẽ không bao
  giờ lọt vào git.
- CLAUDE.md của repo đã ghi các thư mục này là rác, không được tạo lại.

---

## D-04 — `shared/` và `opencode-harness-diagram.html` xử lý sao?

**Ngày:** 2026-08-22 · **Trạng thái:** ✅ Đã thi hành

**Câu hỏi nguyên văn:**

> `shared/` and `opencode-harness-diagram.html` are untracked and not ignored,
> so they show up in every `git status`. What should happen to them?

**Các phương án:**

| | Phương án | Mô tả đưa ra |
|---|---|---|
| ✅ | **Commit both** *(khuyến nghị)* | Các file handoff trong `shared/` là bản ghi xuyên phiên làm việc; commit chúng biến chuỗi handoff thành một phần của repo. |
| | Add ignore rules | Coi như file nháp local. Không vào repo, không hiện trong `git status`. |
| | Commit `shared/`, ignore diagram | Handoff là hồ sơ dự án; file HTML diagram chỉ là sản phẩm nháp dùng một lần. |

**Đã chọn:** ✅ **Commit both**

**Kết quả thực tế:** `shared/handoff/` (3 file handoff: 20, 21, 22 tháng 8) và
`opencode-harness-diagram.html` đã vào repo trong commit `9eabb52`.

**Ràng buộc kéo theo:**

- File handoff của các phiên sau cũng nên đi vào `shared/handoff/`.
- Handoff cũ (20, 21) **chứa những khẳng định đã bị chứng minh là sai**. Chúng
  được giữ lại có chủ đích làm hồ sơ. Handoff 22 mở đầu bằng cảnh báo này.

---

## D-05 — Notch nạp từ preset khi đang chạy Auto thì sau đó ra sao?

**Ngày:** 2026-08-22 · **Trạng thái:** 🔵 Đã chốt, chưa code

**Bối cảnh.** Spec §7 định nghĩa *"Preset = danh sách notch đã lock"*, và GUI
(§6.1) có nút **[Clear All]** cùng **[Save Preset ▼]**. Nghĩa là **GUI cũng đặt
và gỡ notch**, không chỉ có detector. Điều này đụng vào một ràng buộc kỹ thuật:
`LockFreeRingBuffer` là **SPSC** — chỉ chấp nhận **một** producer duy nhất.

**Câu hỏi nguyên văn:**

> Mid-show, a soundman loads the "Music" preset while Auto mode is running — the
> preset installs 6 notches the detector never placed. Thirty seconds later, what
> should happen to those 6 notches?

**Các phương án:**

| | Phương án | Mô tả đưa ra |
|---|---|---|
| ✅ | **Detector adopts them** *(khuyến nghị)* | Notch từ preset đi vào model của detector như thể chính detector đã đặt. Auto-release áp dụng bình thường: notch preset nào ngừng hú trong 30 s thì tự gỡ. Preset là điểm khởi đầu, sau đó detector duy trì. |
| | Detector leaves them alone | Notch preset là vĩnh viễn cho tới khi bấm Clear All. Detector chỉ thêm notch của nó và chỉ auto-release notch của nó. Preset giữ nguyên như lúc lưu, nhưng một notch preset lỗi thời sẽ không bao giờ biến mất. |
| | Preset replaces everything and resets Auto | Nạp preset = reset cứng: xoá hết, cài notch của preset, khởi động lại detection từ đầu với các notch đó. Dễ đoán nhất, nhưng mất mọi notch detector đã tìm được trong show. |

**Đã chọn:** ✅ **Detector adopts them**

**Hệ quả kiến trúc — đây là phần quan trọng nhất của quyết định này.**

Chọn "adopt" nghĩa là **detector sở hữu model notch và là producer lệnh duy
nhất**. Luồng lệnh trở thành:

```
GUI (message thread) ──► Detector (detector thread) ──► Audio thread
                    SPSC                          SPSC
```

chứ không phải GUI và Detector cùng bắn vào audio thread. Điều này:

- **Giữ được bất biến SPSC** mà không cần hàng đợi MPSC (khó viết lock-free đúng).
- **Gỡ bỏ điểm chặn cứng của Task 12.** Handoff ghi Task 12 bị chặn vì
  "không có đường đọc ngược thread-safe từ notch state về detector thread".
  Nhưng nếu detector là nơi duy nhất phát lệnh, nó **không cần đọc ngược** —
  nó chỉ cần **nhớ những gì nó đã ra lệnh**. Điểm chặn tan biến.
- **Là bắt buộc về mặt dữ liệu.** Auto-release (Task 14) cần `locked_at` và
  `last_detected_time` cho từng notch. `NotchChain::NotchInfo` **không có** hai
  trường đó và không thể có (nó là struct audio-thread). Nên detector buộc phải
  giữ model riêng phong phú hơn — dù có quyết định thế nào.

**Ràng buộc kéo theo:**

- GUI **không được** gọi thẳng `AudioEngine::setNotch()`. Mọi thao tác notch của
  GUI (Clear All, nạp preset, sửa tay) đi qua detector.
- Cần một hàng đợi thứ hai: GUI → Detector.
- Snapshot notch state cho GUI hiển thị (Task 22) vẫn cần, nhưng chỉ để **hiển
  thị**, không phải để detector đọc.
- Đổi ý sang "leaves them alone" → detector phải phân biệt notch "của tôi" và
  "của preset", và cần thêm cờ nguồn gốc trong model.

---

## D-06 — Timer auto-release khi mất tín hiệu input ⏳ ĐANG MỞ

**Ngày hỏi:** 2026-08-22 · **Trạng thái:** ⏳ **CHƯA TRẢ LỜI** (câu hỏi bị bỏ lỡ)

**Bối cảnh.** Auto-release là "30 giây không còn peakiness" (spec §5.2 bước 7).
Quy tắc carry-over bắt buộc dùng **thời gian thực (wall-clock)**, không dùng
`readCount` — vì `AudioEngine` chỉ ghi vào tap khi input channel 0 khác null,
nên khi mất input, `readCount` **đứng yên trong khi thời gian thật vẫn trôi**.

Nội dung câu hỏi và các phương án được ghi lại ở phần "Câu hỏi đang chờ" bên
dưới, để khi trả lời xong thì chuyển lên đây thành D-06 hoàn chỉnh.

---

## Câu hỏi đang chờ trả lời

### ⏳ Q-06 (sẽ thành D-06)

> Auto-release is "30 seconds without peakiness" (§5.2 step 7). The carry-over
> rule says it must use real wall-clock time, not `readCount`. But if the mic
> goes dead mid-show — XLR kicked, ASIO glitch, input muted — the tap stops
> advancing while the clock keeps running. After 30 s of dead input, what should
> the notches do?

| | Phương án | Mô tả |
|---|---|---|
| ? | **Freeze timers while input is dead** *(khuyến nghị)* | Auto-release đếm thời gian thực, **nhưng chỉ khi tap thực sự có audio**. Mic sống lại → notch vẫn còn nguyên, phòng vẫn được bảo vệ. Chi phí: một phép kiểm tra "tap có sống không"; 30 s là 30 giây audio thật. |
| ? | Keep counting — clear after 30 s regardless | Đọc spec theo nghĩa đen. Đơn giản nhất. Nhưng một sự cố cáp 30 giây sẽ xoá sạch mọi notch, và ngay khi audio trở lại thì phòng hú không có gì bảo vệ — đúng lúc soundman đang bận sửa cáp. |
| ? | Freeze, and also clear on device restart | Đóng băng khi mất tín hiệu, nhưng coi một lần stop/start thiết bị hoàn chỉnh là một phiên mới thật sự, xoá các notch do Auto đặt. Phân biệt "sự cố ngắn" với "setup mới". |

### Các câu hỏi thiết kế Lane B còn lại (chưa hỏi)

Ghi ra đây để không bị quên, sẽ hỏi lần lượt:

1. **Sức chứa hàng đợi lệnh và xử lý tràn.** Bao nhiêu lệnh notch được drain mỗi
   callback? `setNotch()` tính lại coefficient (vài phép lượng giác) — drain 64
   lệnh trong một callback là một đỉnh CPU. Tràn thì đếm như `tapDropCount_` hay
   xử lý khác?
2. **Nhịp chạy của detector thread.** Poll theo chu kỳ cố định, hay chờ tín hiệu
   từ audio thread? (Audio thread không được block, nên signal có rủi ro.)
3. **Công bố spectrum cho GUI 60 fps (Task 19).** `Spectrum::magnitudes` hiện là
   con trỏ mượn, hết hạn ngay khi `processLatestBlock()` chạy lần sau. Double
   buffer, triple buffer, hay seqlock?
4. **`depthDB` chưa được áp dụng ở đâu cả** — mọi notch hiện là null sâu tối đa.
   Spec §5.1 yêu cầu 6–24 dB. Sửa toán coefficient trước hay sau khi GUI hiển thị?

---

## Phụ lục — cách dùng file này khi muốn đổi ý

1. Tìm mục `D-xx` tương ứng.
2. Đọc cột **Các phương án** — phương án bạn muốn đổi sang có thể đã được cân
   nhắc và loại, kèm lý do loại.
3. Đọc **Ràng buộc kéo theo** để biết đổi ý thì kéo theo sửa những gì.
4. Thêm mục mới ở cuối dạng:

   ```
   ## D-xx-R1 — <chủ đề> (SUPERSEDES D-xx)
   **Ngày:** ... · **Lý do đổi:** ...
   ```

   rồi đánh dấu mục cũ là `⟲ ĐÃ THAY ĐỔI → xem D-xx-R1`.
5. **Không xoá mục cũ.** Lịch sử một quyết định sai còn hữu ích hơn một quyết
   định đúng không có lịch sử.
