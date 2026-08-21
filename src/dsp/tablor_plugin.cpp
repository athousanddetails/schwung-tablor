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
#include <algorithm>
#include <dirent.h>
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
    char  wt_path[TB_WT_SLOTS][512] = {};

    /* presets: factory (from the module) + UNLIMITED user .tblr files in
     * the user library (filename = preset name, content = TBLR1 blob) */
    struct preset { std::string name, blob, path; };   /* path "" = factory */
    std::vector<preset> presets;
    int preset_index = 0;
    int factory_count = 0;

    /* state blob scratch */
    char  state_buf[8 * 1024];

    /* ---- Turnable wavetable selection -------------------------------
     * The filepath control (wt1_table/wt2_table) is OPAQUE to the host:
     * it can be opened, never turned, because turning a path would write
     * nonsense into it. So a pot cannot change the wavetable, which is
     * what was reported.
     *
     * These add the turnable half WITHOUT touching the filepath control:
     * a pack chosen from a list, and one enum per oscillator whose
     * options are the entries in that pack. An enum is turnable AND
     * divable, so hold+click opens a scrolling picker for a long pack.
     *
     * Rebuilt only when the scan or the pack changes -- NEVER inside
     * get_param, which runs on the SPI callback (see the note above
     * tb_chain_params_json). get_param only ever hands out a pointer. */
    std::vector<std::string> packs;      /* distinct "Pack" prefixes, [0] = All */
    std::vector<int>         filtered;   /* indices into scanner.list() */
    int          wt_pack = 0;
    std::string  chain_params_cache;     /* static head + dynamic options */

    float *params() { return engine.pots; }
};

/* User presets are plain files the user can copy, share and manage:
 *   /data/UserData/UserLibrary/Tablor Presets/<Name>.tblr
 * filename (minus extension) IS the preset name; content is one
 * TBLR1 state blob line. */
static constexpr const char *kPresetDir =
    "/data/UserData/UserLibrary/Tablor Presets";

static std::string sanitize_preset_name(const char *raw)
{
    std::string n;
    for (const char *s = raw; *s && n.size() < 40; s++) {
        char c = *s;
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || c == ';' ||
            c == '=' || c == '\n' || c == '\r')
            c = ' ';
        n += c;
    }
    while (!n.empty() && (n.front() == ' ' || n.front() == '.')) n.erase(0, 1);
    while (!n.empty() && n.back() == ' ') n.pop_back();
    if (n.empty()) n = "Untitled";
    return n;
}

static std::string preset_file_for(const std::string &name)
{
    return std::string(kPresetDir) + "/" + name + ".tblr";
}

/* A name nobody has yet: "Name", "Name 2", "Name 3", … */
static std::string unique_preset_name(const std::string &want)
{
    std::string name = want;
    for (int i = 2; i < 1000; i++) {
        struct stat st;
        if (::stat(preset_file_for(name).c_str(), &st) != 0)
            return name;
        name = want + " " + std::to_string(i);
    }
    return want;
}

static bool read_blob_file(const char *path, std::string &blob)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[8192];
    bool ok = false;
    if (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        const char *b = line;
        const char *bar = strchr(line, '|');    /* legacy "Name|blob" lines */
        if (bar) b = bar + 1;
        if (!strncmp(b, "TBLR1;", 6)) { blob = b; ok = true; }
    }
    fclose(f);
    return ok;
}

static void write_preset_file(const std::string &path, const char *blob)
{
    FILE *f = fopen(path.c_str(), "w");
    if (f) {
        fputs(blob, f);
        fputc('\n', f);
        fclose(f);
    }
}

/* One-time migration of the old fixed-slot files (presets/user/uN.tbl). */
static void migrate_old_user_slots(tablor_instance *inst)
{
    for (int i = 0; i < 8; i++) {
        char old[600];
        snprintf(old, sizeof old, "%s/presets/user/u%d.tbl",
                 inst->module_dir, i + 1);
        FILE *f = fopen(old, "r");
        if (!f) continue;
        char line[8192];
        std::string name = "User " + std::to_string(i + 1), blob;
        if (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *bar = strchr(line, '|');
            const char *b = line;
            if (bar) { *bar = 0; if (line[0]) name = line; b = bar + 1; }
            if (!strncmp(b, "TBLR1;", 6)) blob = b;
        }
        fclose(f);
        if (!blob.empty() && blob != "TBLR1;")
            write_preset_file(preset_file_for(unique_preset_name(
                sanitize_preset_name(name.c_str()))), blob.c_str());
        ::remove(old);
    }
}

/* Re-list the user preset folder (sorted by name, case-insensitive). */
static void scan_user_presets(tablor_instance *inst)
{
    inst->presets.resize((size_t) inst->factory_count);

    std::vector<tablor_instance::preset> user;
    DIR *d = ::opendir(kPresetDir);
    if (d) {
        while (dirent *e = ::readdir(d)) {
            const char *dot = strrchr(e->d_name, '.');
            if (!dot || strcasecmp(dot, ".tblr") != 0) continue;
            std::string path = std::string(kPresetDir) + "/" + e->d_name;
            std::string blob;
            if (!read_blob_file(path.c_str(), blob)) continue;
            user.push_back({ std::string(e->d_name, (size_t) (dot - e->d_name)),
                             blob, path });
        }
        ::closedir(d);
    }
    std::sort(user.begin(), user.end(),
              [](const tablor_instance::preset &a,
                 const tablor_instance::preset &b) {
                  return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
              });
    for (auto &u : user)
        inst->presets.push_back(std::move(u));

    if (inst->preset_index >= (int) inst->presets.size())
        inst->preset_index = 0;
}

static void select_preset_by_path(tablor_instance *inst, const std::string &path)
{
    for (size_t i = 0; i < inst->presets.size(); i++)
        if (inst->presets[i].path == path) {
            inst->preset_index = (int) i;
            return;
        }
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
            inst->presets.push_back({ line, blob, "" });
        }
        fclose(f);
    }
    inst->factory_count = (int) inst->presets.size();

    ::mkdir(kPresetDir, 0755);
    migrate_old_user_slots(inst);
    scan_user_presets(inst);
}

static int param_index(const char *key)
{
    for (int i = 0; i < TB_PARAM_COUNT; i++)
        if (!strcmp(tb_params[i].key, key))
            return i;
    return -1;
}

/* Which wavetable slot a param drives: wt1_table -> 0, wt2_table -> 1. */
static int tb_path_slot(int param_idx)
{
    return param_idx == TB_P_WT1_TABLE ? 0 : 1;
}

/* Store a path and post the background load. "" or "Init" = built-in. */
/* The scanner list IS the enum: index 0 = "Init" (built-in), then every
 * file found. Keep the param's index and the loaded path in step. */
static void sync_table_index(tablor_instance *inst, int osc)
{
    const auto &l = inst->scanner.list();
    int idx = 0;
    for (size_t i = 0; i < l.size(); i++)
        if (l[i].path == inst->wt_path[osc]) { idx = (int) i; break; }
    inst->params()[osc == 0 ? TB_P_WT1_TABLE : TB_P_WT2_TABLE] = (float) idx;
}

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
    sync_table_index(inst, osc);
}

/* ------------------------------------------------------------------ */
/* Pack list + per-pack enum options                                    */
/*                                                                      */
/* WtEntry.name is already "Pack/Name" or "Name", so the pack list costs */
/* no extra filesystem work -- it is a pass over a list we already hold. */
/* ------------------------------------------------------------------ */

static std::string tb_pack_of(const std::string &name)
{
    size_t slash = name.find('/');
    return slash == std::string::npos ? std::string() : name.substr(0, slash);
}

static std::string tb_leaf_of(const std::string &name)
{
    size_t slash = name.rfind('/');
    return slash == std::string::npos ? name : name.substr(slash + 1);
}

/* JSON-escape into a std::string. The option names are filenames, so a
 * quote or a backslash is unlikely but entirely legal on disk -- and one
 * unescaped byte turns the whole contract into a failed read. */
static void tb_json_append(std::string &out, const std::string &in)
{
    for (char c : in) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if ((unsigned char) c < 0x20) continue;
        else out += c;
    }
}

static void tb_rebuild_packs(tablor_instance *inst)
{
    inst->packs.clear();
    inst->packs.push_back("All");
    for (const auto &e : inst->scanner.list()) {
        std::string pk = tb_pack_of(e.name);
        if (pk.empty()) continue;
        bool seen = false;
        for (size_t i = 1; i < inst->packs.size(); i++)
            if (inst->packs[i] == pk) { seen = true; break; }
        if (!seen) inst->packs.push_back(pk);
    }
    if (inst->wt_pack >= (int) inst->packs.size()) inst->wt_pack = 0;
}

/* The host's chain-host parser caps an enum at MAX_ENUM_OPTIONS (128).
 * Past that the knob grid still lists them (the JS parses the JSON
 * itself) but CC knob mapping and modulation targets truncate, which
 * fails silently -- so stop at the cap rather than publish a list only
 * half of the host can see. */
static const size_t TB_MAX_ENUM_OPTIONS = 128;

static void tb_rebuild_filtered(tablor_instance *inst)
{
    inst->filtered.clear();
    const auto &l = inst->scanner.list();
    const bool all = (inst->wt_pack <= 0);
    const std::string want = all ? std::string()
                                 : inst->packs[(size_t) inst->wt_pack];
    for (size_t i = 0; i < l.size(); i++) {
        if (i == 0) { inst->filtered.push_back(0); continue; }   /* Init */
        if (all || tb_pack_of(l[i].name) == want)
            inst->filtered.push_back((int) i);
        if (inst->filtered.size() >= TB_MAX_ENUM_OPTIONS) break;
    }
}

/* Where the currently loaded table sits in the filtered list, or 0. */
static int tb_select_index(tablor_instance *inst, int osc)
{
    const auto &l = inst->scanner.list();
    for (size_t i = 0; i < inst->filtered.size(); i++) {
        int g = inst->filtered[i];
        if (g >= 0 && g < (int) l.size() && l[(size_t) g].path == inst->wt_path[osc])
            return (int) i;
    }
    return 0;
}

/* Static head + the three dynamic entries. Built here so get_param can
 * hand out a pointer and nothing else. */
static void tb_rebuild_chain_params(tablor_instance *inst)
{
    std::string &out = inst->chain_params_cache;
    out.assign(tb_chain_params_json);
    if (out.size() < 2 || out.back() != ']') return;   /* not the shape we expect */
    out.pop_back();                                     /* drop the closing ] */

    const auto &l = inst->scanner.list();

    out += ",{\"key\":\"wt_pack\",\"name\":\"WT Pack\",\"type\":\"enum\",\"options\":[";
    for (size_t i = 0; i < inst->packs.size(); i++) {
        if (i) out += ",";
        out += "\""; tb_json_append(out, inst->packs[i]); out += "\"";
    }
    out += "]}";

    for (int osc = 0; osc < 2; osc++) {
        char head[96];
        snprintf(head, sizeof head,
                 ",{\"key\":\"wt%d_select\",\"name\":\"WT%d Table\",\"type\":\"enum\",\"options\":[",
                 osc + 1, osc + 1);
        out += head;
        for (size_t i = 0; i < inst->filtered.size(); i++) {
            int g = inst->filtered[i];
            if (i) out += ",";
            out += "\"";
            tb_json_append(out, g == 0 ? std::string("Init")
                                       : tb_leaf_of(l[(size_t) g].name));
            out += "\"";
        }
        out += "]}";
    }
    out += "]";
}

static void tb_refresh_selection_contract(tablor_instance *inst)
{
    tb_rebuild_packs(inst);
    tb_rebuild_filtered(inst);
    tb_rebuild_chain_params(inst);
}

/* Select by enum index into the live scan. */
static void set_table_index(tablor_instance *inst, int osc, int idx)
{
    const auto &l = inst->scanner.list();
    if (idx < 0 || idx >= (int) l.size()) return;
    set_table_path(inst, osc, l[(size_t) idx].path.c_str());
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
        if (tb_params[i].type == TB_PATH) {   /* wavetable: store the path */
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
         *   "new"      -> a fresh .tblr file ("Untitled", "Untitled 2", …)
         *   "user:N"   -> overwrite the N-th user preset (0-based)
         *   "1" / "On" -> overwrite the current user preset, else new */
        build_state_blob(inst);
        std::string path;
        if (!strcmp(val, "new")) {
            path = preset_file_for(unique_preset_name("Untitled"));
        } else if (!strncmp(val, "user:", 5)) {
            int ui = inst->factory_count + atoi(val + 5);
            if (ui < inst->factory_count || ui >= (int) inst->presets.size())
                return;
            path = inst->presets[(size_t) ui].path;
        } else if (!strcmp(val, "1") || !strcmp(val, "On")) {
            if (inst->preset_index >= inst->factory_count)
                path = inst->presets[(size_t) inst->preset_index].path;
            else
                path = preset_file_for(unique_preset_name("Untitled"));
        } else {
            return;
        }
        if (path.empty()) return;
        write_preset_file(path, inst->state_buf);
        scan_user_presets(inst);
        select_preset_by_path(inst, path);
        return;
    }

    if (!strcmp(k, "preset_name")) {
        /* rename the CURRENT user preset = rename its file */
        if (inst->preset_index < inst->factory_count) return;
        auto &cur = inst->presets[(size_t) inst->preset_index];
        std::string want = sanitize_preset_name(val);
        if (want == cur.name) return;
        std::string newPath = preset_file_for(unique_preset_name(want));
        write_preset_file(newPath, cur.blob.c_str());
        ::remove(cur.path.c_str());
        scan_user_presets(inst);
        select_preset_by_path(inst, newPath);
        return;
    }

    /* Turnable wavetable selection. Indices only: get_param reports an
     * index for these, so the host learns WIRE_INDEX and never sends a
     * name. Nothing here touches the filesystem. */
    if (!strcmp(k, "wt_pack")) {
        int n = atoi(val);
        if (n < 0) n = 0;
        if (n >= (int) inst->packs.size()) n = (int) inst->packs.size() - 1;
        if (n == inst->wt_pack) return;
        inst->wt_pack = n;
        /* Republish: wt1_select / wt2_select now have different options.
         * The host re-reads the contract after an items selection settles
         * (page_controller commitItem -> armContractSettle), which is why
         * the pack is offered as an items level and not as a plain enum
         * knob -- a plain enum commit does NOT arm the re-read. */
        tb_rebuild_filtered(inst);
        tb_rebuild_chain_params(inst);
        return;
    }
    if (!strcmp(k, "wt1_select") || !strcmp(k, "wt2_select")) {
        int osc = (k[2] == '2') ? 1 : 0;
        int n = atoi(val);
        if (n < 0 || n >= (int) inst->filtered.size()) return;
        set_table_index(inst, osc, inst->filtered[(size_t) n]);
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

    /* Static string: a copy, nothing more. get_param is serviced from the
     * SPI callback (chain_internal.h) — anything that formats or reads the
     * filesystem here jams the param bus. */
    if (!strcmp(key, "chain_params"))
        return write_str(buf, buf_len,
                         inst->chain_params_cache.empty()
                             ? tb_chain_params_json
                             : inst->chain_params_cache.c_str());

    /* Turnable wavetable selection. All three are answered from memory --
     * no scan, no allocation beyond the small formatting buffer. */
    if (!strcmp(key, "wt_pack")) {
        char tmp[16];
        snprintf(tmp, sizeof tmp, "%d", inst->wt_pack);
        return write_str(buf, buf_len, tmp);
    }
    if (!strcmp(key, "wt_pack_list")) {
        std::string out = "[";
        for (size_t i = 0; i < inst->packs.size(); i++) {
            char head[32];
            snprintf(head, sizeof head, "%s{\"index\":%d,\"label\":\"", i ? "," : "", (int) i);
            out += head;
            tb_json_append(out, inst->packs[i]);
            out += "\"}";
        }
        out += "]";
        return write_str(buf, buf_len, out.c_str());
    }
    if (!strcmp(key, "wt1_select") || !strcmp(key, "wt2_select")) {
        char tmp[16];
        snprintf(tmp, sizeof tmp, "%d",
                 tb_select_index(inst, key[2] == '2' ? 1 : 0));
        return write_str(buf, buf_len, tmp);
    }

    /* Schwung 0.12+: the stock hierarchy editor (viz graphics, preset
     * browser, file browser, keyboard) IS Tablor's on-device UI. */
    if (!strcmp(key, "ui_hierarchy"))
        return write_str(buf, buf_len, tb_ui_hierarchy_json);

    /* Shadow UI preset browser convention: preset_count / preset /
     * preset_name (the "No presets" screen reads exactly these). */
    if (!strcmp(key, "preset_count")) {
        char tmp[16];
        snprintf(tmp, sizeof tmp, "%d", (int) inst->presets.size());
        return write_str(buf, buf_len, tmp);
    }
    if (!strcmp(key, "preset_factory_count")) {
        char tmp[16];
        snprintf(tmp, sizeof tmp, "%d", inst->factory_count);
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
    /* Derive the pack list and the published option lists ONCE, here,
     * off the single scan -- so get_param never has to. */
    tb_refresh_selection_contract(inst);
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
