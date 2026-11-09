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
| **C1b** (= "C2" của spec C0) | Nối vào luồng detector ở chế độ bóng, model `untrained-fixture-v1`, ghi `nn_score` vào log | log có thêm trường; không đổi âm thanh, không đổi quyết định notch |
| **C1c** (§8) | đường huấn luyện offline, bộ đánh giá, cổng, tập tổng hợp khởi động. **Không đổi mã app**, model nhúng vẫn `untrained-fixture-v1` | không |
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
- **Đã kiểm ở đúng SHA (C1a, 09/11/2026):** các chi tiết bản thiết kế nhớ lại đều đúng, trừ ba
  điểm sửa bên dưới.
  - Backend: không có define `RTNEURAL_USE_STL` nào; STL là thứ còn lại khi `RTNEURAL_USE_EIGEN` và
    `RTNEURAL_USE_XSIMD` đều không được định nghĩa (`#if` của macro chưa định nghĩa = 0). Cả hai đều
    bị `#error` trong `CandidateClassifier.cpp`.
  - `ModelT<T, in_size, out_size, Layers...>`; lấy lớp bằng `get<int Index>()` (chỉ số tính cả lớp
    activation: Dense = 0, 2, 4).
  - `DenseT::setWeights` nhận `const std::vector<std::vector<T>>&` hoặc `T**`, hình `[out][in]`;
    `setBias` nhận `const T*` dài `out`. (Sửa: spec cũ chưa nêu hình.)
  - `forward(const T*)` **có ghi trạng thái**: `outs` của từng lớp và của model. Không ghi trọng số.
    Nghĩa là một instance không được gọi từ hai luồng; trọng số thì không bao giờ bị đổi trong `forward`.
  - `ModelT.h` include `model_loader.h` ⇒ kéo `modules/json/json.hpp` (nlohmann). Chỉ tốn thời gian
    biên dịch; file nằm trong cây RTNeural (không phải submodule lồng), và `modules/Eigen`,
    `modules/xsimd` không cần cho đường STL. **Không** init submodule lồng.
  - (Sửa) Cần `RTNEURAL_DEFAULT_ALIGNMENT=16` đặt trong target INTERFACE, không thì mỗi file include
    RTNeural in một `#pragma message`. Backend STL không dùng giá trị này để căn hàng đầu vào.
  - (Sửa) Dự án build C++17 (`CMAKE_CXX_STANDARD 17`), không phải C++20.
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
| 0 | `log2(hz / 1000)` | hz kẹp vào 20..20000 trước khi lấy log |
| 1 | `log10(peakiness)` (tỉ số không chặn) | 0..4 |
| 2, 3, 5 | `pNorm`, `rNorm`, `mNorm` (0..1) | 0..1 |
| 4 | `log2(riseRatio)` (tỉ số, trung tính 1,0) | −4..8 |
| 6 | `penalty` (0,5 hoặc 1) | |
| 7 | có làn kia (0/1) | |
| 8 | `log10(lrRatio)`; 0 khi mono | ±3 |
| 9–12 | bin −2, −1, +1, +2 tính bằng dB so với bin giữa | −60..0 |
| 13–14 | trung bình váy dưới / trên (bin ±3..5), dB so với bin giữa | −60..0 |
| 15 | `log10(biên độ bin giữa)` — phụ thuộc gain staging, bỏ được nếu owner muốn | −12..6 |

Chi tiết số học cố định bởi `tools/nn/reference_forward.py` (và test so khớp): sàn `1e-12` cho biên
độ / `riseRatio` / `lrRatio` trước khi lấy log; `peakiness` sàn 1; "dB so với bin giữa" là
`20·log10(max(m, 1e-12) / max(giữa, 1e-12))`; váy dưới/trên là **trung bình tuyến tính** của biên độ
ba bin (−5..−3, +3..+5) rồi mới đổi dB; mono thì đặc trưng 7 và 8 bằng 0.
Kiểm `isfinite` áp lên **mọi** trường thô (kể cả `lrRatio` khi mono) trước mọi phép kẹp.

Không đưa vào: `score` (tích của các số trên), `asymmetry` (hằng số ở cấu hình mặc định), `thr`, `persist`.
`input_scale` phải thoả `1e-6 <= |scale| <= 1e6` (không 0: một đặc trưng bị nhân 0 là đầu vào chết); `input_offset` hữu hạn, `|offset| <= 1e6`.
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

Tên suite thật: `CandidateClassifierTest` (cần model nạp sẵn), `CandidateClassifierLoad`,
`CandidateClassifierBench`, `CandidateClassifierAlloc`; filter `CandidateClassifier*` bắt hết. Thêm một
test `RawFeaturesFollowTheSpecTable` ghim bảng đặc trưng. Một ctest `classifier_reference_parity` chạy
chính tham chiếu Python trên cùng file khớp, nên hai phía cùng bị ghim vào một bộ số.

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
- Model `untrained-fixture-v1` nhúng vào binary (không thêm file vào bộ cài): chính bộ trọng số mẫu
  **chưa huấn luyện** của C1a (Q17, bản sửa 09/11). Score của nó **vô nghĩa** về hú hay nhạc; nó chỉ
  chứng minh đường suy luận chạy ổn trên máy tester. Tên model và tài liệu phải nói rõ. `copilot.py`
  không đọc trường này, nên con số không tới LLM. Huấn luyện để tới khi có dữ liệu thật.
- Test bắt buộc: chuỗi lệnh notch **giống hệt** khi có và không có bộ phân loại, kể cả khi trọng số bị
  ép cho score ~0 và ~1.
- Ngân sách: p99 < 1 ms trong app chạy đủ 8 slot (đã ghi ở spec C0). Ước lượng chưa đo: cỡ 1 µs mỗi lần gọi.
- Cùng PR: `docs/GIOI-THIEU.md`, `docs/KY-THUAT-CHONG-HU.md` (bảng log và hai chỗ nói lane C mở khi
  ≥ 300 verdict), spec vòng dữ liệu, release note, memory.

### 5.1 Đã hiện thực (C1b, 09/11/2026) — chỗ thực tế khác bản thiết kế

- Model nhúng bằng target `juce_add_binary_data` riêng `HandsFreeModels` (namespace
  `HandsFreeModelsData`, header `HandsFreeModelsData.h`) vì `HandsFreeFonts` đã giữ `BinaryData`.
  `src/app/EmbeddedClassifier.{h,cpp}` bọc việc nạp; file trọng số là
  `models/candidate-classifier-untrained-v1.json` (cùng số với fixture C1a, chỉ khác `model`).
- Điểm B chỉ chấm cho notch `Origin::Detector` (soundcheck không có `nn_score`), và **một lần chấm
  mỗi lần đặt**: ở chế độ LINK một lần đặt phát hai event Set dùng chung một điểm.
- Bộ đếm thời gian: mảng cố định 32 nhóm log2 của `getHighResolutionTicks()`, công bố sang atomic ở
  cuối mỗi `runOnce()` có lần chấm mới và trong `stop()` sau khi join (spec ban đầu chỉ nói
  "khi dừng"; công bố thêm ở cuối `runOnce` để `session_end` đóng tay khi luồng còn chạy không
  đọc số cũ). `nn_us_p99` là cạnh trên của nhóm (chặn bởi max chính xác) của slot **xấu nhất**, không
  phải p99 gộp.
- `SessionLogger::setSessionEndExtraFields`: hook chung, trường chuẩn đi trước và hook không ghi
  đè được. Khi model không nạp được: không controller nào có bộ phân loại, `session_start` vẫn
  `log_schema: 3` nhưng **không có** `nn_model` / `nn_feature_set`, `session_end` không có `nn_*`.
- Số đo (kịch bản `ShadowClassifier.*`, 414 block = 4,4 s đồng hồ mô phỏng): 19 lần chấm (3 ứng
  viên + 16 lần đặt), p99 và max cỡ 1–7 µs mỗi lần trên máy dựng, tức ~4 lần chấm/giây mô phỏng.
  Trong app thật điểm A bị chặn ≤ 1 lần/giây/controller, điểm B chỉ khi đặt notch.
- Test: `ShadowClassifier.*` (5 test, trong `tests/test_notchcontroller.cpp`) và
  `GuiWiring.NnScoreReachesBothLogLinesWithModelIdAndSchema3` /
  `GuiWiring.SessionEndCarriesNnCallCountAndTiming`; ctest `logstats_nn_shadow`.
  Test `NonFiniteFeaturesGiveNoScoreNotZero` không thể đưa NaN tới điểm B qua detector thật
  (scorer không xác nhận trên NaN, cổng hữu hạn của C0 chặn điểm A), nên nó khẳng định
  bộ phân loại từ chối và không có điểm nào không hữu hạn nào lọt qua khi cho tap ăn NaN.

## 6. Tài liệu và việc đi kèm C1a

- `docs/release-notes/unreleased/candidate-classifier-runtime.md` với dòng mức ra 0 dB.
- `docs/superpowers/runbooks/parallel-lanes.md` và `docs/GIT-WORKFLOW.md`: thêm submodule mới vào các
  bước dựng / gỡ worktree.
- Phụ lục kết quả đo vào bản soát RTNeural (số lần cấp phát, sai số so khớp, ns mỗi lần gọi).
- `docs/GIOI-THIEU.md` và `docs/KY-THUAT-CHONG-HU.md` **không** đổi ở C1a: không gì nhìn thấy được đổi.

## 7. Chưa kiểm / rủi ro

- Chi tiết API ở §2 đã đọc ở SHA đã ghim (C1a) và build được trên MSVC (VS 2026). Chưa thử clang/GCC.
- Backend STL không nằm trong CI Windows của chính RTNeural.
- "MSVC và clang": clang chỉ chạy ở job macOS hằng tuần; job Linux dùng GCC. Chạy `workflow_dispatch`
  trên nhánh trước khi merge nếu được.
- Bản cài chưa kèm file ghi công bên thứ ba; BSD-3 của RTNeural đòi có khi mã vào binary (Q22, chờ owner).
- Lệch huấn luyện / chạy thật: log làm tròn 3 chữ số, app dùng float đầy đủ. **Đã đo ở C1c** (§8.5): tối đa 7,8e-4 trên score 0..1.

## 8. C1c — đường huấn luyện, đánh giá và cổng (thêm 09/11/2026)

Owner (qua phiên điều phối, 09/11): đưa lane C1 qua mức "model chưa huấn luyện chạy bóng" — làm đường
huấn luyện, bộ đánh giá có tập giữ lại và một test cổng; thiếu log thật thì khởi động bằng fixture
hoặc một tập tổng hợp có ghi rõ; giữ bộ phân loại ở chế độ bóng tới khi cổng đạt. Các ngả rẽ: Q23–Q25.

> **Mức ra thay đổi dự kiến: 0 dB.** C1c không đổi mã app: chỉ thêm công cụ offline dưới `tools/nn/`,
> test, và tài liệu. Model nhúng trong app vẫn là `untrained-fixture-v1`.

Hiện trạng dữ liệu (đo 09/11/2026 trên máy build): 9 file log ở `%APPDATA%`, đều `log_schema` 1,
không có notch, không có verdict. **Chưa có một nhãn thật nào.**

### 8.1 Dữ liệu và nhãn

- Đầu vào: các file `session-*.jsonl` (schema 2 và 3). Một dòng dữ liệu = một lần đặt notch gốc
  detector (`notch_set`) **có verdict** của soundman: TRUE → nhãn 1 (hú thật), FALSE → nhãn 0 (đặt oan).
  Cách nối verdict vào notch dùng lại đúng luật của `tools/logstats.py` (kể cả notch bị thay thế).
- Dòng `candidate_features` (suýt đạt, không ai chấm) **không có nhãn**: không dùng để huấn luyện,
  chỉ đếm và dùng để xem phân bố score.
- 16 đặc trưng tính từ dòng log bằng **cùng một mã** với bản tham chiếu `reference_forward.py`
  (một nguồn sự thật cho bảng đặc trưng ở phía Python; test so với `rawFeatures` của C++).
- Mỗi dòng mang `session_id` và `source` (`real` hoặc `synthetic`). Log thật không được commit vào
  repo (repo công khai); chỉ fixture và tập tổng hợp được commit.

### 8.2 Huấn luyện

`tools/nn/train_mlp.py`, chỉ cần numpy, chạy offline (không nằm trong `ctest`): MLP 16-16-8-1 đúng
kiến trúc của app, chuẩn hoá đầu vào thành `input_offset` / `input_scale`, seed cố định, dừng sớm
theo tập kiểm định, xuất đúng định dạng JSON mà `CandidateClassifier::parseWeights` đọc, kèm tên model
và băm của tập dữ liệu.

### 8.3 Đánh giá và cổng

- Chia tập **theo session** (không để cùng một buổi diễn nằm ở cả hai phía).
- Số đo: ROC-AUC của mạng; ROC-AUC của `score` hiện tại trên cùng tập (mốc so sánh); ở ngưỡng giữ được
  ≥ 95 % hú thật: tỉ lệ đặt oan bị bắt. Suy luận lúc đánh giá dùng bản tham chiếu thư viện chuẩn, tức
  cùng phép tính với app.
- **Cổng** (`tools/nn/gate.py`, đóng theo mặc định). Một model chỉ được nhúng vào app khi báo cáo đánh
  giá của nó thoả **tất cả**: dữ liệu `real`; ≥ 300 notch có verdict từ ≥ 3 session; tập giữ lại có
  ≥ 60 dòng và đủ cả hai nhãn; AUC ≥ 0,80 **và** ≥ AUC của `score` + 0,05; giữ ≥ 95 % hú thật thì bắt
  được ≥ 30 % đặt oan; **và** (sửa 09/11/2026) ngưỡng chọn trên các session huấn luyện ở mức 95 % giữ ≥ 90 % hú thật và bắt ≥ 30 % đặt oan trên tập giữ lại.
  Manifest và metadata huấn luyện bắt buộc, ba băm phải khớp. Các ngưỡng là chốt tạm (Q24).
- Test cổng trong `ctest`: model nhúng trong app hoặc mang tên `untrained-*`, hoặc phải có báo cáo
  đánh giá đạt cổng nằm cạnh, băm trọng số khớp. Tập tổng hợp **không bao giờ** qua được cổng.
- Qua cổng mới chỉ là "được nhúng để chạy bóng với score có nghĩa". Cho mạng quyền quyết định notch
  vẫn là bước riêng, cần buổi nghe và owner chốt.

### 8.4 Tập tổng hợp để khởi động

Để đường ống chạy được từ đầu tới cuối khi chưa có log thật: một bộ sinh có ghi rõ cách làm, tạo log
đúng định dạng session (kèm dòng verdict làm nhãn) từ tín hiệu tổng hợp — hú = sin tăng dần; không
phải hú = nốt có hài, có bao biên độ và rung. Model huấn luyện từ đó tên `synthetic-bootstrap-v0`,
nằm dưới `tools/nn/`, **không nhúng vào app**. `memory/peakiness-sweep-2048-2026-09-04.md` đã ghi nguồn
tổng hợp không đại diện cho nhạc thật: số đo trên tập này chỉ chứng minh đường ống chạy, không nói
gì về hiệu quả ngoài hiện trường.

### 8.5 Đã hiện thực (C1c, 09/11/2026) — chỗ thực tế khác bản thiết kế

Công cụ: `tools/nn/{dataset,train_mlp,evaluate,gate,check_embedded_models,slim_log}.py`, hướng dẫn chạy ở
`tools/nn/README.md`. Test: `tests/test_nn_pipeline.py` (ctest `nn_pipeline`; các ca cần numpy tự bỏ
khi thiếu), `nn_embedded_model_gate`, và `CandidateClassifierLoad.ExportedTrainedModelLoadsAndMatchesPythonReference`
(C++ nạp file do `train_mlp.py` xuất qua `parseWeights` + RTNeural, sai số 8,9e-8 so với tham chiếu Python).

- **Từ vựng verdict là `good` / `false`** (không phải TRUE/FALSE): `good` → nhãn 1, `false` → nhãn 0.
- **`nearMags` không có trong dòng `notch_set`.** Dựng lại từ `ctx.now` (phổ cả 1025 bin) quanh bin `lround(hz / ctx.bin_hz)`,
  ngoài phổ = 0 như app. Mọi trường khác của `RawInput` có sẵn: `hz`, `peakiness`, `p_norm`, `rise` (= rNorm), `novelty`
  (= mNorm), `penalty` ở độ chính xác đủ; `rise_ratio`, `lr_ratio` và phổ làm tròn 3 chữ số; `hasOtherLane` = có
  `lr_ratio`. Dòng không dựng lại được bị bỏ và đếm theo lý do, không điền bịa. Log schema 1 (không `rise_ratio`) và các
  fixture viết tay (`ctx` 4 bin hoặc không `ctx`) vì vậy không cho hàng nào.
- **Lệch do làm tròn (đo):** trên 576 dòng tổng hợp, so `nn_score` mà app ghi (tính từ float đầy đủ, rồi làm tròn 3 chữ số)
  với điểm Python tính trên `RawInput` dựng lại từ log, cùng trọng số `untrained-fixture-v1`: sai lệch tuyệt đối tối đa
  **7,8e-4**, trung bình 2,6e-4; sau khi làm tròn điểm Python về 3 chữ số, 554/576 trùng hẳn và còn lại lệch đúng một đơn vị
  chữ số thứ ba. Mô hình chưa huấn luyện có độ nhạy thấp nên đây là cận dưới; một model đã huấn luyện dốc hơn thì lệch có thể lớn hơn
  — đo lại khi có model thật.
- **Đặc trưng lưu ở 10 chữ số có nghĩa** trong dataset: `log2` / `log10` lấy từ libm của nền tảng (glibc khác CRT Windows ở bit cuối), mà `dataset_sha256` được ghim trong báo cáo và test phải giống nhau trên mọi máy.
- **Chia tập:** thứ tự `sha256("hf-c1c-split-v1:" + session_id)`, 25 % đầu (ít nhất 1) là tập giữ lại; `train_mlp.py` chia tiếp tập huấn luyện
  làm fit / kiểm định bằng cùng cách (muối `/val`). Cần ≥ 2 session.
- **Cổng:** thêm một điều kiện chặt hơn Q24: tập giữ lại cần ≥ 10 dòng **mỗi nhãn** (không chỉ "đủ cả hai nhãn": 59 so với 1 không nói gì).
  Cổng còn kiểm `retention_target` / `retention_achieved` ≥ 0,95, tổng số hàng khớp thành phần `source`, và (nếu có manifest) băm dataset khớp.
  Mọi trường thiếu / NaN / không phải số là một điều kiện trượt. Ngưỡng ở một chỗ: `gate.THRESHOLDS`.
- **Test cổng trong ctest:** `nn_embedded_model_gate` đọc mọi JSON dưới `models/`; model nhúng hôm nay (`untrained-fixture-v1`) qua
  vì tên `untrained-*`. Test đơn vị chứng minh nó sẽ trượt với một model không-`untrained` không có báo cáo, và với báo cáo tổng hợp.
- **Tập tổng hợp dùng detector thật**, không phải bộ sinh ở không gian đặc trưng: một test `DISABLED_` trong binary test
  (`SyntheticBootstrap.DISABLED_GenerateSessionLogs`) chạy cảnh âm thanh qua `NotchController` thật. Detector đặt notch cho 369 / 372
  cảnh hú và 207 / 348 cảnh nhạc nên có đủ hai nhãn không cần bộ sinh dự phòng. Nhãn là loại cảnh. Chi tiết và giới hạn: `tools/nn/README.md`.
  Log commit ở dạng thu nhỏ (`ctx.now` ±16 bin + `ctx.first_bin`, bỏ `ctx.ref` / `other_lane_now`) để còn 0,64 MB; `dataset.py` cho cùng sha256 từ hai dạng.
  Dòng `session_start` mang `source: "synthetic"` (trường **mới**, app không bao giờ ghi; thiếu = `real`).
- **Số đo trên tập tổng hợp** (tập giữ lại = session s6, s7: 154 dòng, 89 nhãn 1, 65 nhãn 0): AUC model **0,972**; AUC `score` hiện tại
  **0,288**; giữ 95,5 % hú thì bắt 93,8 % đặt oan (`score`: 4,6 %). Cổng trượt đúng vì `data is not all real` (và manifest liệt kê nguồn không-real);
  mọi điều kiện về kích thước và AUC đều đạt, nên `real` là điều kiện duy nhất chặn. Các số này chỉ chứng minh đường ống chạy.
- Chưa làm: chưa chạy trên log thật (chưa có nhãn thật nào); chưa kiểm numpy phiên bản khác; chưa đo lệch làm tròn với model đã huấn luyện từ log thật.

### 8.6 Sửa sau kiểm chứng độc lập (C1c, 09/11/2026)

Người kiểm chứng độc lập tái tạo đúng số tổng hợp và không thấy rò rỉ; các lỗ hổng cổng sau đã được vá (mỗi lỗ có test):

- AUC và mọi tỉ lệ phải thuộc [0, 1], bộ đếm là số nguyên không âm (trước đó `auc_model = 1.5` qua cổng).
- Id `untrained-*` không còn tự động được phép: danh sách ghim `{id: sha256}` trong `check_embedded_models.py`; trọng số đã huấn luyện đổi tên thành `untrained-x` trượt.
- Log không có `session_start` có `source = unknown` (cổng từ chối); `session_start` thiếu `source` vẫn là `real` (đó là điều app ghi).
- Manifest và metadata huấn luyện (`<weights>.meta.json`) bắt buộc; `dataset_sha256` ở báo cáo, manifest và metadata phải trùng, `weights_sha256` phải trùng file.
  Model nhúng cần đủ `.eval-report.json`, `.manifest.json`, `.meta.json` cạnh nó.
- Ngưỡng 95 % trước đây chọn ngay trên các dòng nhãn 1 của tập giữ lại (lạc quan). Nay báo cáo có hai cặp số: `insample_*` (lạc quan, giữ để so) và
  `retention_label1` / `reject_label0` ở ngưỡng chọn trên các session huấn luyện (trung thực, cổng dùng). Trên tập tổng hợp: ngưỡng 0,362; giữ 96,6 % nhãn 1, bắt 86,2 % nhãn 0
  (số lạc quan: bắt 93,8 %). Cổng vẫn trượt tập này, chỉ vì lý do `source`.
- Giới hạn cần biết: `source` là nhãn tự khai (cổng chống tai nạn, không chống giả mạo; người phải đọc manifest trước khi nhúng); AUC đúng bằng mốc + 0,05
  có thể trượt do làm tròn nhị phân (đóng theo mặc định).
