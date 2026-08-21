# Notes for Schwung upstream

Findings from porting **Tablor** (a 2-oscillator wavetable synth) to the
Schwung 0.12 native parameter pages. Everything here was found on hardware
(Move, Schwung `main` @ `426c1965`), not read off a screen — file/line
references are to `charlesvestal/schwung` at that commit.

Nothing here is a blocker for us; Tablor ships as-is. These are the three
places where a module author is likely to fall into a hole, roughly in
order of how much time each one cost.

---

## 1. `get_param` is serviced from the SPI callback — and nothing warns you

`src/modules/chain/dsp/chain_internal.h` says it, in a comment on
`chain_params_answer_is_useful`:

> *"Pure scan over a caller-owned buffer — no allocation, no I/O — because
> these routes are serviced from the SPI callback."*

That constraint applies to **every** `get_param`/`set_param` a module
implements, but it is documented only there — `docs/MODULES.md` describes
`chain_params` as "define in module.json **or dynamically via
`get_param`**", with no hint that the dynamic answer is computed inside the
audio callback.

**What it did to us.** Tablor briefly served a *dynamic* `chain_params`: the
wavetable knob was an enum whose options were the live folder scan (116
filenames, ~24 KB of JSON), formatted per request. Symptoms, in this order:

1. `param_giveup ... error=4` storms in `debug.log` — **96 in ~2.5 minutes**,
   on unrelated keys (`synth:name`, `fx1:display_name`, `knob_1_name`).
2. The enum knob silently refused to move, because a read had failed and
   (correctly, per `97f1ea9e`) an enum *"never plans from a failed read"*.

So an expensive answer to one key degrades the whole param bus, and the
visible symptom lands on an unrelated control. Caching the payload and
serving a copy took the giveups from **96 → 11**.

**Suggestions, cheapest first**
- Say it in `docs/MODULES.md` where dynamic `chain_params` is introduced:
  *serving this runs in the audio callback; precompute it.*
- `validate.mjs` can't see runtime cost, but the host could: log once when a
  single `get_param` exceeds some fraction of the frame budget. That one line
  would have pointed straight at the module.
- Optional: let a module publish `chain_params` **to** the host at load
  (push, not pull), so a dynamic list is never fetched on the RT thread.

## 2. `drawSample` never looks at the sample

`src/shared/param_pages/viz_draw.mjs`, `drawSample()`:

```js
const v = Math.abs(Math.sin(t * Math.PI)) * (0.55 + 0.35 * Math.sin(t * 23));
```

The `sample` viz kind draws a **fixed synthetic squiggle** — identical for
every file, in every module. Only the position marker carries real data.
`docs/MODULES.md` describes the kind as *"A `filepath`; a companion
`wav_position` param marks playback position on the waveform"*, which reads
like the waveform is the file's.

For a sampler the placeholder is defensible. For a **wavetable** synth the
picture is the whole point: the user's mental model is "I turned this and the
shape changed", and the one graphic that should carry that says nothing.

**Suggestions**
- Short term: a doc line saying the shape is decorative.
- Better: have the host read peaks from the selected file (Movy already does
  this — `MANUAL.md` "Sample waveform": *"the file is read a little at a time
  in the background… WAV (8/16/24-bit and float) and AIFF are both read"*).
  Movy's implementation is the reference, and its incremental read is the
  right shape for the RT constraints.
- If a module could supply the peaks itself (a small `viz_data`-style key
  read off-thread), it would also cover formats the host cannot decode —
  ours are FLAC `.wt2048`.

## 3. Two page-layout traps that cost a debugging session each

Both were found by running `planPages` from
`src/shared/param_pages/page_plan.mjs` locally against our own hierarchy —
which turned out to be the single most useful debugging tool available, and
is not mentioned anywhere as something a module author can do.

**a. A level that declares a preset browser *and* knobs emits two pages.**
`Presets` then `Presets - 2`. Jogging off the browser lands on another
preset page, which reads as being stuck. Not wrong — but worth a sentence
next to the `list_param` docs.

**b. Page order follows nav order, so a preset browser declared as its own
nav level lands *between* Main and the sections** (`Main > [Presets] >
Unison > …`). One jog off Main drops back into the preset list. Declaring
`list_param` on **root** (the obxd pattern) gives `[Presets] > Main >
Unison > …`, which is what people expect. The comment in `page_plan.mjs`
("a level is routinely both the Main knob page and the preset browser")
already knows this; the docs don't say it.

**Suggestion:** document `node tools/param-pages/…` usage for *module*
authors — "print the pages your hierarchy will produce" is a 5-line script
against `planPages`, and `validateContract` is already perfect for CI. We
run both in our build gate now and they have caught real regressions.

---

## Minor

- **Label shortening** (`render_page.mjs shortenLabel`): a two-word name
  loses its *head* word first, so `"WT1 Table"` renders `WTab` — the index,
  the part that distinguishes the two oscillators, is what gets eaten. The
  docstring says the head "reduces towards its initial" deliberately, and
  numbers survive whole *inside* the head; here the whole head is the index.
  Not a bug, but `WT1` / `WT2` was the only way to keep them apart.
- **"Synth v2 loaded: X (0 params)"** is normal and slightly alarming:
  `parse_chain_params` only ever reads `module.json`, so every module that
  serves `chain_params` dynamically logs 0. Worth a word in the log line.

---

*Tablor: <https://github.com/athousanddetails/schwung-tablor> — module
sources, plus `tools/check_config.py` and `tools/validate_viz.mjs`, which
run `validateContract` in the build.*

---

## 4. Stale MIDI_IN events are dispatched twice, which manufactures stuck notes

**Where:** `src/host/shadow_midi.c`, `event_dedup_check_and_record()` (line 121)

**Symptom on hardware:** a pad note keeps sounding after release and only stops
when another note steals its voice; the pad's LED stays lit too. Rare, and it
takes playing pads while turning an encoder to provoke.

**What the traces show.** Tablor logged every raw MIDI message at its own
`on_midi` boundary, and Schwung's `chain_midi_trace_on` logged the same stream
one layer up. For the stuck pitch both agree:

    chain IN  note-ON  91 29:  14      -> synth ON:  14
    chain IN  note-OFF 81 29:  13      -> synth OFF: 13

The chain host forwarded exactly what it received, so nothing below it lost the
event. One note-on had no note-off — and the unpaired note-on is a DUPLICATE,
not a real press:

    214378.54  midi_in  81 26 00     note-off for 38
    214378.59  midi_in  91 26 7f     note-on  for 38   <- 50 us later
    213757.25  midi_in  91 26 7f     ...the original press, 621 ms earlier

Two events 50 microseconds apart are one scan of MIDI_IN, not two fingers.

**Root cause.** `shadow_dispatch_direct_external_midi()` walks MIDI_IN every
frame and relies on the 8-byte dedup key to avoid re-dispatching an event that
is still sitting in the buffer. That entry expires after
`EVENT_DEDUP_MAX_AGE_TICKS` (16), and the tick advances once per SPI frame
(`schwung_shim.c:5401`), so the window is ~46 ms. But how long an event stays
in MIDI_IN is Move's firmware's business, not ours -- measured at 621 ms here.
Once the entry expires the still-present event is dispatched a second time. A
re-dispatched note-ON is a voice that no note-off will ever reach.

**Fix (one line):** refresh the entry's tick whenever it matches, so the window
starts when the event LEAVES MIDI_IN rather than when it was first seen. This
cannot suppress a different event: the key carries the XMOS per-event
timestamp, so a genuine retrigger of the same pitch has a different key and
still dispatches. Ageing only reclaims ring slots.

    if (memcmp(ring[i].key, key, 8) == 0) {
        ring[i].tick = g_dispatched_ext_tick;   /* still present: keep alive */
        return 1;
    }

Prepared as a branch: `fix/midi-in-stale-event-redispatch`.

**Tablor's backstop.** Until the host fix lands, Tablor releases a note whose
pad pressure reached zero and then went silent for 1.5 s
(`Engine::kPadGoneBlocks`). It is armed only by real poly aftertouch, so a MIDI
keyboard is unaffected. 1.5 s rather than the 400 ms tried first: Move sends a
stray zero mid-press, measured arriving 800 ms before the real note-off, and a
short window cut notes that were still held.
