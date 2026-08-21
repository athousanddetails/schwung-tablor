/* Stuck-note hunt.
 *
 * Reported on hardware: playing notes while changing wavetables leaves a
 * note sounding after the pad is released; playing more notes clears it
 * (which is voice STEALING, not a release — the giveaway that a note-off
 * went missing rather than a release simply being long).
 *
 * This drives the module the way a pair of hands does: press a chord, turn
 * the wavetable pot mid-note, release the chord, then listen. Any energy
 * left after a generous release window is a stuck note, and the sequence
 * that produced it is printed so it can be replayed.
 *
 * Deterministic (an LCG, no rand()) so a hit is reproducible.
 *
 *   ./tablor_stucknote ./dsp.so [iterations]
 */
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <dlfcn.h>
#include <ctime>

#include "../src/host/plugin_api_v1.h"

static void host_log(const char *) {}

static uint32_t rngState = 0x13579bdf;
static uint32_t rnd() { rngState = rngState * 1664525u + 1013904223u; return rngState >> 8; }

static plugin_api_v2_t *api;
static void *inst;
static int16_t out[MOVE_FRAMES_PER_BLOCK * 2];

/* render n blocks, return the peak sample seen */
static long renderBlocks(int blocks)
{
    long peak = 0;
    for (int b = 0; b < blocks; b++) {
        api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
        for (size_t i = 0; i < MOVE_FRAMES_PER_BLOCK * 2; i++) {
            long v = out[i] < 0 ? -out[i] : out[i];
            if (v > peak) peak = v;
        }
    }
    return peak;
}

int main(int argc, char **argv)
{
    const char *so_path = (argc > 1) ? argv[1] : "./dsp.so";
    int iters = (argc > 2) ? atoi(argv[2]) : 200;
    if (argc > 3) rngState = (uint32_t) strtoul(argv[3], nullptr, 0);

    void *dl = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (!dl) { printf("dlopen: %s\n", dlerror()); return 1; }
    auto init = (move_plugin_init_v2_fn) dlsym(dl, MOVE_PLUGIN_INIT_V2_SYMBOL);
    host_api_v1_t host = {};
    host.api_version = 1;
    host.sample_rate = MOVE_SAMPLE_RATE;
    host.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    host.log = host_log;
    api = init(&host);
    inst = api->create_instance(".", nullptr);
    if (!inst) { printf("create_instance failed\n"); return 1; }

    { char rb[8] = {}; struct timespec ts = { 0, 10 * 1000000 };
      for (int i = 0; i < 1500; i++) {
          api->get_param(inst, "ready", rb, sizeof rb);
          if (rb[0] == '1') break;
          nanosleep(&ts, nullptr);
      } }

    /* a short release so a legitimate tail cannot be mistaken for a stuck
     * note: 100 ms of release, then we listen for two full seconds */
    api->set_param(inst, "vca_r", "5");
    api->set_param(inst, "vca_s", "100");

    int stuck = 0;
    for (int it = 0; it < iters && stuck < 5; it++) {
        int held[4], nHeld = (int) (rnd() % 4) + 1;
        for (int i = 0; i < nHeld; i++) {
            held[i] = 48 + (int) (rnd() % 25);
            uint8_t on[3] = { 0x90, (uint8_t) held[i], 100 };
            api->on_midi(inst, on, 3, 0);
            renderBlocks((int) (rnd() % 6) + 1);
        }

        /* the hands on the encoders, mid-note. Everything reachable while a
         * pad is held, not just the wavetable pot: the report was "changing
         * wavetables", but a preset recall or a poly->mono flip lands on the
         * same voices and is one encoder away on the same box. */
        char v[16];
        int action = (int) (rnd() % 7);
        int arg = 0;
        switch (action) {
        case 0: case 1:
            arg = (int) (rnd() % 40);
            snprintf(v, sizeof v, "%d", arg);
            api->set_param(inst, action ? "wt2_select" : "wt1_select", v);
            break;
        case 2:
            arg = (int) (rnd() % 3);
            snprintf(v, sizeof v, "%d", arg);
            api->set_param(inst, "wt_pack", v);
            break;
        case 3:                                    /* the file browser path */
            arg = (int) (rnd() % 40);
            snprintf(v, sizeof v, "%d", arg);
            api->set_param(inst, "wt1_select", v);
            api->get_param(inst, "wt1_table", v, sizeof v);
            break;
        case 4:                                    /* poly <-> mono */
            arg = (int) (rnd() % 2);
            api->set_param(inst, "voice_mode", arg ? "Mono" : "Poly");
            break;
        case 5:                                    /* max voices */
            arg = (int) (rnd() % 8) + 1;
            snprintf(v, sizeof v, "%d", arg);
            api->set_param(inst, "voices", v);
            break;
        case 6:                                    /* preset recall */
            arg = (int) (rnd() % 8);
            snprintf(v, sizeof v, "%d", arg);
            api->set_param(inst, "preset", v);
            api->set_param(inst, "vca_r", "5");    /* keep the tail short */
            api->set_param(inst, "vca_s", "100");
            break;
        }
        renderBlocks((int) (rnd() % 20) + 2);

        for (int i = 0; i < nHeld; i++) {
            uint8_t off[3] = { 0x80, (uint8_t) held[i], 0 };
            api->on_midi(inst, off, 3, 0);
        }

        renderBlocks(200);                    /* ~580 ms: release is 100 ms */
        long tail = renderBlocks(500);        /* ~1.45 s of pure listening  */

        if (tail > 40) {
            stuck++;
            printf("STUCK it=%d action=%d arg=%d notes=", it, action, arg);
            for (int i = 0; i < nHeld; i++) printf("%d ", held[i]);
            printf("| tail peak %ld\n", tail);
            uint8_t panic[3] = { 0xB0, 123, 0 };
            api->on_midi(inst, panic, 3, 0);
            renderBlocks(50);
        }
    }

    printf(stuck ? "\nSTUCK NOTES: %d\n" : "\nclean: no stuck notes in %d iterations\n",
           stuck ? stuck : iters);
    api->destroy_instance(inst);
    return stuck ? 1 : 0;
}
