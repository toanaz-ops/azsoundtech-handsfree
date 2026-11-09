# Lane C1 (candidate classifier, C1a runtime + C1b shadow wiring) - 2026-11-09

- Backend RTNeural: KHONG co define `RTNEURAL_USE_STL`. STL la thu con lai khi ca `RTNEURAL_USE_EIGEN` va `RTNEURAL_USE_XSIMD` deu chua dinh nghia (`#if` macro chua dinh nghia = 0). Nen `CandidateClassifier.cpp` `#error` neu mot trong hai bi define.
- `RTNEURAL_DEFAULT_ALIGNMENT=16` dat trong target INTERFACE `az_rtneural_headers` de dap pragma message "not defined" moi TU; backend STL khong dung gia tri do.
- Thay `operator new` toan cuc KHONG song duoc trong binary build duoi ASan/TSan (de bo cap phat cua sanitizer). Test dem cap phat nam trong executable rieng `CandidateClassifierAllocTest` sau `HANDSFREE_BUILD_ALLOC_TEST`.
- `std::max (x, NaN)` tra ve `x`: kep gia tri khong phai la guard. `isfinite` tren moi truong tho phai chay TRUOC moi phep kep (`rawFeatures`).
- `forward()` cua RTNeural GHI trang thai (outs cua lop) nen mot instance / controller, khong chia se giua luong.
- Shadow = classifier chi doi log. Chung minh bang test lap lai CUNG chuoi block voi 4 bien the (khong / model nhung / trong so ep ~0 / ep ~1) va so sanh TOAN BO chuoi `NotchCommand` + moi truong cua event ngoai score, khong dem. Test phai co "teeth": kich ban phai that su dat notch va ghi candidate, va classifier phai that su chay (`nnStats().calls > 0`).
- Harness mot tap = slot width 2 nhung `effectiveLinked()` true => MOT lan dat notch phat HAI event Set chung mot score; so cuoc goi `score()` = so candidate + so lan dat (= so Set / 2), khong phai so Set. Chi tinh o `placeConfirmed` (site B), khong o `setNotchImpl`, nen LINK khong cham doi.
- Hai diem goi deu doc-chi, khong giu khoa: A trong `maybeLogCandidate` sau cong `candidateFeaturesFinite` va sau gioi han 1 dong/giay (nen <= 1 goi/giay/controller); B trong `placeConfirmed` sau khi `modelMutex_` nha, chi voi `Origin::Detector`. Site B khong co cong huu han rieng, nen cong cua `score()` la cong duy nhat.
- Thoi gian goi: mang dem log2 theo `juce::Time::getHighResolutionTicks()`, mang co dinh, khong cap phat; cong bo ra atomic o cuoi `runOnce()` (khi co goi moi) va trong `stop()` sau join. p99 = canh tren cua nhom 2^k tick, chan boi max chinh xac; session_end lay p99 cua slot XAU NHAT (can tren cho ca tap).
- Model nhung bang `juce_add_binary_data` rieng (`HandsFreeModels`, namespace `HandsFreeModelsData`, header `HandsFreeModelsData.h`) vi `HandsFreeFonts` da chiem `BinaryData`. Ten bien: `candidateclassifieruntrainedv1_json`. Phai noi vao CA BA target dung `HANDSFREE_CORE_SOURCES` (app, tests, snapshot).
- `SessionLogger` khong duoc biet ten truong nn: hook chung `setSessionEndExtraFields`; truong chuan di truoc va hook khong ghi de duoc.
- Tool Bash cua harness nuot mot cap `\`: viet chuoi C++ co `\n` qua heredoc Python se ra xuong dong that; dung `chr(92)` hoac Write tool.
- Chi doc `copilot.py`: `notch_context` dung whitelist `keep` (khong co `nn_score`) va `session_summary` liet ke khoa tay, nen con so khong toi LLM.
