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

    /* state blob scratch */
    char  state_buf[8 * 1024];

    float *params() { return engine.pots; }
};

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
        /* Version-tagged blob: "TBLR1;key=val;key=val;…". Anything without
         * the tag is from another life — ignore it entirely rather than
         * half-apply it (the ER-99 total-silence lesson). */
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

    if (!strcmp(key, "ui_hierarchy"))
        return write_str(buf, buf_len, tb_ui_hierarchy_json);

    if (!strcmp(key, "state")) {
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
            float v = inst->params()[i];
            if (v == tb_params[i].def) continue;        /* defaults are implicit */
            o += snprintf(inst->state_buf + o, sizeof inst->state_buf - (size_t) o,
                          "%s=%g;", tb_params[i].key, (double) v);
            if (o >= (int) sizeof inst->state_buf - 64) break;
        }
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
