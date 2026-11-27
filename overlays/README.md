# MMDVM-Display overlays

An **overlay** is a plain-text file that describes what the graphical
MMDVM-Display driver draws on each screen. It is the same idea as a Nextion
layout — you arrange named fields on named pages — but it is a readable text
file you edit by hand, and **no recompiling is needed**: drop a file in a
folder, name it in the config, restart.

This folder contains [`default.ovl`](default.ovl), the full built-in layout,
as a worked example to copy and modify.

---

## Enabling an overlay

In `MMDVM-Display.ini`:

```ini
[Screen]
Device=/dev/fb0
OverlayDir=/etc/mmdvm-display/overlays
Overlay=default
```

* `OverlayDir` — the folder your overlay files live in.
* `Overlay` — the file name inside that folder (e.g. `myskin.ovl`). It may
  also be an absolute path. Leave it blank or set it to `default` to use the
  **built-in** layout and ignore `OverlayDir`.

If the named file is missing or unreadable, the driver logs a warning and
falls back to the built-in default, so a bad path never leaves you blank.

---

## The coordinate system (read this first)

Overlays are **resolution independent**. You do not work in pixels — you work
in a **design space that is always 240 units tall**, and whose **width tracks
the screen's aspect ratio**:

| Screen aspect | Design width |
|---|---|
| 4:3   | 320 |
| 16:10 | 384 |
| 16:9  | ~427 |

The driver scales this to the real framebuffer, so the same overlay looks
right on an 800×480 panel, a 1080p TV, a 4K screen, or anything in between.
It is drawn 1:1 at the detected resolution — never a
stretched or rescaled bitmap — so text stays crisp at any size.

Two rules follow from this:

* **`y` and sizes use the fixed 0–240 scale.** `y=120` is the vertical middle
  of *every* screen; a size of `30` is 30/240 of the height everywhere. `y` is
  the **vertical centre** of the element, so changing a font size (or letting
  text auto-shrink) keeps it centred rather than drifting.
* **`x` and widths: use percentages / anchors for anything proportional,**
  because the *width* in units changes with the screen's shape. Use `%`,
  `center`, `right`, or `fill` so your layout adapts. Plain numbers are still
  perfect for a **fixed left margin or offset** — `x=16` is a 16-unit margin on
  any screen. (Hard-coding `x=300` only looks the same on the aspect you drew
  it for.)

### Value expressions

Any `x`, `y`, `w`, `h` or `fit` value may be:

| Form | Meaning |
|---|---|
| `16` | 16 design units |
| `50%` | 50% of the width (for x/w) or height (for y/h) |
| `center` / `c` | the centre (50%) |
| `right` / `r` | the right edge (for x/w) |
| `bottom` / `b` | the bottom edge (for y/h) |
| `left` / `top` | 0 |
| `fill` | the content width (full width minus a 16-unit margin each side) |
| sums | e.g. `50%-82`, `100%-16`, `50%+58`, `bottom-14` |

---

## Pages

The driver shows one page at a time, chosen by what the hotspot is doing:

| Page | Shown when |
|---|---|
| `idle` | nothing is being received |
| `call` | **any single-stream mode** is active — D-Star, DMR simplex, System Fusion, P25, NXDN, POCSAG, FM. The `banner` adapts its logo and name to the mode. |
| `dmr2` | DMR in **duplex** (both timeslots) |
| `error` | the modem reported an error |
| `lockout` | the modem is locked out |
| `quit` | the host is shutting down |
| `cw` | a CW ident is being sent |

You only need to define the pages you want to change — **any page you omit
falls back to the built-in default**. So an overlay that just restyles `idle`
is three lines long.

A page is declared with `[page name]`; every line after it (until the next
`[page …]`) is an element on that page.

---

## Elements

### `text`

```
text "<template>"  x=.. y=.. size=.. align=.. color=.. fit=..
```

* `"<template>"` — the text to draw. It may contain **`$field`** references
  (see below) mixed with literal characters, e.g. `"TG $tg"`.
* `x`, `y` — position. `y` is the element's vertical centre.
* `size` — `S`, `M`, `L`, or a number in design units (e.g. `42`).
* `align` — `left` (x = left edge, the default), `center` (x = centre), or
  `right` (x = right edge).
* `color` — see Colours.
* `fit` — optional maximum width. If the text is wider, it shrinks to fit, and
  if it is still too long it is truncated with `…`. Great for user-supplied
  values like talker aliases.

### `banner`

Draws the current mode's emblem (a rounded tile + icon) next to its name, with
an accent underline. Placed at the top; takes no coordinates.

### `wordmark`

Draws the MMDVM wordmark + "MULTIMODE DIGITAL VOICE MODEM" strapline. Used on
`idle` and the status pages. Takes no coordinates.

### `rule`

A horizontal line.

```
rule  x=.. y=.. w=.. color=..
```

### `rect`

A filled rectangle (use for dividers, bars, panels).

```
rect  x=.. y=.. w=.. h=.. color=..  r=<corner radius>
```

`r` is optional; with it the corners are rounded and anti-aliased.

### `image`

Draws your own graphic — a club logo, a mode badge, a background. The file is a
**PNG** (so it can carry transparency) kept alongside your overlay:

```
image "logo.png"  x=.. y=.. h=..  [w=..]  align=..
```

* `"logo.png"` — the file, relative to `OverlayDir` (or an absolute path).
* `x`, `align` — horizontal placement, exactly like `text`
  (`left` = left edge, `center`, `right`).
* `y` — the image's **vertical centre**, on the 0–240 scale.
* `h` — the drawn height in design units; the width follows the image's own
  aspect ratio. Give `w` instead to size by width, or give **both** and the
  image is fitted inside that box — keeping its aspect ratio, never stretched.
* Transparent areas let the page show through, so cut-out logos sit cleanly.

The mode **`banner`** and the **`wordmark`** are drawn as vector graphics inside
the binary and need no files; `image` is only for *your own* artwork.

#### Format and sizes

Use **8-bit PNG** — RGBA if you want transparency, RGB otherwise. Only PNG is
supported, by design, to keep the binary small; convert other formats first.

The screen is drawn at its native resolution, so author the file at roughly
the pixels it will occupy on the largest screen it is meant for, or it will
look blocky when enlarged. The design space is 240 units tall and fills the
screen's height, so:

> **pixels ≈ value × screen height ÷ 240** (× 4.5 at 1080p, × 9 at 4K)

For a 1080p screen (double these for 4K):

| Drawn size | Provide at least |
|---|---|
| small badge, `h=40` | ~180 px tall |
| logo, `h=120` | ~540 px tall |
| half the screen, `h=200` | ~900 px tall |

Bigger than that is fine (it scales down smoothly); smaller looks jagged.
Keep files sensible — a logo is usually a few KB to a few tens of KB. Very
large PNGs cost memory and load time for no visible gain.

---

## Fields

Fields are the live values the driver fills in — exactly the data MMDVM-Host
feeds a Nextion. Reference them in a `text` template as `$name`. **If a field
is empty the element is not drawn**, so you can place optional lines freely.

### Always available

| Field | Meaning |
|---|---|
| `$callsign` | the station callsign |
| `$id` | `ID <number>` (DMR ID etc.), already formatted |
| `$ip` | IP address, with interface name if known |
| `$rx` | RX frequency in MHz |
| `$tx` | TX frequency in MHz |
| `$status` | temperature and/or location, combined |

### Single-stream call (`call` page)

| Field | Meaning | Example |
|---|---|---|
| `$src` | source line | `R MW0MWZ` |
| `$dst` | destination line | `TS2 TG23526`, `TG10200`, `CQCQCQ` |
| `$info` | third line | `via REF001 C`, `at CA-XLX427` |
| `$ta` | talker alias (DMR) | `Jonathan G4KLX` |
| `$rssi1` | signal line | `RSSI -84dBm` |
| `$ber1` | error rate | `BER 0.5%` |

### DMR duplex (`dmr2` page)

Per timeslot, `s1…` for TS1 and `s2…` for TS2:

| Field | Meaning |
|---|---|
| `$s1src` / `$s2src` | caller |
| `$s1dst` / `$s2dst` | talkgroup, e.g. `TG91` |
| `$s1ta` / `$s2ta` | talker alias |
| `$s1rssi` / `$s2rssi` | signal, e.g. `-84dBm` |
| `$s1ber` / `$s2ber` | error rate, e.g. `0.5%` |

---

## Colours

| Name | Use |
|---|---|
| `ink` | primary text (near-black) |
| `grey` | labels / secondary text |
| `white` | on dark fills |
| `accent` | the **current mode's** accent colour |
| `green` | talker alias / "good" |
| `red` | errors / warnings |
| `blue` | timeslot labels, highlights |

---

## Sizes

`S`, `M`, `L` are the three built-in text sizes. You can also give a number in
design units for anything in between or larger (e.g. a big `42` callsign).

---

## A minimal example

Restyle only the idle screen; everything else stays default:

```ini
[page idle]
wordmark
rect  x=center-70 y=118 w=140 h=6 color=accent r=3
text  "$callsign" x=center y=132 size=42 align=center color=ink  fit=fill
text  "$id"       x=center y=180 size=S  align=center color=accent
text  "$ip"       x=center y=204 size=S  align=center color=grey  fit=fill
```

Save it as `overlays/bold.ovl`, set `Overlay=bold`, restart. Done — no build.

---

## Tips

* Start from a copy of `default.ovl` and change a few lines at a time.
* Use `center` / `right` / `%` for horizontal placement so it survives a
  change of screen; keep `y` on the 0–240 scale.
* Add `fit=` to any field that carries user text (callsigns, talker aliases,
  POCSAG messages) so it can never overflow its area.
* To add your own logo, drop a `logo.png` next to your overlay and add an
  `image "logo.png" x=center y=.. h=.. align=center` line — size the PNG per the
  table above so it stays sharp.
* Comments start with `#`. Blank lines are ignored.
