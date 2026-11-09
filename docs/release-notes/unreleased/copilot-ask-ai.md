# Next alpha - COPILOT "ask the AI" in Geek mode (lane L2b)

**Audio path touched:** no. Nothing in `src/dsp/`, `AudioEngine` or `NotchController`
changed.

**Expected level change:** none (0 dB). The feature reads the session log and talks to an
AI provider; it never writes to the audio chain.

## What the tester sees

- Geek mode, lower left, COPILOT panel: a new `CÀI AI` button next to `PHÂN TÍCH`, and at the
  bottom an ask row: a text box ("Hỏi về buổi diễn này…") and a `HỎI` button (Enter also sends).
- `CÀI AI` opens an overlay over the whole left column (notch list + COPILOT) until `ĐÓNG`.
  The mode rail on the right (BYPASS) stays reachable. Fields: provider (Claude or
  OpenAI-compatible), model, address (OpenAI-compatible only), API key with `LƯU KHOÁ` /
  `XOÁ KHOÁ`, and a consent switch.
- Pressing `HỎI` sends the question and the session log (no audio) to the chosen provider
  and shows the answer in the panel, with a footer `model · N in / M out`. Up to 2 minutes;
  the button becomes `HUỶ` while waiting.
- An answer that advises raising gain, volume, the depth ceiling or switching feedback
  protection off is replaced by a fixed message: the status line says "Câu trả lời bị chặn."
  and the panel body reads "Câu trả lời của mô hình đã bị chặn vì nó khuyên một việc có thể
  gây hú. Hãy hỏi lại, hoặc đổi mô hình."
- If you press `HỎI` right after `HUỶ` while the cancelled run is still ending, the panel says
  "Đang bận, thử lại sau một giây."; try again a moment later.
- Without consent, or (Claude) without a saved key, `HỎI` only says "Chưa cài AI. Bấm CÀI AI.".

## How it works

The ask runs in the same separate `HandsFreeCopilot.exe` process as `PHÂN TÍCH`, one run at
a time. The key is encrypted with Windows DPAPI for the current Windows user and stored in
`%APPDATA%\AZSoundtech\HandsFree\copilot-credentials.json`; it is never in `settings.json`,
a log, the command line or the environment. Provider, model, address and consent are in
`settings.json`.

## Please check

1. Open `CÀI AI`: does everything fit, can you reach BYPASS while it is open, does `ĐÓNG` work?
2. Save a key, close, reopen: does it say the key is saved? `XOÁ KHOÁ` removes it?
3. Ask a plain question about a session with a few notches. Is the answer readable and the
   footer sensible? Does `HUỶ` stop a slow one?
4. Does your antivirus or firewall complain when you ask?

## Known limits

- **No real provider call has been tested yet** (no key was available): latency, token
  counts and provider-specific errors are unverified.
- DPAPI protects against other Windows users and offline disk reads; it does NOT stop
  malware running as the same Windows user.
- Known limit: buffers the app owns are wiped after DPAPI, but copies of the key held briefly
  inside `juce::String` / `juce::TextEditor` are freed without being zeroed.
- The safety filter is a phrase filter, not a guarantee; it errs towards blocking.
- Answers are not streamed: the panel shows nothing until the whole answer arrives.
