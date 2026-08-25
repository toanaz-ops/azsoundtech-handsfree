---
name: juce-component-snapshot
description: Render a JUCE Component to a PNG offscreen with createComponentSnapshot, with no window and no screen capture. Use whenever GUI work needs to be SEEN — reviewing a layout or paint change, checking a component at several sizes, producing images for docs or a handoff, or verifying a visual state that is hard to reproduce live. Also use instead of screen-capture tooling whenever someone asks for a screenshot of this app or of any JUCE component.
---

# Screenshotting a JUCE component without a screen

`juce::Component::createComponentSnapshot()` renders a component and its
children into a `juce::Image`. No desktop peer, no window, no compositor.

Use it instead of capturing the screen. Screen capture of a live app fails for
reasons that have nothing to do with the app: another window drifts in front of
the capture, the desktop's DPI scaling silently rescales the result, the app
has not finished laying out after a programmatic resize, and the running binary
holds a lock on its own `.exe` so the next build cannot link. A snapshot has
none of those failure modes and is byte-identical every run.

## In this repo

The tool already exists. Build it and run it:

```bash
cmake --build build --config Release --target HandsFreeSnapshot
```

```bash
build/tools/Release/HandsFreeSnapshot.exe shots 1280 880 --fast
```

Source: [tools/snapshot.cpp](tools/snapshot.cpp), wired up in
[tools/CMakeLists.txt](tools/CMakeLists.txt). It writes `console-idle.png`
(the state the app opens in) and `console-live.png` (a real published
spectrum with notches placed in it).

`--fast` skips the real-time waits that age the notches. Drop it when the
sodium-to-ice age ramp has to be visible: the full run takes ~23 s because a
notch cools over ~22 s of wall time, and there is no way to fake that without
faking the clock the ramp reads.

Extend that file rather than writing a new one — a second tool that renders
the same component differently is a second thing to keep in step.

## The pattern

```cpp
const juce::ScopedJuceInitialiser_GUI juceInit;   // MessageManager must exist

MyComponent component;
component.setSize (width, height);
component.resized();      // see "no peer" below

const auto image = component.createComponentSnapshot (component.getLocalBounds(),
                                                      true);   // true = children

juce::FileOutputStream stream (file);
juce::PNGImageFormat().writeImageToStream (image, stream);
```

## What bites

**`setSize()` alone does not call `resized()`.** A component with no desktop
peer never gets the callback, so its children keep their old bounds — or, on a
freshly constructed one, no bounds at all, and the snapshot comes out as an
empty rectangle. Call `resized()` explicitly. This is the single most common
reason a snapshot looks blank.

**Pass `true` for the children argument.** Without it you get only the
component's own `paint()`, which on a container is usually just a background.

**Timers do not run.** No message loop is pumped, so anything that refreshes on
a `juce::Timer` — a meter, an analyser, a table polling a model — will render
whatever it was last given, which for a fresh component is nothing. Drive those
refreshes by hand before the snapshot. In this repo that means
`SpectrumView::refreshFromSnapshot()` and
`NotchListPanel::refreshFromSnapshot()`, both public for exactly this reason,
reached through `MainComponent::getSpectrumViewForTest()` /
`getNotchListPanelForTest()`.

**Feed the real pipeline, not a lookalike.** Write audio into the slot's tap
ring and call `NotchController::runOnce()` so a genuine snapshot gets
published, the way `tests/test_spectrumview.cpp` does. A hand-written
`SnapshotBuffer` proves the paint code draws something; a real one proves the
pipeline behind it works too.

**MSVC needs a bigger stack.** `MainComponent` builds eight `NotchController`s
and one scorer history is ~512 kB, which overflows the default 1 MB. Any target
that constructs it needs `target_link_options(... "/STACK:8388608")` — already
set for both the test target and the snapshot tool.

**No device is opened.** `MainComponent`'s constructor deliberately does not
call `startAudio()`, which is what lets the whole object exist on a machine
with no interface attached. The device combos therefore render empty and the
masthead reads `Stopped`. That is correct output, not a broken snapshot.

## Reporting the result

**Standing instruction from the owner (2026-08-25): every task that changes
what the UI looks like ends by sending the rendered screenshot.** Not a
description of the change, not a list of files touched, and not a green test
count -- the picture.

Send `console-live.png`, plus `console-idle.png` when the empty state changed.

## Reading the result

Read the PNG back and actually look at it. The point of the tool is to catch
what a green build cannot: text that ellipsises inside its cell, a control that
overlaps its neighbour, a marker heavy enough to bury the data behind it, or a
lamp that is not lit when the state says it should be. Every one of those has
been caught this way in this repo, and none of them fails a test.
