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

MOD_SRC = ["None", "LFO 1", "LFO 2", "LFO 3", "Filter EG", "VCA EG",
           "Velocity", "Note", "Mod Wheel", "Aftertouch", "Pitch Bend", "Random"]
MOD_DST = ["None",
           "WT1 Pos", "WT2 Pos", "WT1 Level", "WT2 Level", "WT1 Tune", "WT2 Tune",
           "WT1 Bend", "WT2 Bend", "WT1 Formant", "WT2 Formant", "WT1 Pan", "WT2 Pan",
           "Filter Freq", "Filter Res", "Sub Level", "Noise Level", "Amp",
           "LFO1 Rate", "LFO2 Rate", "LFO3 Rate"]

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
def osc(n):
    return dict(
        table  = wtfile(f"wt{n}_table", f"TBL{n}", f"WT{n} Table"),
        pos    = pot(f"wt{n}_pos",     f"POS{n}",  f"WT{n} Pos", 0),
        level  = pot(f"wt{n}_level",   f"LVL{n}",  f"WT{n} Level", 100 if n == 1 else 0),
        tune   = num(f"wt{n}_tune",    f"TUN{n}",  f"WT{n} Tune", -24, 24, 0),
        fine   = num(f"wt{n}_fine",    f"FIN{n}",  f"WT{n} Fine", -50, 50, 0),
        uni    = num(f"wt{n}_uni",     f"UNI{n}",  f"WT{n} Unison", 1, 4, 1),
        detune = pot(f"wt{n}_detune",  f"DET{n}",  f"WT{n} Detune", 0),
        spread = pot(f"wt{n}_spread",  f"SPR{n}",  f"WT{n} Spread", 0),
        pan    = pot(f"wt{n}_pan",     f"PAN{n}",  f"WT{n} Pan", 64),
        bend   = pot(f"wt{n}_bend",    f"BND{n}",  f"WT{n} Bend", 64),
        formant= pot(f"wt{n}_formant", f"FRM{n}",  f"WT{n} Formant", 64),
        retrig = enum(f"wt{n}_retrig", f"RTR{n}",  f"WT{n} Retrig", RETRIG, 0),
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
    pan   = pot("sub_pan",   "SUBP", "Sub Pan", 64),
)
NOISE = dict(
    level = pot("noise_level", "NOIS", "Noise Level", 0),
    type  = enum("noise_type", "NTYP", "Noise Type", NOISE_TYPES, 0),
    pan   = pot("noise_pan",   "NPAN", "Noise Pan", 64),
)
ROUTE = dict(
    wt1 = enum("rt_wt1",      "FLT1", "WT1 to Filter", ONOFF, 1),
    wt2 = enum("rt_wt2",      "FLT2", "WT2 to Filter", ONOFF, 1),
    subn= enum("rt_subnoise", "FLTS", "Sub/Noise Filt", ONOFF, 1),
)
VCA = dict(
    a = hint(pot("vca_a", "ATK", "VCA Attack", 0),    env="a"),
    d = hint(pot("vca_d", "DEC", "VCA Decay", 64),    env="d"),
    s = hint(pot("vca_s", "SUS", "VCA Sustain", 127), env="s"),
    r = hint(pot("vca_r", "REL", "VCA Release", 24),  env="r"),
    vel    = pot("vca_vel", "VVEL", "VCA Velocity", 100),
    retrig = enum("vca_retrig", "VRTR", "VCA Retrig", ONOFF, 1),
)
FEG = dict(
    a = hint(pot("flt_a", "FATK", "Flt Attack", 0),    env="a"),
    d = hint(pot("flt_d", "FDEC", "Flt Decay", 64),    env="d"),
    s = hint(pot("flt_s", "FSUS", "Flt Sustain", 64),  env="s"),
    r = hint(pot("flt_r", "FREL", "Flt Release", 24),  env="r"),
    retrig = enum("flt_retrig", "FRTR", "Flt Retrig", ONOFF, 1),
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
        retrig= hint(enum(f"lfo{n}_retrig", f"RTG{n}", f"LFO{n} Retrig", RETRIG, 1), lfo="retrig"),
    )
L1, L2, L3 = lfo(1), lfo(2), lfo(3)

def mod(n):
    return [enum(f"m{n}_src", f"SRC{n}", f"Mod{n} Source", MOD_SRC, 0, automatable=False),
            enum(f"m{n}_dst", f"DST{n}", f"Mod{n} Dest",   MOD_DST, 0, automatable=False),
            pot (f"m{n}_amt", f"AMT{n}", f"Mod{n} Amount", 64) | {"automatable": False},
            enum(f"m{n}_on",  f"ON{n}",  f"Mod{n} On",     ONOFF, 0, automatable=False)]
MODS = [mod(n) for n in range(1, 9)]

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
    ("Osc", False, [
        row(O1["table"], O2["table"], O1["pos"], O2["pos"],
            O1["level"], O2["level"], O1["tune"], O2["tune"]),
        row(O1["uni"], O1["detune"], O1["spread"], O1["pan"],
            O2["uni"], O2["detune"], O2["spread"], O2["pan"]),
        row(O1["bend"], O1["formant"], O1["fine"], O1["retrig"],
            O2["bend"], O2["formant"], O2["fine"], O2["retrig"]),
    ]),
    ("Filter", False, [
        row(FILTER["freq"], FILTER["res"], FILTER["env"], FILTER["type"],
            SUB["level"], SUB["wave"], NOISE["level"], NOISE["type"]),
        row(FILTER["key"], FILTER["vel"], SUB["tune"], SUB["pan"],
            NOISE["pan"], ROUTE["wt1"], ROUTE["wt2"], ROUTE["subn"]),
    ]),
    ("Env", False, [
        row(VCA["a"], VCA["d"], VCA["s"], VCA["r"],
            FEG["a"], FEG["d"], FEG["s"], FEG["r"]),
        row(VCA["vel"], VCA["retrig"], FEG["retrig"]),
    ]),
    ("LFO", False, [
        row(*[L1[k] for k in ("shape", "rate", "sync", "beat", "depth", "phase", "offset", "retrig")]),
        row(*[L2[k] for k in ("shape", "rate", "sync", "beat", "depth", "phase", "offset", "retrig")]),
        row(*[L3[k] for k in ("shape", "rate", "sync", "beat", "depth", "phase", "offset", "retrig")]),
    ]),
    ("Mod", False, [
        row(*(MODS[0] + MODS[1])),
        row(*(MODS[2] + MODS[3])),
        row(*(MODS[4] + MODS[5])),
        row(*(MODS[6] + MODS[7])),
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

movy_config = {
    "id": "tablor",
    "name": "Tablor",
    "banks": [
        {"name": name, **({"global": True} if glob else {}),
         "rows": [[movy_slot(s) for s in r] for r in rows]}
        for name, glob, rows in BANKS
    ],
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

# ---------------------------------------------------------------- emit: ui_hierarchy
# The Shadow UI's enterComponentEdit uses the hierarchy editor only when the
# module OFFERS a hierarchy (9W9's plugin comment, shadow_ui.js ~7584) — a
# module with neither ui_hierarchy nor ui_chain.js renders NOTHING. Movy also
# reads it. One level per page; root carries the headline page + navigation.
LEVEL_MAP = [  # (bank name, row index) -> (level id, label)
    ("Osc",    1, "osc_uni",   "Unison"),
    ("Osc",    2, "osc_shape", "Osc Shape"),
    ("Filter", 0, "filter",    "Filter / Sub / Noise"),
    ("Filter", 1, "filter_x",  "Filter Extra"),
    ("Env",    0, "env",       "Envelopes"),
    ("Env",    1, "env_x",     "Env Options"),
    ("LFO",    0, "lfo1",      "LFO 1"),
    ("LFO",    1, "lfo2",      "LFO 2"),
    ("LFO",    2, "lfo3",      "LFO 3"),
    ("Mod",    0, "mod12",     "Mod 1-2"),
    ("Mod",    1, "mod34",     "Mod 3-4"),
    ("Mod",    2, "mod56",     "Mod 5-6"),
    ("Mod",    3, "mod78",     "Mod 7-8"),
    ("Global", 0, "global",    "Global"),
]

def hier_param(p):
    """Editable param entry with FULL metadata — the Shadow UI edits straight
    from the hierarchy, so nothing may rely on a chain_params merge."""
    d = {"key": p["key"], "name": p["full"]}
    if p["type"] == "file":
        d["type"] = "filepath"
        d["root"] = WT_ROOT
        d["start_path"] = WT_ROOT
        d["filter"] = WT_FILTER
        d["live_preview"] = True
    elif p["type"] == "enum":
        d["type"] = "enum"
        d["options"] = p["options"]
    else:
        d["type"] = "int"
        d["min"], d["max"] = p["min"], p["max"]
    return d

def build_hierarchy():
    rows = {(name, i): r for name, _, rws in BANKS for i, r in enumerate(rws)}
    levels = {}

    root_row = rows[("Osc", 0)]
    root_params = [hier_param(s) for s in root_row if s]
    for _, _, lid, label in LEVEL_MAP:
        root_params.append({"level": lid, "label": label})
    levels["root"] = {
        "name": "Tablor",
        "params": root_params,
        "knobs": [s["key"] for s in root_row if s],
    }
    for bank, ri, lid, label in LEVEL_MAP:
        r = rows[(bank, ri)]
        levels[lid] = {
            "name": label,
            "params": [hier_param(s) for s in r if s],
            "knobs": [s["key"] for s in r if s],
        }
    return {"levels": levels}

hierarchy_json = json.dumps(build_hierarchy(), separators=(",", ":"))

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
    "/* ui_hierarchy for the Shadow UI (and Movy's generic path): one level",
    " * per page, root = the headline page + navigation. Static — dynamic",
    " * enum metadata (the wavetable lists) comes from chain_params. */",
    f'static const char *tb_ui_hierarchy_json =\n    "{c_escape(hierarchy_json)}";',
    "",
    "#endif /* TABLOR_PARAMS_H */",
]

# ---------------------------------------------------------------- write
out_module = ROOT / "src" / "module.json"
out_movy   = ROOT / "src" / "movy_config.json"
out_header = ROOT / "src" / "dsp" / "params.h"

out_module.write_text(json.dumps(module_json, indent=2) + "\n")
out_movy.write_text(json.dumps(movy_config, indent=1) + "\n")
out_header.write_text("\n".join(lines) + "\n")

msize = out_module.stat().st_size
print(f"params: {len(PARAMS)}")
print(f"module.json: {msize} bytes  ({'OK' if msize < 8192 else 'OVER 8KB CAP!'})")
print(f"movy_config.json: {out_movy.stat().st_size} bytes")
print(f"params.h: {out_header.stat().st_size} bytes  (chain fmt {len(chain_json)} bytes)")
assert msize < 8192, "module.json exceeds Schwung's 8 KB loader cap"
