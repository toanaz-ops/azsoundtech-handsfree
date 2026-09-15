<!--
Quy ước đầy đủ: docs/GIT-WORKFLOW.md
Điền hết các mục. "Chưa chạy" là câu trả lời hợp lệ; bỏ trống thì không.
-->

## Lane / phạm vi

<!-- Lane nào (S / D / G / M / R / ...), hoặc việc gì nếu không thuộc lane. Đụng file nào. -->

## Đụng audio path?

- [ ] **Không** — không chạm DSP, gain staging, buffer, hay ASIO callback.
- [ ] **Có** — mức level dự kiến thay đổi: <!-- ví dụ: "notch sâu thêm tối đa 6 dB tại tần số đang hú; không đổi gain tổng" -->

> Đây là live-sound DSP, một build merge vào sẽ tới PA thật. Không đoán được mức
> thay đổi thì nói thẳng là không đoán được, để người nghe thử ở âm lượng thấp.

## Build + `ctest`

<!-- Dán output thật, không mô tả. Chưa chạy thì ghi "not verified". -->

```
cmake --build build --config Release
ctest -C Release
```

```
<dán ở đây>
```

## CI

<!-- gh pr checks <n> — dán output xanh. "no checks reported" nghĩa là PR đang
     xung đột, xem docs/GIT-WORKFLOW.md §6, chưa phải là đã qua. -->

```
<dán ở đây>
```

## Ảnh render (chỉ khi đổi GUI)

<!-- build/tools/Release/HandsFreeSnapshot.exe shots 1440 920 --fast
     Đính ảnh, và xác nhận đã ĐỌC LẠI ảnh chứ không chỉ tạo ra nó. -->

- [ ] Không đổi GUI.
- [ ] Có đổi GUI — ảnh đính kèm, đã đọc lại ảnh.

## Docs đã cập nhật

- [ ] `docs/GIOI-THIEU.md` (hành vi user-visible đổi)
- [ ] `docs/KY-THUAT-CHONG-HU.md` (hằng số DSP / topology đổi)
- [ ] `memory/<note>.md` + một dòng index trong `memory/MEMORY.md`
- [ ] Không cần — vì: <!-- lý do -->

## Verifier độc lập

- [ ] Đã chạy. Verifier **đọc file thật**, không đọc báo cáo của agent đã làm.
      Kết quả / finding còn mở: <!-- ... -->
- [ ] Chưa chạy — vì: <!-- lý do -->

## Release

- [ ] Không cần release.
- [ ] Cần release: `-Part` = `patch` / `minor` / `major` — vì: <!-- lý do -->

> Release chạy **trên `main`** sau khi PR này đã merge và đã
> `git pull --ff-only`, không chạy trên nhánh này. Commit bump `CMakeLists.txt`
> đi qua một PR riêng `chore(release): 1.x.y`.
