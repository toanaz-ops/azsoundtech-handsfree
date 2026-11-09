# tools/nn — đường huấn luyện, đánh giá và cổng của bộ phân loại ứng viên (lane C1c)

Mọi thứ ở đây chạy **offline**. Không file nào trong thư mục này đổi hành vi của app: app vẫn
chạy model nhúng `untrained-fixture-v1` ở chế độ bóng. Spec: `docs/superpowers/specs/2026-11-09-candidate-classifier-design.md` §8;
các ngả rẽ: Q23–Q25 trong `docs/superpowers/decisions/2026-11-09-ai-integration.md`.

| File | Việc | Phụ thuộc |
|---|---|---|
| `reference_forward.py` | bảng 16 đặc trưng + forward MLP, nguồn sự thật phía Python (so với `rawFeatures` của C++ bằng `classifier-parity-v1.json`) | stdlib |
| `dataset.py` | log session (schema 2, 3) → dataset JSONL + manifest; cách chia theo session | stdlib |
| `train_mlp.py` | huấn luyện MLP 16-16-8-1, xuất đúng định dạng `CandidateClassifier::parseWeights` | **numpy** |
| `evaluate.py` | AUC, mốc `score` hiện tại, tỉ lệ đặt oan bị bắt ở ngưỡng giữ 95 % hú thật; ghi `eval-report.json` | stdlib |
| `gate.py` | cổng đóng-theo-mặc-định; ngưỡng ở **một chỗ** (`THRESHOLDS`, Q24) | stdlib |
| `check_embedded_models.py` | ctest `nn_embedded_model_gate`: mỗi file CMake nhúng (`HANDSFREE_EMBEDDED_MODELS`) phải là `untrained-*` ghim hoặc qua cổng; `--model-file` | stdlib |
| `slim_log.py` | thu nhỏ log để commit được (chỉ dùng cho log tổng hợp) | stdlib |
| `make_fixture.py` | sinh fixture C1a (trọng số ngẫu nhiên + 64 ca parity) | stdlib |

## Chạy trên log thật, theo thứ tự

Log thật nằm ở `%APPDATA%\AZSoundtech\HandsFree\logs\session-*.jsonl` trên máy tester (lấy qua kênh
alpha). **Không bao giờ commit log thật** — repo công khai, một log mang tên thiết bị và phổ của phòng.
Để chúng ở `tools/nn/data/real/` (đã nằm trong `.gitignore`) và đặt kết quả ở `tools/nn/out/` (cũng bị ignore).

```bash
# 1. dataset: mỗi notch_set của detector có verdict = một dòng; candidate_features đi riêng, không nhãn
python tools/nn/dataset.py --out tools/nn/out/real tools/nn/data/real/session-*.jsonl

# 2. huấn luyện (cần numpy; seed cố định; tập kiểm định tách THEO SESSION; tập giữ lại không đụng tới)
python tools/nn/train_mlp.py --dataset tools/nn/out/real/dataset.jsonl --name candidate-v1 --out tools/nn/out/real/candidate-v1.json

# 3. đánh giá trên các session giữ lại (suy luận bằng reference_forward = cùng phép tính với app)
python tools/nn/evaluate.py --dataset tools/nn/out/real/dataset.jsonl --weights tools/nn/out/real/candidate-v1.json --out tools/nn/out/real/candidate-v1.eval-report.json

# 4. cổng: exit 0 chỉ khi mọi điều kiện đạt; in từng điều kiện trượt
python tools/nn/gate.py --report tools/nn/out/real/candidate-v1.eval-report.json --weights tools/nn/out/real/candidate-v1.json --manifest tools/nn/out/real/manifest.json --meta tools/nn/out/real/candidate-v1.meta.json
```

`dataset.py` đếm mọi dòng bị bỏ theo lý do (`manifest.json` → `dropped`): `no_verdict`, `origin_*`,
`missing_field:<tên>`, `non_numeric:<tên>`, `non_finite`, `no_ctx`, `ctx_now_wrong_length`,
`bin_outside_spectrum`, `conflicting_verdicts`… Đọc nó trước khi tin số hàng.

**Danh tính session là nội dung, không phải tên file.** `session_id` = `s` + 16 hex đầu của sha256 trên nội dung chuẩn hoá của file (mỗi dòng cắt
khoảng trắng, bỏ dòng rỗng, nối bằng LF; CRLF hay LF như nhau). Đổi tên file không đổi session và không đổi cách chia tập; cùng một log giao hai lần
(`a.jsonl`, `a (1).jsonl`) chỉ tính một lần, các bản sau bị bỏ và đếm ở `manifest.json` → `duplicate_sessions` (`identical_content`).

**`source` cần bằng chứng dương.** `real` chỉ khi `session_start` (đúng một dòng): không có `source` khác vắng / `"app"`, không khoá `generator`, không giá trị chuỗi
cấp trên cùng nào chứa "synthetic"; **và** `app_version` khớp `^\d+\.\d+\.\d+$` (chuỗi phiên bản app ghi, ví dụ `1.5.1`); **và** `log_schema` là số nguyên ≥ 2.
Có dấu hiệu tổng hợp → `synthetic`; thiếu bằng chứng, `source` lạ (kể cả `"real"` tự khai), không có `session_start` → `unknown`. Cổng chỉ nhận `real`.

**Một lần đặt LINK = một hàng.** Đặt ở chế độ LINK ghi hai dòng `notch_set` (lane 0, lane 1) từ cùng một sự kiện `scored`: giống hệt nhau ở `slot`, `index`, `hz`, `q`,
`depth_db`, `origin`, `confirmed_lane`, mọi trường điểm, `lr_ratio`, `nn_score` và `ctx.now`; chỉ `t` và `lane` khác. Hai dòng `notch_set` liên tiếp như vậy, khác làn, được gộp.
Một làn có verdict → dùng; hai làn cùng nhãn → dùng; GOOD một làn và FALSE một làn → bỏ lần đặt, lý do `conflicting_verdicts`. Fixture: `tests/fixtures/session-nn-linked.jsonl`.

Qua cổng **chưa** có nghĩa là app dùng nó. Muốn nhúng: (a) chép `candidate-v1.json` vào `models/`
cùng ba file **cạnh nó**: `candidate-v1.eval-report.json`, `candidate-v1.manifest.json` và `candidate-v1.meta.json` (metadata huấn luyện); (b) thêm đường dẫn vào biến
**`HANDSFREE_EMBEDDED_MODELS`** ở `CMakeLists.txt` gốc (một biến duy nhất: cho cả `juce_add_binary_data` lẫn cổng) — một thay đổi mã app, một PR riêng, kèm release note.
ctest `nn_embedded_model_gate` truyền đúng các đường dẫn trong biến đó cho `check_embedded_models.py --model-file ...`, chạy lại cổng trên từng file (bắt băm trọng số lệch),
và **trượt** nếu có JSON dưới `models/` không nằm trong danh sách (thư mục và danh sách nhúng không được lệch nhau). Máy không có Python thì test này **trượt**,
không bỏ qua. Chạy tay không cần CMake: `python tools/nn/check_embedded_models.py` (quét mọi JSON dưới `models/`) hoặc
`python tools/nn/check_embedded_models.py --model-file models/candidate-v1.json` (đúng các file đã nêu). Cho mạng quyền quyết định
notch là bước khác nữa, cần buổi nghe và owner chốt.

**Điểm gọi A nằm ngoài phân bố huấn luyện.** App chấm cả ứng viên suýt đạt (`maybeLogCandidate`, những thứ detector không đặt notch), nhưng hàng huấn luyện chỉ là notch **đã đặt**
có verdict. Model chưa từng thấy ứng viên suýt đạt, nên score của nó ở đó là ngoài phân bố cho tới khi giải quyết (nhãn cho ứng viên suýt đạt, hoặc không dùng score đó). Spec §8.7.

## Nhãn và cách dựng một dòng

Nhãn: verdict `good` → 1 (hú thật), `false` → 0 (đặt oan). Verdict được nối vào `notch_set` đúng bằng
luật của `tools/logstats.py` (import `logstats.summarise`, không viết lại), kể cả notch bị thay thế
(một `notch_set` bị `notch_set` sau trên cùng khoá `(slot, làn, index)` đè trước khi có verdict thì **không** có nhãn).

`RawInput` dựng từ một dòng `notch_set`:

| Trường `RawInput` | Lấy từ | Ghi chú |
|---|---|---|
| `hz`, `peakiness`, `pNorm`, `rNorm`, `mNorm`, `penalty` | `hz`, `peakiness`, `p_norm`, `rise`, `novelty`, `penalty` | log ghi **đủ độ chính xác** |
| `riseRatio` | `rise_ratio` | làm tròn 3 chữ số có nghĩa |
| `hasOtherLane`, `lrRatio` | có `lr_ratio` hay không | app chỉ ghi khi có làn kia; làm tròn 3 chữ số |
| `nearMags[11]` | `ctx.now` quanh bin `lround(hz / ctx.bin_hz)` | **không có trong dòng**, suy ra từ phổ; làm tròn 3 chữ số; ngoài phổ = 0 như app |

Dòng thiếu một trường, trường không phải số, không hữu hạn, thiếu `ctx.now` / sai độ dài / bin ngoài
phổ → **bỏ và đếm**, không bao giờ điền giá trị bịa. Log schema 1 (không có `rise_ratio`) vì vậy
không dùng được. Lệch huấn luyện/chạy thật đo được: xem spec §8.5.

## Tập tổng hợp để khởi động (`data/synthetic-bootstrap-v0/`, `models/synthetic-bootstrap-v0.*`)

Cho đường ống chạy từ đầu tới cuối khi chưa có nhãn thật (hiện `%APPDATA%` chỉ có log schema 1, không nhãn).

**Cách sinh.** `DISABLED_` GoogleTest `SyntheticBootstrap.DISABLED_GenerateSessionLogs`
(`tests/synthetic_bootstrap_generator_test.cpp`): đưa cảnh âm thanh tổng hợp qua một `NotchController`
**thật** (detector, scorer, ngữ cảnh phổ, bộ phân loại bóng) và ghi sự kiện bằng bộ tuần tự hoá thật
của app (`notchEventToVar`, `candidateToVar`), nên dòng log đúng khuôn log app. Nhãn là **cảnh**:

- *hú* → verdict `good`: một sin tần ngẫu nhiên 150–9000 Hz bắt đầu dưới sàn nhiễu, tăng 0,02–0,09 dB/ms
  (75 %) hoặc nhảy bậc (25 %), có khi kèm hài bậc 2, có khi trôi tần ≤ 1,5 Hz;
- *không phải hú* → verdict `false`: nốt có hài (4–10 hài, độ dốc 0,5–1,5, f0 80–1200 Hz) với bao biên độ
  (attack 3–150 ms, suy giảm 0,4–3 s) và rung (0–45 cent, 4,5–6,5 Hz); 20 % là nốt "organ/pad" kéo dài 6–30 s
  không rung — trường hợp giống hú nhất.

Chỉ cảnh mà detector **thật** đã đặt notch mới thành một dòng có nhãn. Tám session (nền nhiễu 0,005–0,03,
mono / stereo khác nhau), 90 cảnh mỗi session, seed cố định trong mã. Hú: 372 cảnh thử, 369 được đặt; nhạc: 348 thử, 207 được đặt →
**576 hàng (369 nhãn 1, 207 nhãn 0)**. Dòng đầu mỗi file có `"source": "synthetic"`.

```bash
HF_SYNTH_OUT_DIR=tools/nn/out/synth-full build/tests/Release/HandsFreeTests.exe --gtest_also_run_disabled_tests --gtest_filter=SyntheticBootstrap.*
```

Rồi thu nhỏ để commit được (log đầy đủ 10,7 MB vì mỗi `notch_set` mang phổ 1025 bin; bản commit 0,64 MB):
`python tools/nn/slim_log.py <vào> <ra>` giữ ±16 bin của `ctx.now` (thêm `ctx.first_bin`) và bỏ `ctx.ref`,
`ctx.other_lane_now`. `dataset.py` cho đúng cùng dataset (cùng sha256) từ bản đầy đủ và bản thu nhỏ — test ghim điều đó.
Bộ sinh dùng `std::mt19937` (chuẩn quy định) nhưng cảnh phụ thuộc `std::sin/pow` của trình dịch: file đã commit là bản gốc,
chạy lại bằng toolchain khác có thể cho số hàng khác chút.

**Giới hạn — đọc trước khi trích bất kỳ con số nào.**

- Nhãn là *loại cảnh*, không phải ý kiến soundman. Mạng học phân biệt "sin tăng dần thuần" với "nốt có hài do
  tôi viết", không phải hú thật với nhạc thật. `memory/peakiness-sweep-2048-2026-09-04.md` đã ghi tín hiệu tổng
  hợp không đại diện nhạc thật.
- Mọi đặc trưng đều có phân bố do tôi chọn tham số. Không có tiếng phòng, không có vòng phản hồi thật (notch
  không được áp lại vào tín hiệu), không có nhạc đa nhạc cụ, không có giọng nói, không có trống.
- Số trong `models/synthetic-bootstrap-v0.eval-report.json` chỉ chứng minh **đường ống chạy** (dataset →
  train → evaluate → gate). AUC model 0,97 ở đó nói về độ dễ của tập tổng hợp. AUC của `score` hiện tại trên
  cùng tập là **0,39 (< 0,5)** (sau khi tái tạo theo danh tính session theo nội dung; trước là 0,29): ở thế giới tổng hợp này detector chấm nốt nhạc *cao hơn* hú — một tính chất
  của bộ sinh (cả hai loại đều đã vượt `kConfirmScore`, hàng nào cũng nằm trong dải hẹp), không phải nhận xét về `score` thật.
- Cổng **luôn** trượt tập này (`data is not all real`), và test `CommittedSyntheticBootstrap` ghim điều đó. Model này
  **không bao giờ** được nhúng vào app (Q25).
- Huấn luyện lại bằng numpy khác phiên bản có thể cho trọng số khác ở chữ số cuối; test chỉ ghim "cùng seed + cùng
  môi trường = trùng từng bit", không ghim băm file đã commit.

## Cổng: những gì nó bảo vệ và không bảo vệ

- **`source` là nhãn tự khai, nhưng `real` cần bằng chứng dương** (luật chính xác ở trên: `app_version` `X.Y.Z`, `log_schema` ≥ 2, không dấu hiệu tổng hợp;
  không có `session_start` hoặc thiếu bằng chứng = `unknown`, cổng từ chối). Cổng chống **tai nạn** (nhúng nhầm tập tổng hợp,
  log không rõ nguồn, trộn file), không chống người cố tình giả log hay giả báo cáo. Trước khi nhúng một model, người phải đọc `manifest.json`
  (số session, nguồn, lý do bỏ dòng).
- **Ba tài liệu phải khớp nhau:** `dataset_sha256` trong báo cáo = trong manifest = trong metadata huấn luyện (tránh "huấn luyện trên A, đánh giá
  trên B"); `weights_sha256` trong báo cáo và trong metadata = băm file trọng số. Manifest và metadata là **bắt buộc**: thiếu một là trượt.
- **Hai con số "giữ 95 % hú thật" và cổng dùng cái nào.** `insample_*` (lạc quan): ngưỡng chọn ngay trên các dòng nhãn 1 của tập giữ lại,
  nên giữ ≥ 95 % là đúng theo cách dựng. `retention_label1` / `reject_label0` (trung thực): ngưỡng chọn trên các session huấn luyện ở mức 95 %, rồi
  áp **nguyên ngưỡng đó** lên tập giữ lại. Cổng đòi cả hai (điều kiện cũ trên số lạc quan, thêm: số trung thực giữ ≥ 90 % và bắt ≥ 30 %).
- **Sàn thống kê (Q24, sửa lần hai, chốt tạm — owner chưa duyệt).** `gate.THRESHOLDS`: ≥ 300 hàng, **≥ 6 session**, tập giữ lại ≥ 60 hàng, **≥ 30 mỗi nhãn**, **≥ 2 session**;
  AUC ≥ 0,80 và ≥ mốc + 0,05; **cận dưới khoảng tin cậy 95 % của AUC model ≥ AUC của `score`**. Lý do: 10 dòng mỗi nhãn cho sai số chuẩn AUC ≈ 0,09 (Hanley–McNeil), nên "mốc + 0,05"
  nằm trong nhiễu. `evaluate.py` ghi `auc_model_se`, `auc_model_ci95_low`, `auc_model_ci95_high`, `heldout.sessions` vào báo cáo.
- **Mép số thực:** AUC đúng bằng AUC mốc + 0,05 có thể trượt vì làm tròn nhị phân (ví dụ 0,78 + 0,05 > 0,83 trong float). Cổng đóng theo mặc
  định nên trượt; không phải lỗi.
- **Mọi tỉ lệ và AUC phải trong [0, 1]; mọi bộ đếm là số nguyên không âm.** Ngoài khoảng là trượt.
- **Model `untrained-*` được ghim theo nội dung** trong `check_embedded_models.py` (`PLACEHOLDERS`: id → sha256 của file). Đổi tên trọng số đã huấn
  luyện thành `untrained-x` không qua được.
