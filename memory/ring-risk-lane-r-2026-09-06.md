# Bài học từ lane R (chip RING RISK có dữ liệu) — 2026-09-06

**Bối cảnh:** 3 task (R1 publish score, R2 banding + hold, R3 wiring + docs),
readout thuần, **0 dB**. Spec `docs/spec-ring-risk.md` viết 25/08 và plan viết
27/08 đều đã lỗi thời so với code sau lane S + lane D; khối "Amendment lane R —
2026-09-06" trong `docs/superpowers/plans/2026-08-27-next-wave.md` là thứ
thắng, không phải spec. Suite 451 → 454.

## 1. Khối publish phải nằm DƯỚI vòng detection

`runOnce()` publish snapshot **trước** khi `processSpectrumForDetection` chạy,
nên tại điểm publish **chưa có score của frame đó**. Ghi score ở đó = luôn trễ
một hop (~10,7 ms) → chip đỏ *sau* khi notch đã hiện, đúng thứ acceptance 2 cấm.
Cách sửa: gom `frameMaxScore_`/`frameScoreValid_` trong vòng detection rồi **dời
cả khối publish xuống dưới** — score, phổ, danh sách notch ra cùng một lần khóa
`snapshotMutex_`. Không lock mới, không thread mới. Danh sách notch vẫn gom
*trước* khi chấm, và chính điều đó làm notch của frame này rơi sang snapshot kế
tiếp — tức chip đi trước bảng một hop.

## 2. Đơn vị của score: 0..1 vs kConfirmScore, KHÔNG phải peakiness 10

Spec ghi band theo `PeakinessAnalyzer::getThreshold()` (5..20, mặc định 10.0).
Nhưng `score` là **tích 0..1** (`CandidateScorer.cpp`), so với
`kConfirmScore = 0.7`. `score ≥ 10.0` không bao giờ xảy ra → nếu làm đúng như
spec thì chip vĩnh viễn LOW và không ai phát hiện, vì test cũng sẽ viết theo
spec. Ngưỡng được **publish trong snapshot** (`ringRiskThreshold`) để GUI không
hardcode gì. Bài học chung: khi spec nói "so với ngưỡng X", kiểm tra **đơn vị**
của cả hai vế trước khi viết dòng đầu tiên.

## 3. `hasHistory()` chứ không phải "đã có frame nào chưa"

`ringRiskValid` cần "scorer đã có lịch sử **kể từ lần reset gần nhất**", không
phải `historyCount_ > 0`: sau khi đổi sample rate / `setWidth`, history cũ vẫn
còn nhưng nó thuộc về một cấu hình khác — chấm theo nó là chấm phòng của quá
khứ. Giải pháp là bộ đếm `blocksSinceReset` trong `LaneAnalysis` (bão hòa,
không wrap), đọc **trước** khi commit frame hiện tại.

## 4. Hold phải nạp lại trên frame BẰNG mức đang giữ

Hold 750 ms nếu chỉ nạp lại khi **bước lên** thì với score nằm đúng mép band,
chip chớp xuống một frame khoảng mỗi 750 ms, mãi mãi. Phải nạp lại mỗi frame mà
mức thô ≥ mức đang giữ. Lỗi này lọt qua vòng review đầu của R2.

## 5. `setController` phải xóa cả trường ĐANG VẼ, không chỉ hold

Reset `ringRiskHold_` thôi vẫn để lại `ringRisk_` (trường `paint()` đọc) mang
Critical của slot vừa rời — `repaint()` trong `setController` chạy trước
`timerCallback` kế tiếp, nên người dùng thấy đúng một frame sai slot. Lỗi này
cũng chỉ lộ ở vòng review thứ hai của R2.

## 6. `NotchController::setSampleRate` không có caller production

Phát hiện ở R1, chưa sửa (ngoài phạm vi lane R): đổi device / đổi sample rate
chỉ tới controller qua `setWidth`, vốn tình cờ reset đúng phần state cần reset.
`setSampleRate` hiện chỉ được gọi từ test và `tools/snapshot.cpp`. Đã ghi vào
`docs/spec-ring-risk.md` mục "Known gaps (owner)". Ai đọc hàm đó sẽ tưởng đường
device gọi nó — không.

## 7. Test GUI trên clock thật: phải trả bằng thời gian thật

`MainComponent` dùng `JuceMonotonicClock`, **không inject được** như
`FakeClock` của `tests/test_notchcontroller.cpp`. Trục "rise" của
`CandidateScorer` so với frame cũ ít nhất `0.45 × riseReferenceMs` (~202 ms), và
đồng hồ của scorer chỉ nhích theo khoảng cách **thật** giữa hai block được drain.
Vòng lặp bơm hop liên tiếp → `rNorm = 0` vĩnh viễn, score = 0, dù tín hiệu có hú
cỡ nào. Công thức nhỏ nhất chạy được: **1 frame ồn nhẹ → `Thread::sleep(260)` →
các frame tone**. Suite hiện trả 2 × ~0,5 s cho hai test đó.

## 8. Ảnh snapshot: chip CRITICAL là DÀN DỰNG, và phải nói rõ

`tools/snapshot.cpp` chạy với detection **tắt**, nên đọc trung thực là N/A —
đúng bằng ảnh idle. Muốn ảnh live cho chủ sở hữu thấy chip sáng thì set thẳng
`ringRiskForTest()` (accessor R2 đã có) rồi mới chụp, **in một dòng ra stdout
nói rõ là dàn dựng**. Không đi arm detection + bơm hú: notch tự đặt sẽ phá ba
mốc tuổi mà ảnh live tồn tại để cho thấy. Ảnh chứng minh chip **trông** thế nào
khi sáng; chuyện chip lên được Critical từ detector thật là việc của test.


## 9. Provider phải trả lời được cả trạng thái DỪNG, không chỉ trạng thái chạy

Snapshot **đóng băng** khi không còn block nào tới: rig dừng hoặc đang restart
thì không ai publish nữa, bản ghi cuối vẫn đọc được nguyên vẹn, và hold 750 ms
lại được nạp lại trên đúng band cũ đó. Kết quả: chip có thể nằm đỏ CRITICAL
trên một PA đang im. Một provider chỉ dịch "số cuối cùng tôi đọc được" là chưa
đủ — nó phải trả lời được câu "nguồn của tôi còn sống không".

Rào chắn hiển nhiên (`! engine_.isRunning()` → Unavailable ngay đầu lambda)
**không ship được** với bộ test hiện tại: không test headless nào mở device,
nên `engine_.isRunning()` luôn false, và rào chắn biến RING RISK thành N/A
vĩnh viễn trong suite — đo được: 2 test xanh (`RingRiskGoesCritical...`,
`RingRiskFollowsTheMonitoredSlot...`) đổ ngay, vì chúng lái detector bằng
`runOnce()` thủ công trong khi engine đứng yên. Bài học quy trình: **một ruling
"thêm guard X" phải được đối chiếu với fixture trước khi ghi vào spec** — ở đây
fixture mô phỏng detector đang chạy mà không có cờ chạy nào bật, nên mọi guard
dựa trên sự thật-chạy đều đụng nó. Đường còn lại (cổng "cũ quá" theo
`lastDataMs_`/sequence number) vá được cả rig-dừng lẫn tap-chết và test headless
được — đang chờ chủ sở hữu quyết.
