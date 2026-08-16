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
check(len(banks) == 15, f"expected 15 banks, got {len(banks)}")
for b in banks:
    # Movy: a config bank is EXACTLY one page (bankGroups is per-bank but
    # indexed per-page; multi-row banks shift every later page label)
    check(len(b.get("rows", [])) == 1,
          f"bank {b.get('name')}: must be single-row (Movy bank == page)")

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
            elif s["type"] == "file":
                check(isinstance(s.get("fileRoot"), str) and s["fileRoot"].startswith("/"),
                      f"{k}: file slot needs absolute fileRoot")
                check(isinstance(s.get("fileFilter"), list) and s["fileFilter"],
                      f"{k}: file slot needs fileFilter")
            else:
                check("min" in s and "max" in s and s["min"] < s["max"],
                      f"{k}: needs min < max")
            if "env" in s:    check(s["env"] in VALID_ENV, f"{k}: env hint {s['env']}")
            if "lfo" in s:    check(s["lfo"] in VALID_LFO, f"{k}: lfo hint {s['lfo']}")
            if "filter" in s: check(s["filter"] in VALID_FILTER, f"{k}: filter hint {s['filter']}")

# ---- chain_params: static JSON, parse + cross-check -------------------
hdr = (ROOT / "src/dsp/params.h").read_text()
m = re.search(r'tb_chain_params_json =\n    "(.*)";', hdr)
check(m is not None, "chain_params found in params.h")
if m:
    raw = m.group(1).replace('\\"', '"').replace("\\\\", "\\")
    try:
        chain = json.loads(raw)
    except json.JSONDecodeError as e:
        fails.append(f"chain_params is not valid JSON: {e}")
        chain = []
    chain_keys = {p["key"] for p in chain}

    # every movy_config key must exist in chain_params, and vice versa
    for k in seen_keys:
        check(k in chain_keys, f"movy_config key {k} missing from chain_params")
    for k in chain_keys:
        check(k in seen_keys, f"chain_params key {k} not on any movy page")

    # types agree (movy 'file' pairs with schwung 'filepath')
    chain_types = {p["key"]: p["type"] for p in chain}
    pair = {"file": "filepath", "int": "int", "float": "float", "enum": "enum"}
    for b in banks:
        for r in b["rows"]:
            for s in r:
                if s and s["key"] in chain_types:
                    check(pair.get(s["type"]) == chain_types[s["key"]],
                          f"{s['key']}: movy type {s['type']} != chain {chain_types[s['key']]}")
    # filepath entries carry the browser config
    for p in chain:
        if p["type"] == "filepath":
            check(p.get("root", "").startswith("/") and p.get("filter") and
                  p.get("live_preview") is True,
                  f"{p['key']}: filepath needs root/filter/live_preview")

# ---- ui_pages.json (ui_chain.js data): parses, full key coverage ------
pages_file = ROOT / "src/ui_pages.json"
check(pages_file.exists(), "ui_pages.json generated")
if pages_file.exists():
    up = json.loads(pages_file.read_text())
    pages = up.get("pages", [])
    check(len(pages) == 15, f"15 pages expected, got {len(pages)}")
    page_keys = set()
    for pg in pages:
        check(isinstance(pg.get("name"), str) and pg.get("sec"), "page has name+sec")
        check(len(pg.get("slots", [])) == 8, f"page {pg.get('name')}: 8 slots")
        for s in pg["slots"]:
            if s is None:
                continue
            check(s.get("k") and s.get("n") and s.get("full") and s.get("t"),
                  f"slot needs k/n/full/t in {pg['name']}")
            if s["t"] == "enum":
                check(isinstance(s.get("options"), list) and s["options"],
                      f"{s['k']}: enum slot needs options")
            elif s["t"] == "int":
                check(s.get("min") is not None and s.get("max") is not None,
                      f"{s['k']}: int slot needs min/max")
            page_keys.add(s["k"])
    for k in seen_keys:
        check(k in page_keys, f"movy key {k} missing from ui_pages")
    for k in page_keys:
        check(k in seen_keys, f"ui_pages key {k} not a real param")

# ---- ui_hierarchy must NOT be emitted (ui_chain.js would be ignored) --
check("tb_ui_hierarchy_json" not in hdr,
      "ui_hierarchy absent from params.h (the 9W9 rule)")
check((ROOT / "src/ui_chain.js").exists(), "ui_chain.js present")

# ---- module.json under the 8 KB loader cap ----------------------------
sz = (ROOT / "src/module.json").stat().st_size
check(sz < 8192, f"module.json {sz} bytes exceeds 8 KB cap")

if fails:
    print("CONFIG CONTRACT FAILED:")
    for f in fails:
        print("  -", f)
    sys.exit(1)
print(f"config contract OK: {len(banks)} banks, {len(seen_keys)} keys, "
      f"chain fmt splices to valid JSON, module.json {sz} B")
