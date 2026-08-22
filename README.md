# Tablor

**A super-simple 2-oscillator wavetable synth for [Schwung](https://github.com/charlesvestal/schwung) on the Ableton Move.**

Tablor is built around one idea: switching wavetables should be the fastest,
most fun thing on the box. The first two knobs on the first page ARE the two
wavetables — turn one and the sound changes under your fingers. Everything
else stays out of the way.

![Main page: both wavetables on knobs 1-2, position, level and tune](docs/img/move-main.png)

13 pages, 95 controls, no menus inside menus. The graphics are Schwung's own
— declared, not drawn by us — so the filter curve and both envelopes are the
real values:

| Filter | Envelopes |
|---|---|
| ![Filter page](docs/img/move-filter.png) | ![Env page](docs/img/move-env.png) |

## Features

- **2 wavetable oscillators** with position morphing, bend (phase distortion),
  formant shift, and up to 4-voice unison with detune + stereo spread
- **Sub oscillator** (sine / triangle / saw / square / pulse) and **noise**
  (white / pink)
- **Multimode filter** — LP / HP / BP / Notch, 12 or 24 dB/oct, with its own
  ADSR and key/velocity tracking
- **Two envelopes** (amp + filter), **2 LFOs** (tempo-syncable, 8 shapes),
  **4-slot mod matrix**
- **8 user macros** — map any knob to anything, on the device; assignments
  save with the patch and can be automated
- **8 voices** polyphonic, or mono with glide/legato
- **8 factory presets** plus **unlimited user presets** — plain `.tblr` files
  in `UserLibrary/Tablor Presets/`, name-as-filename, copy/share them freely;
  save + name entirely from the hardware
- A seeded library of **115 wavetables**
- **Web panel** — full control surface in the browser at
  `move.local:7700/remote-ui`

## Wavetables

All tables live in one folder on the Move:

```
/data/UserData/UserLibrary/Wavetables/
```

Drop wavetables there with Move Manager or the Schwung manager's file browser
— **Serum, Vital and Ableton exports all work**. `.wav` frame size is read
from the Serum `clm` chunk when present and inferred otherwise; Ableton's
FLAC-compressed `.wtNNNN` names its frame size in the extension. Factory packs
are seeded on first run:

- **Adventure Kid** (AKWF/AKWP) — 65 tables, public domain, by Kristoffer
  Ekstrand
- **Neu KatalYst** — 50 tables, free ("use them in all your synths")

On the device, the two wavetables live on knobs 1 and 2 of the main page.
Turn a knob to step through the library and hear each table as you land on
it; hold and click to open a full-screen scrolling picker when you want to
jump somewhere specific.

The file browser and the pack filter are in the **web panel** rather than on
the Move. Both used a control a knob cannot turn, so each one cost a whole
page on a four-cell screen — and the browser's graphic was Schwung's stock
`sample` shape, which is a fixed drawing that never reads the file, so it
told you nothing about the table you had loaded. The parameters still exist
and still save with the patch; they simply have no cell on the hardware.

## The interface

Tablor speaks the platform's native language — no custom editor code. Three
ways to play it, all driven by one generated parameter source so they never
disagree:

1. **The stock Shadow UI** (Schwung ≥ 0.12): knob pages with *declared* `viz`
   graphics — envelopes, filter curve, LFO shapes, faders, switches — plus the
   native full-screen preset browser, the scrolling wavetable picker on the
   WT knobs, and the on-screen keyboard for preset renames. 19 graphics
   declared, validated by Schwung's own `validate_contract` (zero guesses).

   Page order: `[Presets] › Main › Unison › Shape › Filter › Env › Env+ ›
   LFO 1 › LFO 2 › Mod 1-2 › Mod 3-4 › Global › User › U.Map`.
2. **[Movy](https://github.com/DimaDake/schwung-movy)**: the same pages as
   Movy banks, with live filter curves, dual envelope graphics and LFO
   previews.
3. **Web panel**: every control in the browser, live in both directions, laid
   out the way the plugin this came from lays out — and it draws the wavetable
   you actually have loaded.

   ![Web panel](docs/img/web-panel.png)

   The waterfall is the real file: the module publishes a digest of the loaded
   table, and the panel also reads the file itself over the manager's file API
   and decodes it, so changing a table — from the browser **or** from the pot
   on the Move — redraws it. The ADSR and LFO curves are computed from the live
   parameter values. Wavetable and pack pickers sit in each oscillator's title
   bar.

## Building

Cross-compiles for the Move (aarch64, glibc 2.35) in Docker:

```bash
./scripts/docker-build.sh        # inside ubuntu:22.04 with the aarch64 toolchain
```

`scripts/build.sh` is a convenience wrapper that builds on a remote host and
is specific to the author's setup. The build runs the native DSP test suite
and a config contract check; a red test fails the build. `tools/gen_params.py`
is the single source of truth for the whole parameter surface — module.json,
Movy config, the on-device editor and the web panel are all generated from it.

## Notes for Schwung upstream

[`docs/UPSTREAM-NOTES.md`](docs/UPSTREAM-NOTES.md) collects what this port ran
into in the 0.12 parameter pages — the SPI-callback cost of a dynamic
`chain_params`, the `sample` graphic being a placeholder rather than the
file's waveform, and two page-ordering traps — each with the file and line it
comes from, and a suggestion.

A separate hunt, for a pad note-off the host sometimes never delivers, is
written up with its traces in
[charlesvestal/schwung#249](https://github.com/charlesvestal/schwung/issues/249).

## Credits & licenses

Tablor is a from-scratch Schwung port of **[Wavetable](https://github.com/FigBug/Wavetable)**
by **Roland Rabien (FigBug)** — the sound engine (wavetable oscillators,
band-limited mip tables, analog-style envelopes, LFOs, filter behavior) is
ported from Wavetable and its DSP library **[gin](https://github.com/FigBug/Gin)**,
both BSD-3-Clause. The JUCE dependency was removed entirely; the DSP was
re-implemented as plain C++ against Schwung's plugin API, validated against
the original's behavior with numeric golden tests.

| Component | Origin | License |
|---|---|---|
| Synth architecture & DSP | [Wavetable](https://github.com/FigBug/Wavetable) / [gin](https://github.com/FigBug/Gin) by Roland Rabien | BSD-3-Clause |
| Filter core (SVF, MZTi) | AudioFilter by Michael Massberg (via gin) | BSD-3-Clause |
| FLAC decoding | [dr_flac](https://github.com/mackron/dr_libs) by David Reid | Public domain / MIT-0 |
| Pink noise | Voss-McCartney impl. by Thomas Merchant (via gin) | MIT |
| Adventure Kid wavetables | Kristoffer Ekstrand | Public domain |
| Neu KatalYst wavetables | Neu KatalYst | Free to use |
| Host framework | [Schwung](https://github.com/charlesvestal/schwung) by Charles Vestal | — |
| Knob-page UI concepts | [Movy](https://github.com/DimaDake/schwung-movy) by megadake | MIT |
| Schwung port | [athousanddetails](https://github.com/athousanddetails) | BSD-3-Clause |

This module is **not** affiliated with Ableton. "Move" is a trademark of
Ableton AG.

## License

BSD-3-Clause — see [LICENSE](LICENSE). Factory wavetable packs keep their own
licenses as noted above.
