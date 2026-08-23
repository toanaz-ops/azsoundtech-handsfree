# Bridge lifecycle — restart paths live in DevicePanel, not MainComponent

2026-08-23 · session landing the audio↔detector bridge (commits `81b2969..bff5eaf`)

## Bài học

Plan Task 13 giả định các đường restart thiết bị nằm ở nơi sở hữu `AudioEngine`
(MainComponent). Thật ra **mọi restart thật** (`restartWith`, `applySampleRate`,
`applyBufferSize`) đều nằm trong **`DevicePanel`** — panel GUI gọi thẳng
`engine_.stop()/start()` và `setAudioDeviceSetup()`. Nếu chỉ wire lifecycle ở
MainComponent thì mỗi lần người dùng đổi sample rate/buffer sẽ clear cả hai ring
trong khi detector thread còn chạy — vi phạm precondition của
`LockFreeRingBuffer::clear()` một cách im lặng.

## Cách đã đóng

`DevicePanel` có hai hook `onBeforeRestart` / `onAfterRestart` (pattern giống
`onMessage` sẵn có); MainComponent wire chúng thành
`notchController_.stop(1000)` / `notchController_.start()`. Design §6.5 ghi rõ
thứ tự này.

## Hệ quả cho phiên sau

- Thêm bất kỳ đường restart thiết bị nào mới → phải bọc bằng hai hook đó.
- Đừng "dọn dẹp" hai hook vì thấy "không ai gọi" — chúng được gọi từ lambda
  trong constructor của MainComponent.

## Bài học phụ

`PresetManager::PresetNotch` là **struct tự do**, không lồng trong class
(`PresetManager.h:78`) dù nằm trong header đó — đừng viết qualifier
`PresetManager::PresetNotch` theo tên file. Header tự nó comment điều này.
