# Spec — the console's visual reference ("Sodium Rack")

The approved design study is the artifact published 2026-08-25. This file is
its **transcription into numbers**, so a future change can be checked against a
measurement instead of a memory of a picture.

Where the shipped app deliberately departs from the study, the departure is
stated here with its reason. Everything else is a defect if it does not match.

---

## 1. Type

Three faces, all embedded from `assets/fonts/` — **not** taken from whatever
the machine happens to ship. This is the difference between "the design" and
"the design on my PC": Bahnschrift and Cascadia Mono were a reasonable stand-in
while the study was HTML, but they are not the faces the study was approved in.

| Role | Face | Weights | Used for |
|---|---|---|---|
| Legend | **Saira Condensed** | 600, 700 | Every uppercase panel legend, caption, switch and chip |
| Body | **IBM Plex Sans** | 400 | Prose: empty states, dialog copy |
| Numeric | **IBM Plex Mono** | 400, 500 | Every number: Hz, dB, Q, countdown, axis ticks, slot ids |

All OFL; licences sit beside the files as `OFL-*.txt`.

Sizes and tracking, from the study's CSS:

| Element | Face | Size | Weight | Tracking |
|---|---|---|---|---|
| Section caption (`.cap`) | Legend | 11 | 600 | .20em |
| Column caption (`th`) | Legend | 10.5 | 600 | .16em |
| Switch legend (`.sw .lbl`) | Legend | 16 | 700 | .15em |
| Switch hint (`.sw .hint`) | Numeric | 10 | 400 | .10em |
| Brand name (`.mark .name`) | Legend | 14 | 700 | .20em |
| Segment button (`.seg button`) | **Numeric** | 11 | 400 | none, **not uppercase** |
| Table cell (`td`) | Numeric | 12.5 | 400 | none |
| Countdown value (`.countdown .v`) | Numeric | 26 | 500 | none |
| Rig readout (`.rig`) | Numeric | 12 | 400 | none |

**The segment buttons are the trap.** They are mono, 11 px, sentence case, and
untracked — `Line`, `1/1 oct`, `Avg off`. Setting them in the tracked uppercase
legend face makes the toolbar shout over the analyser it labels.

## 2. Colour

| Token | Hex | Role |
|---|---|---|
| `shell` / `background` | `#0A0B0D` | window canvas **and the plot's own ground** |
| `panel` | `#131519` | masthead, transport, toolbar, floor |
| `raise` | `#1B1E24` | raised or selected cell |
| `well` | `#0C0E11` | recessed field |
| `groove` / `border` | `#2B2F37` | hairline |
| `grid` | `#1D2128` | analyser grid — brighter than the groove, on purpose |
| `legend` / `text` | `#E8EAED` | primary |
| `dim` | `#868D98` | secondary |
| `faded` | `#5C636E` | tertiary: units, row numbers, axis ticks |
| `sodium` / `accent` | `#FF9F1C` | the accent, and a notch the instant it fires |
| `trace` | `#FFB552` | the signal line — a lit sodium, not the accent |
| `sig` / `ok` | `#6EE7A0` | PROTECTING, and nothing else |
| `hot` / `danger` | `#FF5A4E` | destructive, clipping |
| `ice` / `settled` | `#5FC9FF` | a notch that has held |
| `cooling` | `#C9D1D9` | the ramp's midpoint (see §6) |
| `peak` | `#DDE6F0` | peak-hold ceiling trace |

## 3. The analyser toolbar — order matters

The study's `.stagebar`, left to right, 32 px tall, 12 px side padding, 8 px gaps:

```
ANALYSER   [ Line │ 1/1 oct │ 1/3 oct ]   [ Avg off │ 1 s │ 3 s ]   ←spacer→   RING RISK  [ low ]
```

Three things the first implementation got wrong, all visible:

1. **The display groups sit on the LEFT**, immediately after the section
   caption. Ring risk sits at the far RIGHT. The first cut mirrored this.
2. **There are no `BAND` / `AVG` legends.** The first button of the averaging
   group carries its own label — `Avg off`. A separate legend duplicates it.
3. **Each group is ONE outlined box**, `1px groove`, `3px` radius, with the
   buttons butted together and separated by a `1px groove` divider
   (`.seg button + button { border-left }`). They are **not** free-floating
   chips with gaps. A selected segment fills with `raise` and its text goes to
   `legend`; unselected text is `dim`. No per-button border at all.

Peak hold is not in the study — it is a real feature the study omitted. It is
drawn as a fourth element after the averaging group, in the same joined style,
and it carries `peak` when on, so the control and the trace it produces are
visibly the same thing.

## 4. The signal

- Plot ground: `shell`. **Not** a distinct well. The study's canvas paints
  `#0A0B0D`, the same as the page.
- Grid: `grid`, 1 px, at 100 Hz / 1 kHz / 10 kHz and 0 / −30 / −60 / −90 dB.
- Area fill: vertical gradient in `accent` — `0.30` at the plot top, `0.09` at
  55 % down, `0.0` at the floor.
- Trace: `trace` `#FFB552`, stroke width `1.4`.
- Axis labels: numeric face, 11 px, `faded`.

## 5. The floor — column split

```
.col.notches { flex: 1.25 }      .col.rig { flex: 1 }
```

So **ACTIVE NOTCHES takes the wider column**, ~55.6 % against ~44.4 %. The
first implementation inverted this and gave the notch table 38 %.

**Departure:** the shipped rig column carries a full 8-lane routing table that
the study did not have (the study showed two summarised "Slot 1 / Slot 2"
rows). That table has a hard minimum width. The shipped split is therefore
**0.52 / 0.48**, not 0.556 / 0.444 — as close to the study as the real control
allows without the table scrolling sideways at the minimum window size.

## 6. The one idea (not in the study, added in build)

A notch is sodium the instant it fires and ice once it has held, over ~22 s,
**through `cooling` at the midpoint**. A straight RGB interpolation from
sodium to ice passes through an olive khaki and reads as a fault.

Every marker is drawn over a 3 px dark keyline so it separates from the trace
at every point on that ramp — the trace is sodium too.

## 7. Deliberate departures, collected

| Study | Shipped | Why |
|---|---|---|
| Ring risk shows `low` | shows `N/A`, hollow | No data source yet. See `spec-ring-risk.md`. A risk chip that reads reassuring while unwired is worse than none. |
| No peak hold | Peak hold present | A real feature; the study simply did not cover it. |
| Two summarised slot rows | Full 8-lane routing table | The real control the product needs. Drives the 0.52 split above. |
| No slot selector | `MONITOR` tabs in the ACTIVE NOTCHES caption | One detector per slot has always existed; nothing could reach past slot 0. Placed on the notch panel, not the masthead, because that is where a user looks for it. |
| Fixed 210 px floor | Floor capped at 55 % of the space under the transport, and the window grows when a routing row is added | The study had no scrolling table. |
