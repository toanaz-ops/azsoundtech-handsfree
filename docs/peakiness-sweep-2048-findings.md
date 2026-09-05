# Peakiness sweep — offline baseline at FFT-2048 (LANE T, Task T1)

**Status:** tool shipped, synthetic baseline measured. The threshold decision
itself waits on **T2** (real WAV capture at the rehearsal room + human). Do NOT
retune `PeakinessAnalyzer::kDefaultThreshold` or the measurement table in
`src/dsp/PeakinessAnalyzer.h` from THIS page alone — these are synthetic
signals, not real-room logs.

## The tool

`tools/peakiness_sweep.cpp` (CMake target `PeakinessSweep`). Offline, no audio
device, no GUI. Drives the real `Detector` (2048-point FFT) and the static
`PeakinessAnalyzer::peakinessAt` through their public API only — no `src/`
change. Per analysed block it reports the **maximum** peakiness over every bin
with a full ±5 annulus (the same quantity as `maxPeakiness()` in
`tests/test_peakiness.cpp`), because `analyse()` alone only returns bins ABOVE
the threshold and would hide the whole sub-threshold noise-floor distribution.

Build and run (generator is **VS 18 2026**, not 2022):

```bash
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release --target PeakinessSweep
build/tools/Release/PeakinessSweep.exe > sweep.out
```

Output: per-scenario CSV (`# scenario: <name>` then
`block,freq,peakiness,score,threshold`), followed by the summary table below.
`score` = 0.5 when that block's max crossed 10.0, else 0.0; `threshold` is the
live `analyzer.getThreshold()` (10.0 default), not a hardcoded literal.

## Distribution — synthetic sweep, Detector @ FFT-2048, threshold 10.0

| scenario | blocks | max peakiness | freq@max (Hz) | crossed 10.0? | first-cross block |
|---|---:|---:|---:|:--:|---:|
| noise-floor | 216 | 13.99 | 1640.62 | yes (once) | 173 |
| tone-1khz-in-noise | 216 | 149.20 | 1007.81 | yes | 2 |
| howl-ramp | 216 | 153.28 | 1992.19 | yes | 9 |
| music-like | 216 | 374.52 | 1921.88 | yes | 2 |

Scenarios: `noise-floor` = white noise amp 1.0, seed 12345, no tone.
`tone-1khz-in-noise` = 1 kHz sine amp 1.0 + noise 0.05, seed 12345.
`howl-ramp` = 2 kHz sine ramping 0→1.0 over noise 0.1, seed 777.
`music-like` = the 9 static partials from `tools/snapshot.cpp` `makeHop()` +
noise 0.05, seed 2024. Each scenario walks one continuous
`200·512 + 4·2048 = 110592`-sample stream through 216 overlapping (75 %) hops.

## Findings to carry into the T2 threshold decision

1. **noise-floor crossed 10.0 once (13.99 @ block 173), then fell straight
   back to ~6.2.** The `PeakinessAnalyzer.h` baseline ("worst noise-only bin
   over seeds 1..60 is 7.35") was **60 single-shot** measurements. This sweep
   samples ONE seed's continuous stream through 216 correlated overlapping
   windows — far denser draws on the same random process, so an isolated
   extreme-value excursion is expected, not a broken metric. But it is real:
   under **sustained** noise, 10.0's margin is thinner than the discrete-seed
   figure suggested. The real pipeline's downstream age/hold logic (NotchController,
   deliberately NOT modelled by this offline tool) is what would suppress a
   one-block blip; T2 should re-run with more seeds / longer streams and,
   ideally, exercise that downstream logic before treating 10.0 as comfortable.

2. **music-like crossed immediately and stayed crossed — a proxy artifact, not
   FFT-2048 news.** Nine STATIC sine partials, spaced far outside each other's
   ±5-bin annulus, over a tiny 0.05 noise floor: each reads as an isolated test
   tone under the scale-invariant metric, exactly like `tone-1khz-in-noise`. A
   faithful false-positive-headroom test needs **real recorded programme
   material** (the optional WAV-input path) or partials with amplitude/frequency
   modulation — not static sines. Design the next iteration accordingly.

`tone-1khz-in-noise` landed as expected: 149.20 at 1007.81 Hz, modestly higher
than the old 1024-point rig's ~131, consistent with the tone concentrating into
narrower 2048 bins (as `PeakinessAnalyzer.h` documents). `howl-ramp` climbs
cleanly through the threshold and settles at its own 1992.19 Hz.

## Not done here (T2, needs a rig + human)

Record real WAV (iD14/Wing): noise floor, music, controlled low-volume howl.
Re-run this tool on the captures. If the distribution shifts → propose a new
threshold + update the table in `PeakinessAnalyzer.h` and the
`tests/test_peakiness.cpp` pins; if not → just annotate "re-measured at 2048,
kept 10.0" and drop the caveat in `docs/KY-THUAT-CHONG-HU.md §3.2`.
