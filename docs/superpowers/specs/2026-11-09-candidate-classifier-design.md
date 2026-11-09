# Lane C1 — bộ phân loại ứng viên bằng mạng neuron, chạy bóng

Ngày 09/11/2026. Owner: "riêng vụ mạng neuron được tôi muốn làm luôn" (Q16 của
[`../decisions/2026-11-09-ai-integration.md`](../decisions/2026-11-09-ai-integration.md)).
Các ngả rẽ của lane này: Q17–Q22 cùng file. Nền: soát mã nguồn RTNeural
[`../../research/2026-11-09-rtneural-realtime-audit.md`](../../research/2026-11-09-rtneural-realtime-audit.md),
spec C0 [`2026-11-09-candidate-feature-log-design.md`](2026-11-09-candidate-feature-log-design.md).

> **Mức ra thay đổi dự kiến: 0 dB ở mọi ngõ ra.** Lane này không chạm đường audio, không đổi ngưỡng,
> hằng số hay thứ tự quyết định của detector. Mạng chỉ tính một con số và ghi vào log.

## 1. Phạm vi và tên bước

Spec C0 §6 gọi "C1 = soát + huấn luyện + lớp nạp" và "C2 = chạy bóng". Lane này gộp và chia lại:

| Bước | Nội dung | Thay đổi nhìn thấy |
|---|---|---|
| **C1a** | RTNeural vào repo, lớp `CandidateClassifier`, cổng Q5 (test đếm cấp phát, so khớp tham chiếu, đầu vào hỏng). **App không tạo bộ phân loại.** | không; log không đổi |
| **C1b** (= "C2" của spec C0) | Nối vào luồng detector ở chế độ bóng, model `synthetic-v0`, ghi `nn_score` vào log | log có thêm trường; không đổi âm thanh, không đổi quyết định notch |
| để sau | huấn luyện trên dữ liệu thật từ tester; đổi model giữa buổi diễn; cho mạng quyền quyết định (cần buổi nghe riêng) | |

Model huấn luyện thật **không** nằm trong lane này: nó cần log `candidate_features` kèm verdict từ
tester, thứ 1.5.0 mới bắt đầu sinh ra.

## 2. Đưa RTNeural vào

- Submodule `external/RTNeural`, ghim commit `95c3c0f987a6fe903e7eec71e797405dbed7caf7` (repo không có
  tag). Không `add_subdirectory`: target CMake của RTNeural là thư viện STATIC với backend mặc định
  Eigen (soát mã, bẫy 2 và 4).
- Dùng dạng header với **backend STL**: target INTERFACE `az_rtneural_headers`, include `SYSTEM`,
  nối vào cả ba nơi dùng `HANDSFREE_CORE_SOURCES` (`HandsFree`, `HandsFreeTests`, `HandsFreeSnapshot`).
- Thiếu `external/RTNeural/RTNeural/RTNeural.h` thì configure **dừng** bằng `FATAL_ERROR` kèm lệnh sửa
  (`git submodule update --init external/RTNeural`). Không cảnh báo rồi chạy tiếp: xem
  `memory/asio-sdk-silent-disable-2026-08-24.md`.
- `#include <RTNeural/RTNeural.h>` nằm trong đúng một file `.cpp`, sau một pimpl; file đó `#error`
  nếu `RTNEURAL_USE_EIGEN` hoặc `RTNEURAL_USE_XSIMD` được định nghĩa.
- Không junction thư mục này giữa các worktree; mỗi worktree tự clone.
- **Việc đầu tiên của C1a:** đọc lại ở đúng SHA các điểm bản thiết kế mới chỉ nhớ chứ chưa kiểm: tên
  define backend, chữ ký `ModelT`, cách lấy lớp (`get<N>()`), dạng tham số `setWeights`/`setBias`,
  `forward()` có ghi trạng thái không, có submodule lồng không. Sai khác thì sửa spec này.
- Nếu backend STL không build được trên MSVC: dừng và hỏi owner (điều kiện Q5), không tự đổi thư viện.

## 3. Lớp `CandidateClassifier`

`src/app/CandidateClassifier.{h,cpp}`, đặc thù sản phẩm nên ở trong app (Q18). Chỉ phụ thuộc
`juce_core`, `juce_audio_basics` và RTNeural; nhận struct `RawInput` riêng để test được độc lập với
`NotchController`.

- `parseWeights(json, error)`: đọc file trọng số của chính app bằng `juce::JSON`, ngoài luồng detector.
  Từ chối `feature_set` sai, sai kích thước, trọng số không hữu hạn hoặc quá lớn. Cố ý không dùng bộ
  nạp nlohmann của RTNeural (đó là đường có cấp phát).
- `load(weights)`: dựng model trên heap, ghi trọng số bằng `setWeights`/`setBias`.
- `score(input) noexcept → optional<float>`: kiểm `isfinite` trên đầu vào thô **trước** mọi phép kẹp
  (`std::max(x, NaN)` trả `x`), `ScopedNoDenormals`, chuẩn hoá, `forward`, kiểm `isfinite` đầu ra,
  kẹp 0..1. Không có ý kiến thì trả rỗng — **không bao giờ** thành một quyết định.
- Kiến trúc: MLP 16 → 16 (tanh) → 8 (tanh) → 1 (sigmoid), 417 tham số. Không dùng softmax (soát mã, bẫy 6).
- Mỗi `NotchController` một bản riêng (8 luồng detector, RTNeural không đồng bộ).
- **Nạp model chỉ khi mọi luồng detector đang dừng** (cùng điều kiện với `setCandidateSink`). Không đổi
  model giữa buổi diễn trong lane này (Q20).

### Bộ đặc trưng `feature_set = 1` (16 số)

Tất cả suy ra được từ cả dòng `candidate_features` lẫn `notch_set`, để sau này chấm lại offline.

| # | Đặc trưng | Kẹp |
|---|---|---|
| 0 | `log2(hz / 1000)` | theo dải tần hợp lệ |
| 1 | `log10(peakiness)` (tỉ số không chặn) | 0..4 |
| 2, 3, 5 | `pNorm`, `rNorm`, `mNorm` (0..1) | 0..1 |
| 4 | `log2(riseRatio)` (tỉ số, trung tính 1,0) | −4..8 |
| 6 | `penalty` (0,5 hoặc 1) | |
| 7 | có làn kia (0/1) | |
| 8 | `log10(lrRatio)`; 0 khi mono | ±3 |
| 9–12 | bin −2, −1, +1, +2 tính bằng dB so với bin giữa | −60..0 |
| 13–14 | trung bình váy dưới / trên (bin ±3..5), dB so với bin giữa | −60..0 |
| 15 | `log10(biên độ bin giữa)` — phụ thuộc gain staging, bỏ được nếu owner muốn | |

Không đưa vào: `score` (tích của các số trên), `asymmetry` (hằng số ở cấu hình mặc định), `thr`, `persist`.
File trọng số mang `feature_set`, `input_offset`, `input_scale`; C++ từ chối file lệch `feature_set`.

## 4. Cổng Q5 — test của C1a

| Test | Khẳng định |
|---|---|
| `CandidateClassifierAlloc.ScoreAllocatesNothingAfterLoad` | 0 lần gọi `operator new` toàn cục quanh 1000 lần `score()` |
| `CandidateClassifierAlloc.CounterSeesAVectorAllocation` | đối chứng dương: bộ đếm có đếm thật |
| `CandidateClassifier.MatchesPythonReferenceWithinTolerance` | chuẩn hoá + forward trên ~64 đầu vào lưu sẵn, sai số tuyệt đối ≤ 1e-5 trên score 0..1 (dung sai là ước lượng, đo rồi chốt) |
| `CandidateClassifier.NonFiniteInputGivesNoOpinion` | NaN, +inf, −inf ở từng trường → rỗng |
| `CandidateClassifier.DenormalInputsStayFiniteAndFast` | biên độ denormal → score hữu hạn |
| `CandidateClassifier.ExtremeFiniteInputsAreClamped` | `riseRatio` 1e12, `lrRatio` 1e-12 → score hữu hạn trong 0..1 |
| `CandidateClassifier.MissingOrCorruptWeightsLeaveItInactive` | rỗng, cụt, sai kích thước, sai `feature_set`, trọng số NaN → nạp thất bại, `score()` rỗng, không sập |
| `CandidateClassifier.UnloadedClassifierHasNoOpinion` | chưa nạp → rỗng |
| `CandidateClassifierBench.ScoreCostIsReported` | in ns mỗi lần gọi; trần lỏng (trung vị < 50 µs) |

Bộ đếm cấp phát thay mọi dạng `operator new`/`delete` toàn cục (kể cả dạng `align_val_t`), chỉ đếm khi
một probe `thread_local` đang bật, và nằm trong **một file chạy riêng** `CandidateClassifierAllocTest`:
thay `operator new` ngay trong `HandsFreeTests` sẽ đè bộ cấp phát của ASan/TSan ở job đêm.

Tham chiếu Python (`tools/nn/reference_forward.py`) chỉ dùng thư viện chuẩn để chạy được trong `ctest`.

## 5. C1b — nối bóng (PR sau)

- Hai điểm gọi, đều chỉ đọc và không giữ khoá: (A) ứng viên suýt đạt, trong `maybeLogCandidate` sau
  bước kiểm hữu hạn, đã nằm sau giới hạn tần suất của C0 (nhiều nhất 1 lần mỗi giây mỗi controller);
  (B) lúc đặt notch, trong `placeConfirmed`.
- Ghi `nn_score` (0..1, làm tròn 3 chữ số có nghĩa) vào **cả** `candidate_features` lẫn `notch_set`.
  Verdict TRUE/FALSE chỉ gắn vào `notch_set`; thiếu trường ở đó thì score không bao giờ có nhãn.
  Không có ý kiến thì bỏ trường, không ghi 0.
- `session_start`: `log_schema: 3`, `nn_model`, `nn_feature_set`. `session_end`: `nn_calls`,
  `nn_us_p99`, `nn_us_max`.
- Model `synthetic-v0` nhúng vào binary (không thêm file vào bộ cài). Nhãn của nó là tổng hợp: score
  **không** phải bằng chứng về nhạc thật, tên model và tài liệu phải nói rõ. `copilot.py` không đọc
  trường này, nên con số không tới LLM.
- Test bắt buộc: chuỗi lệnh notch **giống hệt** khi có và không có bộ phân loại, kể cả khi trọng số bị
  ép cho score ~0 và ~1.
- Ngân sách: p99 < 1 ms trong app chạy đủ 8 slot (đã ghi ở spec C0). Ước lượng chưa đo: cỡ 1 µs mỗi lần gọi.
- Cùng PR: `docs/GIOI-THIEU.md`, `docs/KY-THUAT-CHONG-HU.md` (bảng log và hai chỗ nói lane C mở khi
  ≥ 300 verdict), spec vòng dữ liệu, release note, memory.

## 6. Tài liệu và việc đi kèm C1a

- `docs/release-notes/unreleased/candidate-classifier-runtime.md` với dòng mức ra 0 dB.
- `docs/superpowers/runbooks/parallel-lanes.md` và `docs/GIT-WORKFLOW.md`: thêm submodule mới vào các
  bước dựng / gỡ worktree.
- Phụ lục kết quả đo vào bản soát RTNeural (số lần cấp phát, sai số so khớp, ns mỗi lần gọi).
- `docs/GIOI-THIEU.md` và `docs/KY-THUAT-CHONG-HU.md` **không** đổi ở C1a: không gì nhìn thấy được đổi.

## 7. Chưa kiểm / rủi ro

- Các chi tiết API của RTNeural ở §2 là nhớ lại, chưa đọc ở SHA đã ghim.
- Backend STL không nằm trong CI Windows của chính RTNeural.
- "MSVC và clang": clang chỉ chạy ở job macOS hằng tuần; job Linux dùng GCC. Chạy `workflow_dispatch`
  trên nhánh trước khi merge nếu được.
- Bản cài chưa kèm file ghi công bên thứ ba; BSD-3 của RTNeural đòi có khi mã vào binary (Q22, chờ owner).
- Lệch huấn luyện / chạy thật: log làm tròn 3 chữ số, app dùng float đầy đủ. Chưa đo.
