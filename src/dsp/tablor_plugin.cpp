/* Tablor — 2-osc wavetable synth for Ableton Move (Schwung module).
 *
 * plugin_api_v2. Parameter surface + version-tagged state round-trip +
 * the phase 3 engine: 8 voices, poly/mono, glide, mod matrix.
 *
 * Realtime rules (docs/REALTIME_SAFETY.md): render_block never allocates,
 * never touches the filesystem, never logs.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <new>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "../host/plugin_api_v1.h"
#include "params.h"
#include "engine.h"
#include "wt/scanner.h"
#include "wt/loader.h"

static const host_api_v1_t *g_host = nullptr;

/* ------------------------------------------------------------------ */
/* Instance                                                            */
/* ------------------------------------------------------------------ */

struct tablor_instance {
    tb::Engine engine;
    tb::WtScanner scanner;                  /* used for first-run seeding only */
    tb::WtLoader loader { engine };
    char  module_dir[512] = {};

    /* wavetable selection = absolute file paths ("" = built-in Init) */
    char  wt_path[TB_PATH_COUNT][512] = {};

    /* factory presets (presets/factory.tbl): name + state blob each */
    struct preset { std::string name, blob; };
    std::vector<preset> presets;
    int preset_index = 0;

    /* state blob scratch */
    char  state_buf[8 * 1024];

    float *params() { return engine.pots; }
};

static constexpr int kUserPresetSlots = 8;

static void user_preset_path(tablor_instance *inst, int slot, char *out, size_t cap)
{
    snprintf(out, cap, "%s/presets/user/u%d.tbl", inst->module_dir, slot + 1);
}

static void load_presets(tablor_instance *inst)
{
    char path[600];
    snprintf(path, sizeof path, "%s/presets/factory.tbl", inst->module_dir);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[4096];
        while (fgets(line, sizeof line, f)) {
            if (line[0] == '#' || line[0] == '\n') continue;
            char *bar = strchr(line, '|');
            if (!bar) continue;
            *bar = 0;
            char *blob = bar + 1;
            blob[strcspn(blob, "\r\n")] = 0;
            inst->presets.push_back({ line, blob });
        }
        fclose(f);
    }

    /* 8 fixed user slots after the factory bank (old-hardware style).
     * File format matches factory: "Name|TBLR1;..." — the name is the
     * user's own. An empty slot applies defaults (= Init). */
    snprintf(path, sizeof path, "%s/presets/user", inst->module_dir);
    ::mkdir(path, 0755);
    for (int i = 0; i < kUserPresetSlots; i++) {
        char defname[16];
        snprintf(defname, sizeof defname, "User %d", i + 1);
        std::string name = defname, blob = "TBLR1;";
        user_preset_path(inst, i, path, sizeof path);
        FILE *uf = fopen(path, "r");
        if (uf) {
            char line[8192];
            if (fgets(line, sizeof line, uf)) {
                line[strcspn(line, "\r\n")] = 0;
                char *bar = strchr(line, '|');
                const char *b = line;
                if (bar) { *bar = 0; if (line[0]) name = line; b = bar + 1; }
                if (!strncmp(b, "TBLR1;", 6)) blob = b;
            }
            fclose(uf);
        }
        inst->presets.push_back({ name, blob });
    }
}

static void write_user_preset(tablor_instance *inst, int slot,
                              const std::string &name, const char *blob)
{
    char path[600];
    user_preset_path(inst, slot, path, sizeof path);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s|%s\n", name.c_str(), blob);
        fclose(f);
    }
}

static int param_index(const char *key)
{
    for (int i = 0; i < TB_PARAM_COUNT; i++)
        if (!strcmp(tb_params[i].key, key))
            return i;
    return -1;
}

/* Which path slot a TB_PATH param uses: wt1_table -> 0, wt2_table -> 1. */
static int tb_path_slot(int param_idx)
{
    return param_idx == TB_P_WT1_TABLE ? 0 : 1;
}

/* Store a path and post the background load. "" or "Init" = built-in. */
static void set_table_path(tablor_instance *inst, int osc, const char *path)
{
    if (!strcmp(path, "Init")) path = "";
    snprintf(inst->wt_path[osc], sizeof inst->wt_path[osc], "%s", path);

    tb::WtEntry e;
    e.path = path;
    if (const char *dot = strrchr(path, '.'); dot && !strncasecmp(dot, ".wt", 3)) {
        int fs = atoi(dot + 3);
        if (fs >= 256 && fs <= 4096 && (fs & (fs - 1)) == 0)
            e.flacFrameSize = fs;
    }
    inst->loader.requestLoad(osc, e);
}

static float clamp_param(const tb_param_t *p, float v)
{
    if (v < p->min) v = p->min;
    if (v > p->max) v = p->max;
    return v;
}

/* Serialize the current sound into inst->state_buf (TBLR1 blob). */
static void build_state_blob(tablor_instance *inst)
{
    int o = snprintf(inst->state_buf, sizeof inst->state_buf, "TBLR1;");
    for (int i = 0; i < TB_PARAM_COUNT; i++) {
        if (tb_params[i].type == TB_PATH) {
            const char *path = inst->wt_path[tb_path_slot(i)];
            if (path[0])
                o += snprintf(inst->state_buf + o,
                              sizeof inst->state_buf - (size_t) o,
                              "%s=%s;", tb_params[i].key, path);
            continue;
        }
        /* preset machinery is not part of a sound */
        if (!strcmp(tb_params[i].key, "preset") ||
            !strncmp(tb_params[i].key, "save_", 5))
            continue;
        float v = inst->params()[i];
        if (v == tb_params[i].def) continue;            /* defaults are implicit */
        o += snprintf(inst->state_buf + o, sizeof inst->state_buf - (size_t) o,
                      "%s=%g;", tb_params[i].key, (double) v);
        if (o >= (int) sizeof inst->state_buf - 64) break;
    }
}

static void reset_to_defaults(tablor_instance *inst)
{
    for (int i = 0; i < TB_PARAM_COUNT; i++)
        if (tb_params[i].type != TB_PATH)
            inst->params()[i] = tb_params[i].def;
    set_table_path(inst, 0, "");
    set_table_path(inst, 1, "");
    inst->engine.syncModSlots();
}

/* Version-tagged blob: "TBLR1;key=val;key=val;…". Anything without the
 * tag is from another life — ignore it entirely rather than half-apply
 * it (the ER-99 total-silence lesson). */
static void apply_state_blob(tablor_instance *inst, const char *val)
{
    if (strncmp(val, "TBLR1;", 6) != 0) return;
    const char *p = val + 6;
    char kbuf[64], vbuf[512];
    while (*p) {
        const char *eq = strchr(p, '=');
        if (!eq) break;
        const char *semi = strchr(eq, ';');
        if (!semi) semi = eq + strlen(eq);
        size_t kl = (size_t) (eq - p), vl = (size_t) (semi - eq - 1);
        if (kl < sizeof kbuf && vl < sizeof vbuf) {
            memcpy(kbuf, p, kl); kbuf[kl] = 0;
            memcpy(vbuf, eq + 1, vl); vbuf[vl] = 0;
            int idx = param_index(kbuf);
            if (idx >= 0) {
                if (tb_params[idx].type == TB_PATH)
                    set_table_path(inst, tb_path_slot(idx), vbuf);
                else
                    inst->params()[idx] = clamp_param(&tb_params[idx],
                                                      strtof(vbuf, nullptr));
            }
        }
        p = (*semi) ? semi + 1 : semi;
    }
    inst->engine.syncModSlots();
}

/* ------------------------------------------------------------------ */
/* User macros: u{i} pots write through to their selected target;      */
/* picking a new target back-syncs the pot so nothing jumps.           */
/* ------------------------------------------------------------------ */

static int user_macro_target(tablor_instance *inst, int i)
{
    int opt = (int) inst->params()[tb_user_sels[i]];
    if (opt < 0 || opt >= (int) (sizeof tb_user_target_map / sizeof *tb_user_target_map))
        return -1;
    return tb_user_target_map[opt];
}

static void user_macro_apply(tablor_instance *inst, int i)
{
    int t = user_macro_target(inst, i);
    if (t < 0) return;
    const tb_param_t *p = &tb_params[t];
    float v01 = inst->params()[tb_user_pots[i]] / 127.0f;
    float span = p->max - p->min;
    inst->params()[t] = p->min + (float) (int) (v01 * span + 0.5f);
    if (p->key[0] == 'm' && p->key[1] >= '1' && p->key[1] <= '8')
        inst->engine.syncModSlots();
}

static void user_macro_backsync(tablor_instance *inst, int i)
{
    int t = user_macro_target(inst, i);
    if (t < 0) return;
    const tb_param_t *p = &tb_params[t];
    float span = p->max - p->min;
    float v01 = span > 0 ? (inst->params()[t] - p->min) / span : 0.0f;
    inst->params()[tb_user_pots[i]] = (float) (int) (v01 * 127.0f + 0.5f);
}

/* ------------------------------------------------------------------ */
/* set / get                                                           */
/* ------------------------------------------------------------------ */

static void tb_set_param(void *instance, const char *key, const char *val)
{
    auto *inst = (tablor_instance *) instance;
    if (!inst || !key || !val) return;

    /* state restore: host sends "<prefix>:state" resolved to "state" upstream
     * or the raw prefixed key — accept both spellings. */
    const char *k = key;
    const char *colon = strrchr(key, ':');
    if (colon) k = colon + 1;

    if (!strcmp(k, "state")) {
        apply_state_blob(inst, val);
        return;
    }

    if (!strcmp(k, "preset")) {
        /* accept a preset NAME (Movy/web may echo the enum option) or index */
        int pi = -1;
        for (size_t i = 0; i < inst->presets.size(); i++)
            if (inst->presets[i].name == val) { pi = (int) i; break; }
        if (pi < 0) {
            char *end = nullptr;
            long n2 = strtol(val, &end, 10);
            if (end == val) return;
            pi = (int) n2;
        }
        if (pi < 0 || pi >= (int) inst->presets.size()) return;
        inst->preset_index = pi;
        /* a preset is a full sound: reset to defaults first so nothing
         * from the previous sound leaks through */
        reset_to_defaults(inst);
        apply_state_blob(inst, inst->presets[(size_t) pi].blob.c_str());
        return;
    }

    if (!strcmp(k, "save_preset")) {
        /* Trigger: never stores a value. Destination:
         *   "slot:N" (N = 1..8)  -> that user slot (the editor's save flow)
         *   "1" / "On"           -> current User slot, else first empty;
         *                           refuses when all 8 are taken. */
        const int userBase = (int) inst->presets.size() - kUserPresetSlots;
        int slot = -1;
        if (!strncmp(val, "slot:", 5)) {
            slot = atoi(val + 5) - 1;
        } else if (!strcmp(val, "1") || !strcmp(val, "On")) {
            if (inst->preset_index >= userBase)
                slot = inst->preset_index - userBase;
            else
                for (int i = 0; i < kUserPresetSlots; i++)
                    if (inst->presets[(size_t) (userBase + i)].blob == "TBLR1;") {
                        slot = i;
                        break;
                    }
        } else {
            return;
        }
        if (slot < 0 || slot >= kUserPresetSlots) return;
        build_state_blob(inst);

        int entry = userBase + slot;
        inst->presets[(size_t) entry].blob = inst->state_buf;
        inst->preset_index = entry;        /* you're now ON the saved slot */
        write_user_preset(inst, slot, inst->presets[(size_t) entry].name,
                          inst->state_buf);
        return;
    }

    if (!strcmp(k, "preset_name")) {
        /* rename the CURRENT slot — user slots only */
        const int userBase = (int) inst->presets.size() - kUserPresetSlots;
        if (inst->preset_index < userBase) return;
        std::string name = val;
        if (name.empty()) return;
        for (char &c : name)                       /* keep the file format safe */
            if (c == '|' || c == ';' || c == '\n' || c == '=') c = ' ';
        int slot = inst->preset_index - userBase;
        inst->presets[(size_t) inst->preset_index].name = name;
        write_user_preset(inst, slot, name,
                          inst->presets[(size_t) inst->preset_index].blob.c_str());
        return;
    }

    int idx = param_index(k);
    if (idx < 0) return;
    const tb_param_t *p = &tb_params[idx];

    if (p->type == TB_PATH) {
        set_table_path(inst, tb_path_slot(idx), val);
        return;
    }

    float v;
    if (p->type == TB_ENUM) {
        /* Accept an option NAME or an integer index — never trust one
         * spelling (the Forge atoi-collapse gotcha). */
        v = -1;
        for (int i = 0; i < p->n_options; i++)
            if (!strcmp(p->options[i], val)) { v = (float) i; break; }
        if (v < 0) {                            /* not a name — try a number */
            char *end = nullptr;
            float n = strtof(val, &end);
            if (end == val) return;             /* unknown name: ignore */
            v = (float) (int) n;
        }
    } else {
        v = strtof(val, nullptr);
    }

    inst->params()[idx] = clamp_param(p, v);

    /* keep the engine's mod-slot cache in step (cheap: 32 reads) */
    if (k[0] == 'm' && k[1] >= '1' && k[1] <= '8')
        inst->engine.syncModSlots();

    /* user macros */
    for (int i = 0; i < TB_USER_MACROS; i++) {
        if (idx == tb_user_pots[i]) { user_macro_apply(inst, i); break; }
        if (idx == tb_user_sels[i]) { user_macro_backsync(inst, i); break; }
    }
}

static int write_str(char *buf, int buf_len, const char *s)
{
    int n = (int) strlen(s);
    if (n >= buf_len) n = buf_len - 1;
    memcpy(buf, s, (size_t) n);
    buf[n] = 0;
    return n;
}

static int tb_get_param(void *instance, const char *key, char *buf, int buf_len)
{
    auto *inst = (tablor_instance *) instance;
    if (!inst || !key || !buf || buf_len <= 1) return -1;

    if (!strcmp(key, "chain_params"))
        return write_str(buf, buf_len, tb_chain_params_json);

    /* ui_hierarchy is deliberately NOT served: the Shadow UI's hierarchy
     * editor would take precedence over our ui_chain.js (the 9W9 rule). */

    /* Fresh newline-separated list of wavetable files, for ui_chain.js's
     * encoder stepping. Rescan on every call — names-only, a few ms, and
     * always current with whatever the user dropped in the folder. */
    if (!strcmp(key, "wt_files")) {
        inst->scanner.scan();
        int o = 0;
        for (const auto &e : inst->scanner.list()) {
            if (e.path.empty()) continue;          /* skip built-in Init */
            int need = (int) e.path.size() + 1;
            if (o + need >= buf_len - 1) break;
            memcpy(buf + o, e.path.c_str(), e.path.size());
            o += (int) e.path.size();
            buf[o++] = '\n';
        }
        buf[o] = 0;
        return o;
    }

    /* Shadow UI preset browser convention: preset_count / preset /
     * preset_name (the "No presets" screen reads exactly these). */
    if (!strcmp(key, "preset_count")) {
        char tmp[16];
        snprintf(tmp, sizeof tmp, "%d", (int) inst->presets.size());
        return write_str(buf, buf_len, tmp);
    }
    if (!strcmp(key, "preset")) {
        char tmp[16];
        snprintf(tmp, sizeof tmp, "%d", inst->preset_index);
        return write_str(buf, buf_len, tmp);
    }
    if (!strcmp(key, "preset_name")) {
        if (inst->preset_index < 0 ||
            inst->preset_index >= (int) inst->presets.size())
            return write_str(buf, buf_len, "");
        return write_str(buf, buf_len,
                         inst->presets[(size_t) inst->preset_index].name.c_str());
    }
    if (!strcmp(key, "preset_names")) {
        /* JSON array — Movy's buildPresetParam convention, also used by
         * ui_chain's list overlay and the web panel. */
        int o = 0;
        buf[o++] = '[';
        for (size_t i = 0; i < inst->presets.size() && o < buf_len - 8; i++) {
            if (i) buf[o++] = ',';
            buf[o++] = '"';
            for (const char *s = inst->presets[i].name.c_str();
                 *s && o < buf_len - 8; s++) {
                if (*s == '"' || *s == '\\') buf[o++] = '\\';
                buf[o++] = *s;
            }
            buf[o++] = '"';
        }
        buf[o++] = ']';
        buf[o] = 0;
        return o;
    }

    if (!strcmp(key, "state")) {
        build_state_blob(inst);
        return write_str(buf, buf_len, inst->state_buf);
    }

    int idx = param_index(key);
    if (idx < 0) return -1;
    const tb_param_t *p = &tb_params[idx];

    if (p->type == TB_PATH)
        return write_str(buf, buf_len, inst->wt_path[tb_path_slot(idx)]);

    if (p->type == TB_ENUM) {
        const char *name = p->options[(int) inst->params()[idx]];
        return write_str(buf, buf_len, name ? name : "?");
    }
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%g", (double) inst->params()[idx]);
    return write_str(buf, buf_len, tmp);
}

static int tb_get_error(void *, char *, int) { return 0; }

/* ------------------------------------------------------------------ */
/* MIDI + audio                                                        */
/* ------------------------------------------------------------------ */

static void tb_on_midi(void *instance, const uint8_t *msg, int len, int /*source*/)
{
    auto *inst = (tablor_instance *) instance;
    if (inst && msg)
        inst->engine.onMidi(msg, len);
}

static void tb_render_block(void *instance, int16_t *out_lr, int frames)
{
    auto *inst = (tablor_instance *) instance;
    if (!inst) {
        memset(out_lr, 0, (size_t) frames * 2 * sizeof(int16_t));
        return;
    }
    if (g_host && g_host->get_bpm)
        inst->engine.setHostBpm(g_host->get_bpm());
    inst->engine.renderBlock(out_lr, frames);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void *tb_create_instance(const char *module_dir, const char *json_defaults)
{
    (void) json_defaults;
    auto *inst = new (std::nothrow) tablor_instance();
    if (!inst) return nullptr;

    if (module_dir)
        snprintf(inst->module_dir, sizeof inst->module_dir, "%s", module_dir);

    tb::WtScanner::seedUserFolder(inst->module_dir);
    inst->scanner.scan();
    load_presets(inst);

    if (g_host && g_host->log) {
        char msg[128];
        snprintf(msg, sizeof msg, "tablor: v" TB_VERSION ", %d wavetables found",
                 (int) inst->scanner.list().size());
        g_host->log(msg);
    }
    return inst;
}

static void tb_destroy_instance(void *instance)
{
    delete (tablor_instance *) instance;
}

static plugin_api_v2_t g_api = {
    /* .api_version      = */ 2,
    /* .create_instance  = */ tb_create_instance,
    /* .destroy_instance = */ tb_destroy_instance,
    /* .on_midi          = */ tb_on_midi,
    /* .set_param        = */ tb_set_param,
    /* .get_param        = */ tb_get_param,
    /* .get_error        = */ tb_get_error,
    /* .render_block     = */ tb_render_block,
};

extern "C" plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host)
{
    g_host = host;
    return &g_api;
}
