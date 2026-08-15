#!/usr/bin/env python3
"""Contract check: the generated movy_config.json + chain_params template
against Movy's ModuleConfig shape (reference/movy/src/types/param.ts).

Runs in the build container after gen_params.py — a violation fails the build.
"""
import json, pathlib, re, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
fails = []

def check(cond, msg):
    if not cond:
        fails.append(msg)

# ---- movy_config.json shape -------------------------------------------
cfg = json.loads((ROOT / "src/movy_config.json").read_text())
check(cfg.get("id") == "tablor" and cfg.get("name"), "id/name present")
banks = cfg.get("banks", [])
check(len(banks) == 6, f"expected 6 banks, got {len(banks)}")

VALID_TYPES  = {"float", "int", "enum", "file"}
VALID_ENV    = {"a", "d", "s", "r"}
VALID_LFO    = {"shape", "phase", "mode", "retrig", "rate", "depth", "deform"}
VALID_FILTER = {"cutoff", "resonance", "mode", "slope"}

seen_keys = set()
for b in banks:
    check(isinstance(b.get("name"), str) and b["name"], "bank has a name")
    for r in b.get("rows", []):
        check(len(r) == 8, f"bank {b['name']}: row must have exactly 8 slots, got {len(r)}")
        for s in r:
            if s is None:
                continue
            k = s.get("key")
            check(isinstance(k, str) and k, f"slot missing key in bank {b['name']}")
            check(k not in seen_keys, f"duplicate key {k}")
            seen_keys.add(k)
            check(s.get("type") in VALID_TYPES, f"{k}: bad type {s.get('type')}")
            check(isinstance(s.get("short"), str) and len(s["short"]) <= 5,
                  f"{k}: short label must be <=5 chars ('{s.get('short')}')")
            check(isinstance(s.get("full"), str) and s["full"], f"{k}: full label")
            if s["type"] == "enum":
                check(isinstance(s.get("options"), list) and s["options"],
                      f"{k}: enum needs options")
            else:
                check("min" in s and "max" in s and s["min"] < s["max"],
                      f"{k}: needs min < max")
            if "env" in s:    check(s["env"] in VALID_ENV, f"{k}: env hint {s['env']}")
            if "lfo" in s:    check(s["lfo"] in VALID_LFO, f"{k}: lfo hint {s['lfo']}")
            if "filter" in s: check(s["filter"] in VALID_FILTER, f"{k}: filter hint {s['filter']}")

# ---- chain_params template: splice + parse ----------------------------
hdr = (ROOT / "src/dsp/params.h").read_text()
m = re.search(r'tb_chain_params_fmt =\n    "(.*)";', hdr)
check(m is not None, "chain fmt found in params.h")
if m:
    fmt = m.group(1).replace('\\"', '"').replace("\\\\", "\\")
    # splice a realistic table list, exactly like the plugin does
    opts = json.dumps(["Init", "Adventure Kid/AKWP 0001", 'Odd "quoted" name'])
    filled = fmt
    for sub in (opts, '"Init"', opts, '"Init"'):
        filled = filled.replace("%s", sub, 1)
    try:
        chain = json.loads(filled)
    except json.JSONDecodeError as e:
        fails.append(f"spliced chain_params is not valid JSON: {e}")
        chain = []
    chain_keys = {p["key"] for p in chain}

    # every movy_config key must exist in chain_params, and vice versa
    for k in seen_keys:
        check(k in chain_keys, f"movy_config key {k} missing from chain_params")
    for k in chain_keys:
        check(k in seen_keys, f"chain_params key {k} not on any movy page")

    # types agree
    chain_types = {p["key"]: p["type"] for p in chain}
    for b in banks:
        for r in b["rows"]:
            for s in r:
                if s and s["key"] in chain_types:
                    check(s["type"] == chain_types[s["key"]],
                          f"{s['key']}: movy type {s['type']} != chain {chain_types[s['key']]}")

# ---- module.json under the 8 KB loader cap ----------------------------
sz = (ROOT / "src/module.json").stat().st_size
check(sz < 8192, f"module.json {sz} bytes exceeds 8 KB cap")

if fails:
    print("CONFIG CONTRACT FAILED:")
    for f in fails:
        print("  -", f)
    sys.exit(1)
print(f"config contract OK: 6 banks, {len(seen_keys)} keys, "
      f"chain fmt splices to valid JSON, module.json {sz} B")
