# Test profile (DEVCAVE_TEST_PROFILE) — 2026-11-10

Why: a real-app UI test of another JUCE app answered "don't save" on quit and deleted the owner's autosaves. JUCE `getSpecialLocation` uses CSIDL, so overriding APPDATA does not redirect it; the app must honour the override itself.

How: `src/app/DataRoot.h/.cpp`. If `DEVCAVE_TEST_PROFILE` is an absolute dir, roaming root = `<profile>/AppData/AZSoundtech/HandsFree`, documents = `<profile>/Documents`. Relative or blank value = refuse to start (never fall back to real data). Routed: settings.json, license.key, presets/, logs/ (copilot credentials follow the settings dir). Test mode adds ` [TEST PROFILE]` to the window title, logs the profile path, and uses a per-profile InterProcessLock instead of JUCE's single-instance lock (so a test run does not hand off to the owner's real app).

Rule: launch the real app for UI tests only via `D:/DEV CAVE EP3/shared/harness/guards/test_app_launcher.ps1 -App handsfree`; never answer don't-save/discard on a window you did not start.
