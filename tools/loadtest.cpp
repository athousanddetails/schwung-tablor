/* Tablor loader test — dlopens dsp.so exactly like Schwung's chain host does,
 * then verifies the phase 0 contract:
 *   - init + create_instance succeed
 *   - chain_params serves valid-looking JSON with every key
 *   - set_param/get_param round-trip (int, enum-by-name, enum-by-index)
 *   - state round-trips, and a stale/garbage blob is IGNORED (not half-applied)
 *   - render_block fills silence and doesn't crash across many blocks
 *
 * Params apply-testing matters: an ER-99 stress test once passed while every
 * control was dead. This test asserts VALUES, not just survival.
 *
 * Run on the Move:  ./tablor_loadtest ./dsp.so
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <dlfcn.h>

#include "../src/host/plugin_api_v1.h"

static int g_fail = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
    else         {           printf("  ok: " __VA_ARGS__); printf("\n"); } \
} while (0)

static void host_log(const char *msg) { printf("  [host log] %s\n", msg); }

int main(int argc, char **argv)
{
    const char *so_path = (argc > 1) ? argv[1] : "./dsp.so";

    void *dl = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (!dl) { printf("FAIL: dlopen: %s\n", dlerror()); return 1; }
    printf("  ok: dlopen %s\n", so_path);

    auto init = (move_plugin_init_v2_fn) dlsym(dl, MOVE_PLUGIN_INIT_V2_SYMBOL);
    CHECK(init != nullptr, "dlsym " MOVE_PLUGIN_INIT_V2_SYMBOL);
    if (!init) return 1;

    host_api_v1_t host = {};
    host.api_version = 1;
    host.sample_rate = MOVE_SAMPLE_RATE;
    host.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    host.log = host_log;

    plugin_api_v2_t *api = init(&host);
    CHECK(api && api->api_version == 2, "init returns v2 api");
    if (!api) return 1;

    void *inst = api->create_instance(".", nullptr);
    CHECK(inst != nullptr, "create_instance");
    if (!inst) return 1;

    static char big[64 * 1024];

    /* ---- chain_params ---- */
    int n = api->get_param(inst, "chain_params", big, sizeof big);
    CHECK(n > 100, "chain_params returns %d bytes", n);
    CHECK(big[0] == '[' && big[n - 1] == ']', "chain_params looks like a JSON array");
    CHECK(strstr(big, "\"wt1_table\"") && strstr(big, "\"volume\"") &&
          strstr(big, "\"m8_on\""), "chain_params contains first/mid/last keys");
    CHECK(strstr(big, "%s") == nullptr, "no unfilled %%s slots leaked");

    /* ---- int param round-trip ---- */
    char buf[256];
    api->set_param(inst, "flt_freq", "93");
    n = api->get_param(inst, "flt_freq", buf, sizeof buf);
    CHECK(n > 0 && !strcmp(buf, "93"), "flt_freq set 93 -> get \"%s\"", buf);

    api->set_param(inst, "flt_freq", "999");            /* clamps to 127 */
    api->get_param(inst, "flt_freq", buf, sizeof buf);
    CHECK(!strcmp(buf, "127"), "flt_freq clamps 999 -> \"%s\"", buf);

    /* ---- enum by name and by index ---- */
    api->set_param(inst, "flt_type", "HP 24");
    api->get_param(inst, "flt_type", buf, sizeof buf);
    CHECK(!strcmp(buf, "HP 24"), "flt_type by name -> \"%s\"", buf);

    api->set_param(inst, "flt_type", "4");              /* index of BP 12 */
    api->get_param(inst, "flt_type", buf, sizeof buf);
    CHECK(!strcmp(buf, "BP 12"), "flt_type by index 4 -> \"%s\"", buf);

    api->set_param(inst, "flt_type", "NoSuchType");     /* must be ignored */
    api->get_param(inst, "flt_type", buf, sizeof buf);
    CHECK(!strcmp(buf, "BP 12"), "unknown enum name ignored -> \"%s\"", buf);

    /* ---- dynamic enum placeholder ---- */
    api->get_param(inst, "wt1_table", buf, sizeof buf);
    CHECK(!strcmp(buf, "Init"), "wt1_table placeholder -> \"%s\"", buf);

    /* ---- state round-trip ---- */
    api->set_param(inst, "vca_r", "77");
    n = api->get_param(inst, "state", big, sizeof big);
    CHECK(n > 6 && !strncmp(big, "TBLR1;", 6), "state is version-tagged (%d bytes)", n);
    CHECK(strstr(big, "vca_r=77") != nullptr, "state contains vca_r=77");

    static char saved[sizeof big];
    memcpy(saved, big, sizeof saved);
    api->set_param(inst, "vca_r", "10");
    api->set_param(inst, "synth:state", saved);         /* prefixed spelling */
    api->get_param(inst, "vca_r", buf, sizeof buf);
    CHECK(!strcmp(buf, "77"), "state restore via synth:state -> vca_r=\"%s\"", buf);

    api->set_param(inst, "vca_r", "10");
    api->set_param(inst, "state", "GARBAGE;vca_r=99;"); /* stale blob: ignore */
    api->get_param(inst, "vca_r", buf, sizeof buf);
    CHECK(!strcmp(buf, "10"), "untagged blob fully ignored -> vca_r=\"%s\"", buf);

    /* ---- render: silence, no crash, many blocks ---- */
    int16_t out[MOVE_FRAMES_PER_BLOCK * 2];
    memset(out, 0x55, sizeof out);
    long acc = 0;
    for (int b = 0; b < 2000; b++) {                    /* ~5.8 s of audio */
        api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
        for (size_t i = 0; i < MOVE_FRAMES_PER_BLOCK * 2; i++)
            acc += out[i] < 0 ? -out[i] : out[i];
    }
    CHECK(acc == 0, "2000 blocks rendered, all silence (acc=%ld)", acc);

    /* ---- midi doesn't crash ---- */
    const uint8_t note_on[]  = { 0x90, 60, 100 };
    const uint8_t note_off[] = { 0x80, 60, 0 };
    api->on_midi(inst, note_on, 3, 0);
    api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
    api->on_midi(inst, note_off, 3, 0);
    CHECK(1, "midi note on/off survived");

    api->destroy_instance(inst);
    dlclose(dl);

    printf("\n%s (%d failures)\n", g_fail ? "LOADTEST FAILED" : "LOADTEST PASSED", g_fail);
    return g_fail ? 1 : 0;
}
