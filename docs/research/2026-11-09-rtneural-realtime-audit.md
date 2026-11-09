# RTNeural — soát mã nguồn về an toàn realtime (điều kiện của Q5)

Ngày 09/11/2026. Lane C, chuẩn bị cho bước C1. Owner chọn RTNeural ở Q5 với điều kiện
"không cấp phát bộ nhớ và không khoá trên lời gọi suy luận". Tài liệu này ghi lại thứ đã
**đọc trong mã nguồn**, tách khỏi thứ chỉ thấy README nói.

Nguồn: `github.com/jatinchowdhury18/RTNeural`, nhánh `main` tại commit
`95c3c0f987a6fe903e7eec71e797405dbed7caf7` (20/08/2026). Đọc qua
`raw.githubusercontent.com` và GitHub API; không build, không chạy.

## Kết luận

Trên đường `ModelT` (model cố định lúc biên dịch) với lớp Dense + tanh/ReLU/sigmoid,
mã nguồn không có `new`, không thao tác `std::vector`, không gọi `std::function`, không
khoá trong `forward()`. Đây là kết luận từ việc đọc mã, **chưa phải bằng chứng lúc chạy**.
Điều kiện Q5 chỉ được coi là thoả sau khi một test đếm cấp phát chạy trong build của mình.

## Thứ đã đọc

| Điểm | Kết quả | Chỗ đọc |
|---|---|---|
| Khoá | Không có mutex, atomic hay lock nào trong toàn bộ thư viện | grep `RTNeural/*.h, *.tpp, *.cpp` |
| `ModelT::forward` | Lớp nằm trong `std::tuple`, đầu ra là mảng cố định | `ModelT.h:57-63, 374-426` |
| Dense (STL) | `std::inner_product` trên mảng cố định | `dense.h:195-207` |
| Activation (STL) | Vòng lặp trên mảng cố định | `activation.h:78-83, 262-276` |
| Dense (Eigen) | `noalias()` trên ma trận kích thước cố định | `dense_eigen.h:163-173` |
| `Model<T>` (động) | Gọi ảo mỗi lớp vào `std::vector` đã cấp sẵn; không cấp phát trong `forward()` | `Model.h:72, 83-93` |
| Cấp phát ở đâu | Constructor của API động, và lúc nạp JSON (`nlohmann::json`, `make_unique`, vector lồng) | `dense.h:28`, `model_loader.h:41, 612-624` |
| Nạp trọng số | `setWeights`/`setBias` ghi tại chỗ; nạp một lần ngoài luồng realtime rồi suy luận | `dense.h:116-142, 215-253` |
| Khởi tạo trễ | Không có trong `forward()` | `ModelT.h:347-348` |
| Denormal, NaN | Thư viện không làm gì; là việc của bên gọi | grep `denormal, FTZ, isnan, isinf` → không có |
| An toàn luồng | Không có đồng bộ; đổi trọng số khi luồng khác đang `forward()` là data race | `dense.h:215-225`, `Model.h:46-47` |
| Giấy phép | BSD-3-Clause | `LICENSE` |

Chưa đọc hết từng dòng: thân `forward` của Dense trên backend xsimd, và Conv1D / BatchNorm
/ LSTM (mới grep, không thấy cấp phát ngoài constructor và `prepare()`).

## Bẫy cần biết trước khi tích hợp

1. **Không có tag hay release nào.** GitHub API trả `[]` cho `/tags` và `/releases`.
   Chỉ ghim được bằng SHA của commit. `CMakeLists.txt` ghi `VERSION 1.0.0` nhưng đã cũ.
2. **Không thuần header-only dưới CMake.** Target `RTNeural` là thư viện `STATIC`
   (chỉ khởi tạo `Model<float/double>`). Dùng header-only được nếu tự đặt define.
3. **FetchContent chưa kiểm.** Các file cmake dùng đường dẫn `modules/...` tính từ gốc
   repo; khả năng chỉ đúng khi RTNeural là thư mục nguồn gốc. Đường ít rủi ro: submodule
   + `add_subdirectory`, hoặc header-only với backend STL. (Suy luận từ mã, chưa thử.)
4. **Backend mặc định dưới CMake là Eigen**, và đường Eigen / xsimd của `ModelT` đòi
   bộ đệm đầu vào **đã căn hàng** (`Eigen::Aligned16/32`, `load_aligned`;
   `ModelT.h:381-383`). Backend STL không có ràng buộc này (`ModelT.h:385`).
5. **Backend STL không nằm trong ma trận CI Windows của RTNeural** (chỉ có Eigen và
   xsimd + AVX). Với MLP cỡ 16→16→8→1 thì STL vẫn là lựa chọn đơn giản nhất: không cần
   thư viện ngoài, không ràng buộc căn hàng. Bù lại mình phải tự test trên MSVC và clang.
6. **Softmax không trừ giá trị lớn nhất** (`activation.h:231, 264-275`): đầu vào lớn ra
   `inf`/`NaN`. Bộ phân loại nhị phân nên dùng sigmoid.
7. **`ModelT` giữ trọng số ngay trong object.** Issue #157 (mở): model GRU lớn làm tràn
   stack. MLP nhỏ không dính, nhưng vẫn nên đặt model trên heap, không đặt trên stack luồng.
8. **`nlohmann/json` 3.11.1 đi kèm** và là include `PUBLIC` của target.
9. **Xuất model:** `python/model_utils.py` chỉ cho Keras Sequential (Dense, GRU, LSTM,
   Conv1D/2D, BatchNorm, tanh/relu/sigmoid/softmax/elu). Lớp lạ xuất thành "Unknown"
   (issue #85: Dropout hay Activation tách riêng làm kết quả sai). PyTorch chỉ có script
   mẫu theo từng lớp, không có bộ xuất chung.
10. Issue liên quan khác: #167 (mở) tiếng "pop" khi gọi `reset()`; #55 (đóng) Eigen ra
    sai khi activation là lớp cuối, đã sửa 2022. Không issue nào báo cấp phát lúc chạy
    hay lỗi riêng của MSVC.

## Điều kiện để C1 được coi là thoả Q5

Phải đo trong build của mình, trên MSVC và clang:

1. Test đếm `operator new`/`delete` quanh `forward()` sau khi khởi động: phải bằng 0.
   Trên Linux clang có thể bật thêm `RTNEURAL_ENABLE_RADSAN`.
2. So đầu ra `ModelT` với đầu ra tham chiếu từ Python trong một dung sai nêu rõ.
3. Đầu vào denormal và `NaN`: bọc `juce::ScopedNoDenormals`, kiểm `isfinite` ở đầu ra;
   `NaN` thì coi như "không có ý kiến", không bao giờ thành một quyết định.
4. Đổi model bằng hai bộ đệm (đổi con trỏ), không ghi trọng số vào model đang chạy.
5. Model đặt trên heap, tạo ngoài luồng detector.

## Đề xuất cho C1 (chờ owner khi tới lượt)

`ModelT` + backend STL, ghim theo SHA, đưa vào bằng submodule; chỉ chạy ở chế độ bóng
(ghi quyết định vào log, không tác động vào notch). Chưa có dữ liệu `candidate_features`
thật thì chưa huấn luyện được: C1 cần C0 tới tay tester trước.

## Phụ lục — Kết quả đo C1a

Đo ngày 09/11/2026 trong build của mình: MSVC (VS Build Tools 2026), Release, x64, backend STL,
RTNeural `95c3c0f`, model 16→16→8→1 (417 tham số) với trọng số fixture ngẫu nhiên có seed.
Chạy: `build/tests/Release/CandidateClassifierAllocTest.exe` và
`build/tests/Release/HandsFreeTests.exe --gtest_filter='CandidateClassifier*'`.

| Điều kiện Q5 | Kết quả |
|---|---|
| 1. Cấp phát quanh `forward()` | `score()` ×1000 (983 có ý kiến, phần còn lại là đầu vào NaN/denormal): `operator new` = **0**, `operator delete` = **0**. Đối chứng dương (`std::vector<int>(100)`): new = 1, delete = 1, nên bộ đếm nhìn thấy cấp phát. |
| 2. So khớp tham chiếu Python | 64 ca (mono + stereo + biên kẹp): sai số tuyệt đối lớn nhất **8,15e-8** trên score 0..1 (dung sai đặt 1e-5). |
| 3. Denormal / NaN | NaN, +inf, −inf ở từng trường thô → không có ý kiến; biên độ denormal và giá trị cực lớn → score hữu hạn trong 0..1. |
| 4. Đổi model bằng hai bộ đệm | Chưa làm: C1a không đổi model khi chạy (Q20). |
| 5. Model trên heap, dựng ngoài luồng detector | Có: `std::make_unique<Net>()` trong `load()`. |

Chi phí: `score()` trung vị **256 ns/lần** (15 lô × 20 000 lần, min 251, max 268) trên máy này, gồm
chuẩn hoá đặc trưng, `ScopedNoDenormals` và forward. Bản thiết kế ước lượng ~1 µs; thực tế thấp hơn.

Chưa đo: clang, GCC, Linux, `RTNEURAL_ENABLE_RADSAN`; số cấp phát trong app chạy đủ 8 slot (thuộc C1b).
Một thay đổi so với bản đọc mã: `RTNeural.h` cần `RTNEURAL_DEFAULT_ALIGNMENT` được định nghĩa, nếu
không mỗi file in một `#pragma message` (không phải lỗi).
