#!/usr/bin/env python3
"""Tablor parameter generator — the single source of truth.

Emits, from one table:
  src/module.json        (< 8 KB — Schwung's loader cap; holds NO chain_params)
  src/movy_config.json   (the six Movy banks with render hints)
  src/dsp/params.h       (param table + chain_params JSON served via get_param)

Every continuous control is a 0..127 pot (the ER-99 doctrine): the DSP maps
each pot to its musical range internally; the screen never shows ms or Hz.
Musical integers (semitones, cents, voices) keep their real ranges.

Run: python3 tools/gen_params.py     (from the repo root)
"""
import json, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
VERSION = "0.1.0"

# ---------------------------------------------------------------- enums
FILTER_TYPES = ["LP 12", "LP 24", "HP 12", "HP 24", "BP 12", "BP 24", "Notch 12", "Notch 24"]
SUB_WAVES    = ["Sine", "Triangle", "Saw", "Square", "Pulse 25", "Pulse 12"]
NOISE_TYPES  = ["White", "Pink"]
LFO_SHAPES   = ["Sine", "Triangle", "Saw Up", "Saw Down", "Square", "Square+", "S&H", "Noise"]
LFO_BEATS    = ["1/32", "1/16T", "1/16", "1/8T", "1/16.", "1/8", "1/4T", "1/8.",
                "1/4", "1/2T", "1/4.", "1/2", "1/1T", "1/2.", "1/1", "2/1"]
ONOFF        = ["Off", "On"]
RETRIG       = ["Free", "Retrig"]
VOICE_MODES  = ["Poly", "Mono"]
GLIDE_MODES  = ["Off", "Glissando", "Portamento"]

MOD_SRC = ["None", "LFO 1", "LFO 2", "Filter EG", "VCA EG",
           "Velocity", "Note", "Mod Wheel", "Aftertouch", "Pitch Bend", "Random"]
MOD_DST = ["None",
           "WT1 Pos", "WT2 Pos", "WT1 Level", "WT2 Level", "WT1 Tune", "WT2 Tune",
           "WT1 Bend", "WT2 Bend", "WT1 Formant", "WT2 Formant", "WT1 Pan", "WT2 Pan",
           "Filter Freq", "Filter Res", "Sub Level", "Noise Level", "Amp",
           "LFO1 Rate", "LFO2 Rate"]

# Wavetable selection is a FILEPATH param: Schwung's Shadow UI opens its real
# file browser (folders + live preview while the cursor moves), Movy opens its
# own via type 'file'. No giant flat enum, no dynamic options machinery.
WT_ROOT   = "/data/UserData/UserLibrary/Wavetables"
WT_FILTER = [".wav", ".wt2048", ".wt1024", ".wt512", ".wt256"]

def wtfile(key, short, full):
    return dict(key=key, short=short, full=full, type="file", default="")

# ---------------------------------------------------------------- helpers
def pot(key, short, full, default=64):
    """0..127 pot; DSP maps to the musical range."""
    return dict(key=key, short=short, full=full, type="int", min=0, max=127, default=default)

def num(key, short, full, lo, hi, default=0):
    """A musical integer shown as-is (semitones, cents, voices…)."""
    return dict(key=key, short=short, full=full, type="int", min=lo, max=hi, default=default)

def enum(key, short, full, options, default=0, **kw):
    d = dict(key=key, short=short, full=full, type="enum", options=options, default=default)
    d.update(kw)
    return d

def hint(p, **kw):
    """Attach movy-only render hints (env/lfo/filter/knobAcceleration…)."""
    q = dict(p); q["_movy"] = kw; return q

# ---------------------------------------------------------------- the surface
# Trimmed 2026-08-16 per Gus: no fine, no retrig anywhere, no sub/noise pan,
# no filter routing switches, 4 mod slots, 2 LFOs. Behaviors hardcode to the
# old defaults in the DSP.
def osc(n):
    return dict(
        table  = wtfile(f"wt{n}_table", f"TBL{n}", f"WT{n} Table"),
        pos    = pot(f"wt{n}_pos",     f"POS{n}",  f"WT{n} Pos", 0),
        level  = pot(f"wt{n}_level",   f"LVL{n}",  f"WT{n} Level", 100 if n == 1 else 0),
        tune   = num(f"wt{n}_tune",    f"TUN{n}",  f"WT{n} Tune", -24, 24, 0),
        uni    = num(f"wt{n}_uni",     f"UNI{n}",  f"WT{n} Unison", 1, 4, 1),
        detune = pot(f"wt{n}_detune",  f"DET{n}",  f"WT{n} Detune", 0),
        spread = pot(f"wt{n}_spread",  f"SPR{n}",  f"WT{n} Spread", 0),
        pan    = pot(f"wt{n}_pan",     f"PAN{n}",  f"WT{n} Pan", 64),
        bend   = pot(f"wt{n}_bend",    f"BND{n}",  f"WT{n} Bend", 64),
        formant= pot(f"wt{n}_formant", f"FRM{n}",  f"WT{n} Formant", 64),
    )

O1, O2 = osc(1), osc(2)

FILTER = dict(
    freq = hint(pot("flt_freq", "FREQ", "Filter Freq", 127), filter="cutoff"),
    res  = hint(pot("flt_res",  "RES",  "Filter Res", 0),    filter="resonance"),
    env  = pot("flt_env",  "FENV", "Filter EG Amt", 64),
    type = hint(enum("flt_type", "TYPE", "Filter Type", FILTER_TYPES, 1), filter="mode"),
    key  = pot("flt_key",  "KEYT", "Key Track", 0),
    vel  = pot("flt_vel",  "VELT", "Vel Track", 0),
)
SUB = dict(
    level = pot("sub_level", "SUB",  "Sub Level", 0),
    wave  = enum("sub_wave", "SUBW", "Sub Wave", SUB_WAVES, 0),
    tune  = num("sub_tune",  "SUBT", "Sub Tune", -24, 24, -12),
)
NOISE = dict(
    level = pot("noise_level", "NOIS", "Noise Level", 0),
    type  = enum("noise_type", "NTYP", "Noise Type", NOISE_TYPES, 0),
)
VCA = dict(
    # NO env hints: Movy's roleOf() gives every hinted stage qualifier "" —
    # two hinted foursomes then collapse into ONE group and the loser draws
    # as lone ramps. Unhinted, Movy parses the LABELS ("VCA Attack" -> Amp
    # group, "Flt Attack" -> Filter group) and stacks two named envelopes.
    a = pot("vca_a", "ATK", "VCA Attack", 0),
    d = pot("vca_d", "DEC", "VCA Decay", 64),
    s = pot("vca_s", "SUS", "VCA Sustain", 127),
    r = pot("vca_r", "REL", "VCA Release", 24),
    vel = pot("vca_vel", "VVEL", "VCA Velocity", 100),
)
FEG = dict(
    a = pot("flt_a", "FATK", "Flt Attack", 0),
    d = pot("flt_d", "FDEC", "Flt Decay", 64),
    s = pot("flt_s", "FSUS", "Flt Sustain", 64),
    r = pot("flt_r", "FREL", "Flt Release", 24),
)
def lfo(n):
    return dict(
        shape = hint(enum(f"lfo{n}_shape", f"SHP{n}", f"LFO{n} Shape", LFO_SHAPES, 0), lfo="shape"),
        rate  = hint(pot(f"lfo{n}_rate",  f"RAT{n}", f"LFO{n} Rate", 64), lfo="rate"),
        sync  = enum(f"lfo{n}_sync",  f"SYN{n}", f"LFO{n} Sync", ONOFF, 0),
        beat  = enum(f"lfo{n}_beat",  f"BEA{n}", f"LFO{n} Beat", LFO_BEATS, 8),
        depth = hint(pot(f"lfo{n}_depth", f"DEP{n}", f"LFO{n} Depth", 127), lfo="depth"),
        phase = hint(pot(f"lfo{n}_phase", f"PHA{n}", f"LFO{n} Phase", 0), lfo="phase"),
        offset= pot(f"lfo{n}_offset", f"OFF{n}", f"LFO{n} Offset", 64),
    )
L1, L2 = lfo(1), lfo(2)

def mod(n):
    return [enum(f"m{n}_src", f"SRC{n}", f"Mod{n} Source", MOD_SRC, 0, automatable=False),
            enum(f"m{n}_dst", f"DST{n}", f"Mod{n} Dest",   MOD_DST, 0, automatable=False),
            pot (f"m{n}_amt", f"AMT{n}", f"Mod{n} Amount", 64) | {"automatable": False},
            enum(f"m{n}_on",  f"ON{n}",  f"Mod{n} On",     ONOFF, 0, automatable=False)]
MODS = [mod(n) for n in range(1, 5)]

# Preset names come from the factory bank + 8 fixed user slots, so the picker
# is an ordinary static enum that works in Movy, Tablor's editor AND the web
# panel. Save writes the current sound into the chosen user slot.
PRESET_NAMES = []
for _line in (ROOT / "src" / "presets" / "factory.tbl").read_text().splitlines():
    if _line and not _line.startswith("#") and "|" in _line:
        PRESET_NAMES.append(_line.split("|", 1)[0])
USER_SLOTS = [f"User {i}" for i in range(1, 9)]
PRESET_OPTIONS = PRESET_NAMES + USER_SLOTS

# Save has no destination knob: it overwrites the CURRENT User slot, or the
# first empty one when a factory preset is selected (refuses when all 8 are
# full and a factory sound is up — pick a User slot to overwrite instead).
PRESET_PAGE = dict(
    # render:'preset' -> Movy's live preset widget (shows the CURRENT name,
    # including user renames, via preset_name/preset_names)
    preset = hint(enum("preset", "PRST", "Preset", PRESET_OPTIONS, 0,
                       automatable=False), render="preset"),
    save   = enum("save_preset", "SAVE", "Save Preset", ONOFF, 0,
                  automatable=False, behavior="trigger"),
)

GLOBAL = dict(
    mode   = enum("voice_mode", "MODE", "Voice Mode", VOICE_MODES, 0),
    voices = num("voices", "VOIC", "Voices", 1, 8, 8),
    glide  = pot("glide", "GLID", "Glide", 0),
    gmode  = enum("glide_mode", "GMOD", "Glide Mode", GLIDE_MODES, 0),
    legato = enum("legato", "LEGA", "Legato", ONOFF, 0),
    pb     = num("pb_range", "BEND", "PB Range", 0, 24, 2),
    vol    = pot("volume", "VOL", "Volume", 100),
)

# ---------------------------------------------------------------- pages
def row(*slots):
    out = list(slots) + [None] * (8 - len(slots))
    assert len(out) == 8, f"row has {len(slots)} slots"
    return out

BANKS = [
    ("Preset", True, [
        row(PRESET_PAGE["preset"], PRESET_PAGE["save"]),
    ]),
    ("Osc", False, [
        row(O1["table"], O2["table"], O1["pos"], O2["pos"],
            O1["level"], O2["level"], O1["tune"], O2["tune"]),
        row(O1["uni"], O1["detune"], O1["spread"], O1["pan"],
            O2["uni"], O2["detune"], O2["spread"], O2["pan"]),
        row(O1["bend"], O1["formant"], O2["bend"], O2["formant"],
            SUB["tune"]),
    ]),
    ("Filter", False, [
        row(FILTER["freq"], FILTER["res"], FILTER["env"], FILTER["type"],
            SUB["level"], SUB["wave"], NOISE["level"], NOISE["type"]),
    ]),
    # Both envelopes on ONE page: a Movy envelope graphic spans one 4-cell
    # line and a page has two lines, so VCA (line 1) + Filter (line 2) draw
    # as two stacked ADSR graphics (the manual's env_dual). Each foursome
    # must own its own line — split across lines it degrades to lone ramps.
    ("Env", False, [
        row(VCA["a"], VCA["d"], VCA["s"], VCA["r"],
            FEG["a"], FEG["d"], FEG["s"], FEG["r"]),
        row(VCA["vel"], FILTER["key"], FILTER["vel"]),
    ]),
    ("LFO", False, [
        row(*[L1[k] for k in ("shape", "rate", "sync", "beat", "depth", "phase", "offset")]),
        row(*[L2[k] for k in ("shape", "rate", "sync", "beat", "depth", "phase", "offset")]),
    ]),
    ("Mod", False, [
        row(*(MODS[0] + MODS[1])),
        row(*(MODS[2] + MODS[3])),
    ]),
    ("Global", True, [
        row(GLOBAL["mode"], GLOBAL["voices"], GLOBAL["glide"], GLOBAL["gmode"],
            GLOBAL["legato"], GLOBAL["pb"], GLOBAL["vol"]),
    ]),
]

# ---------------------------------------------------------------- collect
def all_params():
    seen, out = set(), []
    for _, _, rows in BANKS:
        for r in rows:
            for s in r:
                if s and s["key"] not in seen:
                    seen.add(s["key"]); out.append(s)
    return out

# ---------------------------------------------------------------- user macros
# 8 assignable macros as REAL DSP params, so the User page exists in Movy AND
# in ui_chain, assignments live inside patches, and a Movy LFO can drive a
# macro. u{i} is a 0..127 pot; u{i}_target picks any parameter by name; the
# DSP writes through (and back-syncs the pot when the target changes).
BASE_PARAMS = all_params()
NOT_TARGETS = {"preset", "save_preset"}
USER_TARGETS = ["None"] + [p["full"] for p in BASE_PARAMS
                           if p["type"] != "file" and p["key"] not in NOT_TARGETS]

USER_POTS, USER_SELS = [], []
for i in range(1, 9):
    USER_POTS.append(pot(f"u{i}", f"USR{i}", f"Macro {i}", 0))
    USER_SELS.append(enum(f"u{i}_target", f"TGT{i}", f"Macro {i} Target",
                          USER_TARGETS, 0, automatable=False))

BANKS.append(("User", False, [row(*USER_POTS)]))
BANKS.append(("UMap", True,  [row(*USER_SELS)]))

PARAMS = all_params()

# ---------------------------------------------------------------- emit: movy_config
def movy_slot(p):
    if p is None:
        return None
    s = {"key": p["key"], "short": p["short"], "full": p["full"], "type": p["type"]}
    if p["type"] == "file":
        s["fileRoot"] = WT_ROOT
        s["fileFilter"] = WT_FILTER
    elif p["type"] == "enum":
        s["options"] = p["options"]
    else:
        s["min"], s["max"] = p["min"], p["max"]
    if p.get("automatable") is False:
        s["automatable"] = False
    if p.get("behavior"):
        s["behavior"] = p["behavior"]
    for k, v in p.get("_movy", {}).items():
        s[k] = v
    return s

# Movy constraint (config-pages.ts: "Each config bank is exactly one page"):
# bankGroups is one entry PER BANK but indexed PER PAGE, so a multi-row bank
# shifts every following page label. One bank per page, named like ui_chain.
MOVY_PAGE_NAMES = {
    ("Preset", 0): "Preset",
    ("Osc", 0): "Osc",    ("Osc", 1): "Unison",  ("Osc", 2): "Shape",
    ("Filter", 0): "Filter",
    ("Env", 0): "Env", ("Env", 1): "Env+",
    ("LFO", 0): "LFO 1",  ("LFO", 1): "LFO 2",
    ("Mod", 0): "Mod 1-2", ("Mod", 1): "Mod 3-4",
    ("Global", 0): "Global",
    ("User", 0): "User",  ("UMap", 0): "U.Map",
}

movy_banks = []
for name, glob, rows in BANKS:
    for ri, r in enumerate(rows):
        movy_banks.append({
            "name": MOVY_PAGE_NAMES[(name, ri)],
            **({"global": True} if glob else {}),
            "rows": [[movy_slot(s) for s in r]],
        })

movy_config = {
    "id": "tablor",
    "name": "Tablor",
    "banks": movy_banks,
}

# ---------------------------------------------------------------- emit: chain_params
def chain_param(p):
    if p["type"] == "file":
        return {"key": p["key"], "name": p["full"], "type": "filepath",
                "root": WT_ROOT, "start_path": WT_ROOT,
                "filter": WT_FILTER, "live_preview": True, "default": ""}
    d = {"key": p["key"], "name": p["full"], "type": p["type"]}
    if p["type"] == "enum":
        d["options"] = p["options"]
        d["default"] = p["options"][p["default"]]
    else:
        d["min"], d["max"], d["default"] = p["min"], p["max"], p["default"]
    return d

chain = [chain_param(p) for p in PARAMS]
chain_json = json.dumps(chain, separators=(",", ":"))
assert "%" not in chain_json, "chain_params must be printf-safe (served verbatim)"

# ---------------------------------------------------------------- emit: ui_pages.json
# Data for Tablor's OWN Shadow UI (ui_chain.js) — the 9W9 treatment: jog flips
# pages, Shift+jog jumps sections, 8 encoders edit the visible page.
# NOTE: the module must NOT serve ui_hierarchy — the Shadow UI's hierarchy
# editor takes precedence over ui_chain.js whenever a module offers one.
PAGE_MAP = [  # (bank name, row index, page title). Section = bank name.
    ("Preset", 0, "PRESET"),
    ("Osc",    0, "OSC"),
    ("Osc",    1, "UNISON"),
    ("Osc",    2, "SHAPE"),
    ("Filter", 0, "FILTER"),
    ("Env",    0, "ENV"),
    ("Env",    1, "ENV+"),
    ("LFO",    0, "LFO 1"),
    ("LFO",    1, "LFO 2"),
    ("Mod",    0, "MOD 1-2"),
    ("Mod",    1, "MOD 3-4"),
    ("Global", 0, "GLOBAL"),
    ("User",   0, "USER"),
    ("UMap",   0, "U.MAP"),
]

def page_slot(p):
    if p is None:
        return None
    d = {"k": p["key"], "n": p["short"], "full": p["full"], "t": p["type"]}
    if p["type"] == "enum":
        d["options"] = p["options"]
    elif p["type"] == "int":
        d["min"], d["max"] = p["min"], p["max"]
    if p.get("behavior"):
        d["b"] = p["behavior"]
    return d

def build_pages():
    rows = {(name, i): r for name, _, rws in BANKS for i, r in enumerate(rws)}
    pages = []
    for bank, ri, title in PAGE_MAP:
        pages.append({
            "name": title,
            "sec": bank,
            "slots": [page_slot(s) for s in rows[(bank, ri)]],
        })
    return {"pages": pages}

ui_pages = build_pages()

# ---------------------------------------------------------------- emit: module.json
module_json = {
    "id": "tablor",
    "name": "Tablor",
    "abbrev": "TBL",
    "version": VERSION,
    "description": "2-oscillator wavetable synth - 8-voice poly, sub, noise, "
                   "multimode filter, 3 LFOs, mod matrix. Serum/Vital/Ableton "
                   "wavetables via Move Manager.",
    "author": "athousanddetails; after Wavetable (Roland Rabien / FigBug, BSD-3)",
    "dsp": "dsp.so",
    "api_version": 2,
    "capabilities": {
        "audio_out": True,
        "audio_in": False,
        "midi_in": True,
        "midi_out": False,
        "chainable": True,
        "component_type": "sound_generator",
    },
}

# ---------------------------------------------------------------- emit: params.h
def c_escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')

lines = [
    "/* Generated by tools/gen_params.py — DO NOT EDIT.",
    " * The parameter surface: IDs, ranges, defaults, and the chain_params",
    " * JSON template (wavetable option lists are spliced in at runtime). */",
    "#ifndef TABLOR_PARAMS_H",
    "#define TABLOR_PARAMS_H",
    "",
    f'#define TB_VERSION "{VERSION}"',
    f"#define TB_PARAM_COUNT {len(PARAMS)}",
    "",
    "typedef enum { TB_INT = 0, TB_ENUM = 1, TB_PATH = 2 } tb_param_type_t;",
    "",
    "typedef struct {",
    "    const char *key;",
    "    tb_param_type_t type;",
    "    float min, max, def;              /* enums: def is the option index */",
    "    int n_options;                    /* 0 for non-enums                */",
    "    const char *const *options;       /* NULL for non-enums             */",
    "} tb_param_t;",
    "",
    "/* TB_PATH params (wavetable files) store a string, not a float — the",
    " * plugin keeps them in a side table indexed by tb_path_slot(). */",
    "#define TB_PATH_COUNT 2",
    "",
]
enum_names = []
for i, p in enumerate(PARAMS):
    enum_names.append(f"    TB_P_{p['key'].upper()} = {i},")
lines += ["enum {"] + enum_names + ["};", ""]

# Option string tables (shared where identical)
opt_tables, opt_index = [], {}
for p in PARAMS:
    if p["type"] == "enum" and p["options"] is not None:
        key = tuple(p["options"])
        if key not in opt_index:
            name = f"tb_opts_{len(opt_tables)}"
            opt_index[key] = name
            opts = ", ".join(f'"{c_escape(o)}"' for o in p["options"])
            opt_tables.append(f"static const char *const {name}[] = {{ {opts} }};")
lines += opt_tables + [""]

rows = []
for p in PARAMS:
    if p["type"] == "file":
        rows.append(f'    {{ "{p["key"]}", TB_PATH, 0, 0, 0, 0, 0 }},')
    elif p["type"] == "enum":
        tbl = opt_index[tuple(p["options"])]
        rows.append(f'    {{ "{p["key"]}", TB_ENUM, 0, {len(p["options"]) - 1}, '
                    f'{p["default"]}, {len(p["options"])}, {tbl} }},')
    else:
        rows.append(f'    {{ "{p["key"]}", TB_INT, {p["min"]}, {p["max"]}, {p["default"]}, 0, 0 }},')
lines += ["static const tb_param_t tb_params[TB_PARAM_COUNT] = {"] + rows + ["};", ""]

lines += [
    "/* chain_params JSON — fully static, served verbatim (wavetable selection",
    " * is a filepath param; the file browser lists the folder live). */",
    f'static const char *tb_chain_params_json =\n    "{c_escape(chain_json)}";',
    "",
    "/* NOTE: ui_hierarchy is deliberately NOT served — the Shadow UI's",
    " * hierarchy editor would take precedence over the module's own",
    " * ui_chain.js (the 9W9 rule). Movy uses movy_config + chain_params. */",
    "",
]

# User-macro wiring: pot index, selector index, and option->param-index map.
target_map = ["-1"]
for p in BASE_PARAMS:
    if p["type"] != "file" and p["key"] not in NOT_TARGETS:
        target_map.append(f'TB_P_{p["key"].upper()}')
lines += [
    "/* User macros: u{i} pots write through to their selected target. */",
    "#define TB_USER_MACROS 8",
    "static const int tb_user_pots[TB_USER_MACROS] = {",
    "    " + ", ".join(f"TB_P_U{i}" for i in range(1, 9)) + " };",
    "static const int tb_user_sels[TB_USER_MACROS] = {",
    "    " + ", ".join(f"TB_P_U{i}_TARGET" for i in range(1, 9)) + " };",
    f"/* selector option index -> param index (0 = None) */",
    f"static const int tb_user_target_map[{len(target_map)}] = {{",
    "    " + ",\n    ".join(", ".join(target_map[i:i+6])
                            for i in range(0, len(target_map), 6)) + " };",
    "",
    "#endif /* TABLOR_PARAMS_H */",
]

# ---------------------------------------------------------------- write
out_module = ROOT / "src" / "module.json"
out_movy   = ROOT / "src" / "movy_config.json"
out_header = ROOT / "src" / "dsp" / "params.h"

out_pages = ROOT / "src" / "ui_pages.json"

out_module.write_text(json.dumps(module_json, indent=2) + "\n")
out_movy.write_text(json.dumps(movy_config, indent=1) + "\n")
out_header.write_text("\n".join(lines) + "\n")
out_pages.write_text(json.dumps(ui_pages, separators=(",", ":")) + "\n")

msize = out_module.stat().st_size
print(f"params: {len(PARAMS)}")
print(f"module.json: {msize} bytes  ({'OK' if msize < 8192 else 'OVER 8KB CAP!'})")
print(f"movy_config.json: {out_movy.stat().st_size} bytes")
print(f"params.h: {out_header.stat().st_size} bytes  (chain fmt {len(chain_json)} bytes)")
assert msize < 8192, "module.json exceeds Schwung's 8 KB loader cap"
