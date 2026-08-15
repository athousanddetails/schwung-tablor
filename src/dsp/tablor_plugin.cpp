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
    tb::WtScanner scanner;
    tb::WtLoader loader { engine };
    char  module_dir[512] = {};

    /* chain_params JSON is assembled once per request into this buffer;
     * options lists (the scanned table names) build in opts_buf. */
    char  chain_buf[96 * 1024];
    char  opts_buf[64 * 1024];

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

/* Dynamic enum options (the wavetable list) come from the scanner. */
static int dyn_option_count(const tablor_instance *inst, int /*param*/)
{
    return (int) inst->scanner.list().size();
}
static const char *dyn_option_name(const tablor_instance *inst, int /*param*/, int idx)
{
    const auto &l = inst->scanner.list();
    if (idx < 0 || idx >= (int) l.size()) return nullptr;
    return l[(size_t) idx].name.c_str();
}

/* Post a background load for the table the pot now points at. */
static void request_table(tablor_instance *inst, int osc)
{
    int idx = (int) inst->params()[osc == 0 ? TB_P_WT1_TABLE : TB_P_WT2_TABLE];
    const auto &l = inst->scanner.list();
    if (idx >= 0 && idx < (int) l.size())
        inst->loader.requestLoad(osc, l[(size_t) idx]);
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
                if (!strcmp(kbuf, "wt1_table_name") || !strcmp(kbuf, "wt2_table_name")) {
                    int osc = kbuf[2] == '1' ? 0 : 1;
                    int ti = inst->scanner.indexOfName(vbuf);
                    inst->params()[osc == 0 ? TB_P_WT1_TABLE : TB_P_WT2_TABLE] =
                        (float) (ti >= 0 ? ti : 0);
                } else {
                    int idx = param_index(kbuf);
                    if (idx >= 0)
                        inst->params()[idx] = clamp_param(&tb_params[idx],
                                                        strtof(vbuf, nullptr), inst, idx);
                }
            }
            p = (*semi) ? semi + 1 : semi;
        }
        inst->engine.syncModSlots();
        request_table(inst, 0);
        request_table(inst, 1);
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

    /* wavetable selection -> background load + swap */
    if (idx == TB_P_WT1_TABLE) request_table(inst, 0);
    if (idx == TB_P_WT2_TABLE) request_table(inst, 1);

    /* rescan trigger: re-list the folders, keep current selections by name */
    if (idx == TB_P_WT_RESCAN && inst->params()[idx] > 0.5f) {
        char n1[256] = {}, n2[256] = {};
        const char *c1 = dyn_option_name(inst, TB_P_WT1_TABLE,
                                         (int) inst->params()[TB_P_WT1_TABLE]);
        const char *c2 = dyn_option_name(inst, TB_P_WT2_TABLE,
                                         (int) inst->params()[TB_P_WT2_TABLE]);
        if (c1) snprintf(n1, sizeof n1, "%s", c1);
        if (c2) snprintf(n2, sizeof n2, "%s", c2);

        inst->scanner.scan();

        int i1 = inst->scanner.indexOfName(n1);
        int i2 = inst->scanner.indexOfName(n2);
        inst->params()[TB_P_WT1_TABLE] = (float) (i1 >= 0 ? i1 : 0);
        inst->params()[TB_P_WT2_TABLE] = (float) (i2 >= 0 ? i2 : 0);
        inst->params()[TB_P_WT_RESCAN] = 0.0f;      /* trigger re-arms */
        request_table(inst, 0);
        request_table(inst, 1);
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

    if (!strcmp(key, "chain_params")) {
        /* Assemble the scanned table names as a JSON string array. */
        char *opts = inst->opts_buf;
        const size_t optsCap = sizeof inst->opts_buf;
        int  n = dyn_option_count(inst, TB_P_WT1_TABLE);
        size_t o = 0;
        opts[o++] = '[';
        for (int i = 0; i < n && o < optsCap - 8; i++) {
            if (i) opts[o++] = ',';
            opts[o++] = '"';
            for (const char *s = dyn_option_name(inst, TB_P_WT1_TABLE, i);
                 s && *s && o < optsCap - 8; s++) {
                if (*s == '"' || *s == '\\') opts[o++] = '\\';
                opts[o++] = *s;
            }
            opts[o++] = '"';
        }
        opts[o++] = ']'; opts[o] = 0;

        char def1[264], def2[264];
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

    if (!strcmp(key, "ui_hierarchy"))
        return write_str(buf, buf_len, tb_ui_hierarchy_json);

    if (!strcmp(key, "state")) {
        int o = snprintf(inst->state_buf, sizeof inst->state_buf, "TBLR1;");
        for (int i = 0; i < TB_PARAM_COUNT; i++) {
            if (tb_params[i].n_options == -1) continue; /* tables: by name below */
            float v = inst->params()[i];
            if (v == tb_params[i].def) continue;        /* defaults are implicit */
            o += snprintf(inst->state_buf + o, sizeof inst->state_buf - (size_t) o,
                          "%s=%g;", tb_params[i].key, (double) v);
            if (o >= (int) sizeof inst->state_buf - 64) break;
        }
        /* wavetable selections are stored BY NAME — indexes shift when the
         * user adds files, names don't. */
        const char *n1 = dyn_option_name(inst, TB_P_WT1_TABLE,
                                         (int) inst->params()[TB_P_WT1_TABLE]);
        const char *n2 = dyn_option_name(inst, TB_P_WT2_TABLE,
                                         (int) inst->params()[TB_P_WT2_TABLE]);
        o += snprintf(inst->state_buf + o, sizeof inst->state_buf - (size_t) o,
                      "wt1_table_name=%s;wt2_table_name=%s;",
                      n1 ? n1 : "Init", n2 ? n2 : "Init");
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
