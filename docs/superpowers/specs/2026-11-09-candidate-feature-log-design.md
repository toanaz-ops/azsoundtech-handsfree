# Lane C0 — Ghi đặc trưng ứng viên ở chế độ bóng (chưa có mạng nơ-ron)

**Ngày:** 2026-11-09. **Roadmap:** lane C, bước đầu. Cần D (đã có), S (đã có).
**Sổ quyết định:** [`../decisions/2026-11-09-ai-integration.md`](../decisions/2026-11-09-ai-integration.md) (Q4 2, Q5 1, Q10).
**Nghiên cứu nền:** [`../../research/2026-11-09-ai-integration-llm-and-realtime.md`](../../research/2026-11-09-ai-integration-llm-and-realtime.md) §3.5.

**Đụng audio path:** **không.** Chạy trên detector thread (`NotchController`); audio
callback (`AudioEngine`) không đổi một dòng. **Mức ra dự kiến: 0 dB thay đổi.** Quyết
định đặt notch không đổi: không ngưỡng, hằng số DSP hay thứ tự xử lý nào bị sửa; lane
này chỉ **ghi thêm** vào log.

All identifiers, file/dir names and code-like string values in this lane are English.

## 1. Vì sao

Lane C cần dữ liệu để học phân biệt hú với nốt nhạc giữ. Log hôm nay có hai lỗ:

1. **Chỉ ứng viên được đặt notch mới có bản ghi.** Ứng viên bị scorer từ chối — chính là
   các mẫu "không phải hú" mà mạng cần — không để lại gì.
2. `NotchEvent` có `riseRatio` nhưng `notchEventToVar` không ghi nó ra JSON; tỉ lệ L/R
   chỉ tồn tại dưới dạng hệ số nhị phân `asymmetry`.

C0 vá hai lỗ đó. **Không có suy luận, không RTNeural, không phụ thuộc mới** (Q10): đó là
C1, sau khi C0 có dữ liệu.

## 2. Thay đổi

### 2.1 `notch_set` thêm hai trường

| Trường | Nghĩa | Khi nào có |
|---|---|---|
| `rise_ratio` | `mag_now / mag_ref` thô, không bão hoà (đã có trong `NotchEvent::riseRatio`) | khi `hasScore` |
| `lr_ratio` | biên độ bin ở làn này chia biên độ cùng bin ở làn kia, sàn mẫu số `1e-12` | khi làn kia tồn tại |

### 2.2 Sự kiện mới `candidate_features`

Ghi cho ứng viên **suýt được xác nhận nhưng không được đặt notch** trong block đó.

Điều kiện ghi, tất cả phải đúng:

- `score >= kCandidateLogScore` (= **0,35**; đơn vị: tích 0..1, cùng thang với
  `kConfirmScore` 0,7 — không phải peakiness);
- block này **không** gọi `placeConfirmed` cho bin đó;
- mọi đặc trưng đều `isfinite`;
- chưa ghi bin này (±1 bin, cùng slot và làn) trong `kCandidateLogIntervalMs` (= **2000 ms**);
- controller này chưa ghi quá `kMaxCandidateLogsPerSecond` (= **1**) trong giây hiện tại.

Trần thô: 8 slot × 1 dòng/giây ≈ 29.000 dòng/giờ, cỡ 9 MB/giờ ở trường hợp xấu nhất;
một hai slot là thường gặp.

Trường của sự kiện (vô hướng, không kèm phổ 1025 bin):

`slot, lane, bin, hz, peakiness, score, p_norm, rise, rise_ratio, novelty, penalty,
asymmetry, lr_ratio` (nếu có làn kia), `persist` (bộ đếm hiện tại), `persist_needed`,
`thr`, và `near`: 11 biên độ quanh đỉnh (bin −5…+5; ra ngoài dải thì 0).

### 2.3 Hàng đợi riêng

`candidate_features` đi qua **`candidateOutbox_` riêng** (sức chứa 16, cấp phát trước
trong constructor), không dùng chung `eventOutbox_` 64 phần tử. Lý do: nhạc sinh ứng viên
liên tục; nếu dùng chung, một đợt ứng viên có thể đẩy rớt `notch_set` / `notch_clear` —
thứ mà copilot và `logstats` dựa vào. Rớt ở hàng đợi này đếm riêng
(`droppedCandidateEvents()`), ghi vào `session_end` dưới tên `dropped_candidates`.

Bản ghi trong hàng đợi là struct POD; `juce::var` và chuỗi JSON dựng lúc xả hàng đợi,
đúng như đường `NotchEvent` hôm nay.

### 2.4 Phiên bản định dạng log

`session_start` thêm `log_schema: 2`. Log không có trường này là bản 1.

### 2.5 `tools/logstats.py`

Đếm `candidate_features` (tổng, và theo slot) trong `summarise()`; in một dòng. Tên sự
kiện lạ vốn đã bị bỏ qua nên log mới đọc được bằng `logstats` cũ.

## 3. Thứ KHÔNG đổi (và cách chứng minh)

- So sánh `score > kConfirmScore`, bộ đếm `persistence`, `placeConfirmed`: không sửa.
  Chứng minh: toàn bộ test `test_notchcontroller.cpp` hiện có xanh **không sửa một
  assert nào**.
- Thêm một test ghim: cùng một chuỗi block, dãy `NotchCommand` phát ra giống hệt nhau từng
  phần tử dù hàng đợi ứng viên đầy hay rỗng.
- Không file nào trong `src/dsp/` đổi hằng số. Nếu cần đọc biên độ làn kia hay lịch sử
  của scorer thì chỉ thêm accessor `const`.

## 4. Test

| Test | Khẳng định |
|---|---|
| near-miss ghi một sự kiện | tone có `score` trong [0,35; 0,7) sinh đúng 1 `candidate_features` với đủ trường |
| dưới ngưỡng không ghi | `score < 0,35` không sinh sự kiện |
| được đặt notch thì không ghi trùng | block gọi `placeConfirmed` không sinh `candidate_features` cho bin đó |
| giới hạn theo bin | cùng bin trong 2000 ms chỉ một sự kiện (dùng `FakeClock`) |
| giới hạn theo giây | nhiều bin cùng lúc: tối đa 1 sự kiện mỗi giây mỗi controller |
| hàng đợi tách biệt | làm đầy hàng đợi ứng viên, `notch_set` vẫn tới sink; `droppedCandidateEvents()` tăng |
| không hữu hạn thì bỏ | đặc trưng NaN/inf không sinh sự kiện |
| `notch_set` có `rise_ratio`, `lr_ratio` | mono không có `lr_ratio`; stereo có |
| `log_schema` | header có `log_schema: 2` |
| `logstats` | fixture mới có `candidate_features` được đếm; fixture cũ kết quả không đổi |

Đơn vị ghi ở cả hai vế tại chỗ so sánh (bài học lane R và lane M).

## 5. Tài liệu đi cùng

- `docs/release-notes/unreleased/candidate-feature-log.md` với dòng *expected level
  change: 0 dB*.
- Bảng sự kiện log ở spec vòng dữ liệu (`2026-09-05-data-loop-design.md`) thêm
  `candidate_features` và hai trường mới.
- `docs/GIOI-THIEU.md` và `docs/KY-THUAT-CHONG-HU.md` **không cần sửa**: không hằng số DSP,
  topology hay hành vi nhìn thấy được nào đổi.

## 6. Bước sau (không thuộc C0)

- **C1:** kiểm mã nguồn RTNeural về cấp phát lúc chạy (điều kiện của Q5); huấn luyện
  offline trên log C0 + dữ liệu tổng hợp để khởi động; lớp nạp model là thư viện riêng.
- **C2:** mạng chạy bóng, ghi `shadow_score` cạnh `score`; nghiệm thu < 1 ms bằng p99 trong
  app chạy đủ 8 slot.
- **C3:** mạng được quyền quyết định — cần buổi nghe riêng.
