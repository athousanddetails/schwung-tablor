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
#include <ctime>

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

    /* ---- wavetable filepath default: empty = built-in Init ---- */
    n = api->get_param(inst, "wt1_table", buf, sizeof buf);
    CHECK(n == 0 && buf[0] == 0, "wt1_table default empty (built-in Init)");

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

    /* ---- render: silence before any note ---- */
    int16_t out[MOVE_FRAMES_PER_BLOCK * 2];
    memset(out, 0x55, sizeof out);
    long acc = 0;
    for (int b = 0; b < 200; b++) {
        api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
        for (size_t i = 0; i < MOVE_FRAMES_PER_BLOCK * 2; i++)
            acc += out[i] < 0 ? -out[i] : out[i];
    }
    CHECK(acc == 0, "silence before any note (acc=%ld)", acc);

    /* ---- note on -> SOUND (the engine gate), note off -> decay to zero */
    const uint8_t note_on[]  = { 0x90, 60, 100 };
    const uint8_t note_off[] = { 0x80, 60, 0 };
    api->on_midi(inst, note_on, 3, 0);
    long peak = 0;
    for (int b = 0; b < 200; b++) {                     /* ~0.6 s held */
        api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
        for (size_t i = 0; i < MOVE_FRAMES_PER_BLOCK * 2; i++) {
            long v = out[i] < 0 ? -out[i] : out[i];
            if (v > peak) peak = v;
        }
    }
    CHECK(peak > 2000, "note 60 makes sound (peak %ld of 32767)", peak);
    CHECK(peak < 32767, "not clipping at defaults (peak %ld)", peak);

    /* chord: 4 more notes, still sane */
    const uint8_t chordNotes[4] = { 48, 64, 67, 72 };
    for (int ci = 0; ci < 4; ci++) {
        const uint8_t on[] = { 0x90, chordNotes[ci], 100 };
        api->on_midi(inst, on, 3, 0);
    }
    long chordPeak = 0;
    for (int b = 0; b < 200; b++) {
        api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
        for (size_t i = 0; i < MOVE_FRAMES_PER_BLOCK * 2; i++) {
            long v = out[i] < 0 ? -out[i] : out[i];
            if (v > chordPeak) chordPeak = v;
        }
    }
    CHECK(chordPeak > peak, "chord is louder than one note (%ld > %ld)", chordPeak, peak);

    api->on_midi(inst, note_off, 3, 0);
    const uint8_t all_off[] = { 0xB0, 123, 0 };
    api->on_midi(inst, all_off, 3, 0);
    long tail = 0;
    for (int b = 0; b < 600; b++) {                     /* ~1.7 s of release */
        api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
        if (b > 500) {
            for (size_t i = 0; i < MOVE_FRAMES_PER_BLOCK * 2; i++)
                tail += out[i] < 0 ? -out[i] : out[i];
        }
    }
    CHECK(tail == 0, "all-notes-off decays to true silence (tail acc=%ld)", tail);

    /* filter audibly filters: closed lowpass must be quieter than open */
    api->set_param(inst, "flt_type", "LP 24");          /* enum tests left BP 12 */
    api->set_param(inst, "flt_freq", "127");
    api->on_midi(inst, note_on, 3, 0);
    long openPeak = 0;
    for (int b = 0; b < 120; b++) {
        api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
        for (size_t i = 0; i < MOVE_FRAMES_PER_BLOCK * 2; i++) {
            long v = out[i] < 0 ? -out[i] : out[i];
            if (v > openPeak) openPeak = v;
        }
    }
    api->set_param(inst, "flt_freq", "20");             /* slam it shut */
    for (int b = 0; b < 30; b++)                        /* let the SVF settle */
        api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
    long closedPeak = 0;
    for (int b = 0; b < 120; b++) {
        api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
        for (size_t i = 0; i < MOVE_FRAMES_PER_BLOCK * 2; i++) {
            long v = out[i] < 0 ? -out[i] : out[i];
            if (v > closedPeak) closedPeak = v;
        }
    }
    CHECK(closedPeak < openPeak / 3,
          "closing the filter quiets the note (%ld -> %ld)", openPeak, closedPeak);
    api->on_midi(inst, all_off, 3, 0);
    api->set_param(inst, "flt_freq", "127");

    /* ---- ui_hierarchy: the Shadow UI needs this to render ANYTHING ---- */
    n = api->get_param(inst, "ui_hierarchy", big, sizeof big);
    CHECK(n > 1000 && strstr(big, "\"levels\"") && strstr(big, "\"root\""),
          "ui_hierarchy served (%d bytes)", n);
    CHECK(strstr(big, "\"lfo3\"") && strstr(big, "\"global\"") &&
          strstr(big, "\"mod78\""), "hierarchy has all section levels");
    CHECK(strstr(big, "\"filepath\"") && strstr(big, "\"live_preview\":true"),
          "hierarchy exposes the wavetable file browser");

    /* ---- filepath selection + mid-note switching ---- */
    n = api->get_param(inst, "chain_params", big, sizeof big);
    CHECK(n > 1000 && strstr(big, "\"filepath\"") &&
          strstr(big, "/data/UserData/UserLibrary/Wavetables"),
          "chain_params exposes filepath browser (%d bytes)", n);

    #define WTDIR "/data/UserData/UserLibrary/Wavetables/"
    api->set_param(inst, "wt1_table", WTDIR "Adventure Kid/AKWP 0001.wt2048");
    api->get_param(inst, "wt1_table", buf, sizeof buf);
    CHECK(strstr(buf, "AKWP 0001.wt2048") != nullptr,
          "wt1 select by path -> \"%s\"", buf);

    api->on_midi(inst, note_on, 3, 0);
    struct timespec ts = { 0, 50 * 1000000 };
    long swPeak = 0, swMin = 1 << 30;
    const char *cycle[4] = { WTDIR "Neu KatalYst/NK - ACTIVE.wt2048", "",
                             WTDIR "Adventure Kid/AKWP 0042.wt2048",
                             WTDIR "Neu KatalYst/NK - AGE.wt2048" };
    for (int s = 0; s < 4; s++) {                       /* switch WHILE held */
        api->set_param(inst, "wt1_table", cycle[s]);
        for (int b = 0; b < 60; b++) {                  /* ~175 ms per table */
            api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
            long p = 0;
            for (size_t i = 0; i < MOVE_FRAMES_PER_BLOCK * 2; i++) {
                long v = out[i] < 0 ? -out[i] : out[i];
                if (v > p) p = v;
            }
            if (p > swPeak) swPeak = p;
            if (b > 20 && p < swMin) swMin = p;         /* after load settles */
        }
        nanosleep(&ts, nullptr);
    }
    CHECK(swPeak > 1500 && swMin > 200,
          "sound continuous across 4 mid-note table switches (peak %ld, min %ld)",
          swPeak, swMin);
    api->get_param(inst, "wt1_table", buf, sizeof buf);
    CHECK(strstr(buf, "NK - AGE.wt2048") != nullptr, "landed on \"%s\"", buf);

    /* state stores the table PATH and restores it */
    n = api->get_param(inst, "state", big, sizeof big);
    CHECK(strstr(big, "wt1_table=" WTDIR "Neu KatalYst/NK - AGE.wt2048;") != nullptr,
          "state carries table path");
    memcpy(saved, big, sizeof big);
    api->set_param(inst, "wt1_table", "");
    api->set_param(inst, "synth:state", saved);
    api->get_param(inst, "wt1_table", buf, sizeof buf);
    CHECK(strstr(buf, "NK - AGE.wt2048") != nullptr, "state restores table path");

    api->on_midi(inst, all_off, 3, 0);
    api->set_param(inst, "wt1_table", "");

    api->destroy_instance(inst);
    dlclose(dl);

    printf("\n%s (%d failures)\n", g_fail ? "LOADTEST FAILED" : "LOADTEST PASSED", g_fail);
    return g_fail ? 1 : 0;
}
