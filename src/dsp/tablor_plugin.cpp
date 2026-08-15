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

static const host_api_v1_t *g_host = nullptr;

/* ------------------------------------------------------------------ */
/* Instance                                                            */
/* ------------------------------------------------------------------ */

/* Phase 4 replaces this with the wavetable scanner's list. */
static const char *const k_dyn_placeholder[] = { "Init" };

struct tablor_instance {
    tb::Engine engine;
    char  module_dir[512] = {};

    /* chain_params JSON is assembled once per request into this buffer.
     * fmt is ~13 KB; dynamic option lists will add table names later. */
    char  chain_buf[48 * 1024];

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

/* Dynamic enum option table (wavetables). Phase 4 replaces this with the
 * scanner's list; everything already routes through these two calls. */
static int dyn_option_count(const tablor_instance *, int /*param*/)
{
    return 1;
}
static const char *dyn_option_name(const tablor_instance *, int /*param*/, int idx)
{
    return (idx == 0) ? k_dyn_placeholder[0] : nullptr;
}

static float clamp_param(const tb_param_t *p, float v, const tablor_instance *inst, int idx)
{
    float lo = p->min, hi = p->max;
    if (p->n_options == -1)                    /* dynamic enum: live count */
        hi = (float) (dyn_option_count(inst, idx) - 1);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
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
        char kbuf[64], vbuf[128];
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
                if (idx >= 0)
                    inst->params()[idx] = clamp_param(&tb_params[idx],
                                                    strtof(vbuf, nullptr), inst, idx);
            }
            p = (*semi) ? semi + 1 : semi;
        }
        inst->engine.syncModSlots();
        return;
    }

    int idx = param_index(k);
    if (idx < 0) return;
    const tb_param_t *p = &tb_params[idx];

    float v;
    if (p->type == TB_ENUM) {
        /* Accept an option NAME or an integer index — never trust one
         * spelling (the Forge atoi-collapse gotcha). */
        v = -1;
        if (p->n_options > 0) {
            for (int i = 0; i < p->n_options; i++)
                if (!strcmp(p->options[i], val)) { v = (float) i; break; }
        } else {                                /* dynamic enum */
            int n = dyn_option_count(inst, idx);
            for (int i = 0; i < n; i++) {
                const char *name = dyn_option_name(inst, idx, i);
                if (name && !strcmp(name, val)) { v = (float) i; break; }
            }
        }
        if (v < 0) {                            /* not a name — try a number */
            char *end = nullptr;
            float n = strtof(val, &end);
            if (end == val) return;             /* unknown name: ignore */
            v = (float) (int) n;
        }
    } else {
        v = strtof(val, nullptr);
    }

    inst->params()[idx] = clamp_param(p, v, inst, idx);

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

    if (!strcmp(key, "chain_params")) {
        /* Assemble the dynamic option lists as JSON arrays. Phase 4: the
         * scanner's table names go here. */
        char opts[40 * 1024];
        int  n = dyn_option_count(inst, TB_P_WT1_TABLE);
        int  o = 0;
        opts[o++] = '[';
        for (int i = 0; i < n && o < (int) sizeof opts - 64; i++) {
            if (i) opts[o++] = ',';
            o += snprintf(opts + o, sizeof opts - (size_t) o, "\"%s\"",
                          dyn_option_name(inst, TB_P_WT1_TABLE, i));
        }
        opts[o++] = ']'; opts[o] = 0;

        char def1[136], def2[136];
        snprintf(def1, sizeof def1, "\"%s\"",
                 dyn_option_name(inst, TB_P_WT1_TABLE,
                                 (int) inst->params()[TB_P_WT1_TABLE]));
        snprintf(def2, sizeof def2, "\"%s\"",
                 dyn_option_name(inst, TB_P_WT2_TABLE,
                                 (int) inst->params()[TB_P_WT2_TABLE]));

        int len = snprintf(inst->chain_buf, sizeof inst->chain_buf,
                           tb_chain_params_fmt, opts, def1, opts, def2);
        if (len < 0 || len >= (int) sizeof inst->chain_buf) return -1;
        return write_str(buf, buf_len, inst->chain_buf);
    }

    if (!strcmp(key, "state")) {
        int o = snprintf(inst->state_buf, sizeof inst->state_buf, "TBLR1;");
        for (int i = 0; i < TB_PARAM_COUNT; i++) {
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

    if (p->type == TB_ENUM) {
        const char *name = (p->n_options > 0)
            ? p->options[(int) inst->params()[idx]]
            : dyn_option_name(inst, idx, (int) inst->params()[idx]);
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

    if (g_host && g_host->log)
        g_host->log("tablor: instance created (v" TB_VERSION ")");
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
