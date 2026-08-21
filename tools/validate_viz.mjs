/* Deep-validate Tablor's contract with Schwung's OWN validator
 * (src/shared/param_pages/validate_contract.mjs). Runs when a schwung
 * checkout is available; the build gate's check_config.py covers the
 * hard rules regardless.
 *   node tools/validate_viz.mjs [path-to-schwung-repo]
 */
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
const HERE = path.dirname(fileURLToPath(import.meta.url));
const SCHWUNG = process.argv[2] || path.join(HERE, "../../schwung-overtake");
const mod = path.join(SCHWUNG, "src/shared/param_pages/validate_contract.mjs");
if (!fs.existsSync(mod)) {
    console.log("validate_viz: no schwung checkout at " + SCHWUNG + " — skipped");
    process.exit(0);
}
const { validateContract } = await import(mod);
const hdr = fs.readFileSync(path.join(HERE, "../src/dsp/params.h"), "utf8");
const un = s => s.replace(/\\"/g, '"').replace(/\\\\/g, "\\");
let chainRaw = un(hdr.match(/tb_chain_params_(?:json|fmt) =\n    "(.*)";/)[1]);
/* the wavetable lists are spliced in at runtime — stand in a realistic scan */
for (const sub of ['["Init","Adventure Kid/AKWP 0001"]', '"Init"',
                   '["Init","Adventure Kid/AKWP 0001"]', '"Init"'])
    chainRaw = chainRaw.replace("%s", sub);
const chain = JSON.parse(chainRaw);
const hier  = JSON.parse(un(hdr.match(/tb_ui_hierarchy_json =\n    "(.*)";/)[1]));
const r = validateContract({ id: "tablor", hierarchy: hier, chainParams: chain });
let bad = 0;
for (const f of r.findings) {
    console.log(`[${f.level}] ${f.rule}: ${f.message}`);
    if (f.level === "error" || f.level === "warn") bad++;
}
if (!r.findings.length) console.log("validator: clean");
process.exit(bad ? 1 : 0);
