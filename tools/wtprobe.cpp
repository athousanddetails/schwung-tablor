/* wtprobe — does selecting a wavetable actually CHANGE the sound?
 * (The loadtest verified continuity across switches, which a silently
 * failing loader also passes. This one compares waveforms.)
 * Run on the Move in the module dir: ./tablor_wtprobe ./dsp.so
 */
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <dlfcn.h>

#include "../src/host/plugin_api_v1.h"

static void hlog(const char *m) { printf("  [log] %s\n", m); }

static double renderFingerprint(plugin_api_v2_t *api, void *inst, double *outRms)
{
    /* strike a fresh note, settle, then fingerprint one second of audio:
     * normalized mean |derivative| — a cheap brightness measure that moves
     * when the waveform changes shape. */
    const uint8_t off[] = { 0xB0, 123, 0 };
    api->on_midi(inst, off, 3, 0);
    int16_t out[256];
    for (int b = 0; b < 400; b++) api->render_block(inst, out, 128);   /* tail out */

    const uint8_t on[] = { 0x90, 48, 100 };
    api->on_midi(inst, on, 3, 0);
    for (int b = 0; b < 100; b++) api->render_block(inst, out, 128);   /* settle */

    double d = 0, e = 0;
    int16_t prev = 0;
    for (int b = 0; b < 344; b++) {
        api->render_block(inst, out, 128);
        for (int i = 0; i < 256; i += 2) {                             /* L only */
            d += std::fabs((double) out[i] - prev);
            e += (double) out[i] * out[i];
            prev = out[i];
        }
    }
    api->on_midi(inst, off, 3, 0);
    *outRms = std::sqrt(e / (344.0 * 128.0));
    return e > 0 ? d / std::sqrt(e) : 0.0;
}

int main(int argc, char **argv)
{
    void *dl = dlopen(argc > 1 ? argv[1] : "./dsp.so", RTLD_NOW | RTLD_LOCAL);
    if (!dl) { printf("dlopen: %s\n", dlerror()); return 1; }
    auto init = (move_plugin_init_v2_fn) dlsym(dl, MOVE_PLUGIN_INIT_V2_SYMBOL);
    host_api_v1_t host = {};
    host.api_version = 1; host.sample_rate = 44100;
    host.frames_per_block = 128; host.log = hlog;
    plugin_api_v2_t *api = init(&host);
    void *inst = api->create_instance(".", nullptr);

    char buf[512];
    api->get_param(inst, "wt1_table", buf, sizeof buf);
    printf("start table: %s\n", buf);

    double rms0, fp0 = renderFingerprint(api, inst, &rms0);
    printf("Init:        fingerprint %.4f  rms %.0f\n", fp0, rms0);

    const char *targets[] = {
        "/data/UserData/UserLibrary/Wavetables/Adventure Kid/AKWP 0001.wt2048",
        "/data/UserData/UserLibrary/Wavetables/Neu KatalYst/NK - ACTIVE.wt2048" };
    for (const char *t : targets) {
        api->set_param(inst, "wt1_table", t);
        api->get_param(inst, "wt1_table", buf, sizeof buf);
        printf("selected:    %s (param now \"%s\")\n", t, buf);

        struct timespec ts = { 2, 0 };                 /* let the loader finish */
        nanosleep(&ts, nullptr);

        double rms, fp = renderFingerprint(api, inst, &rms);
        double delta = std::fabs(fp - fp0) / (fp0 > 0 ? fp0 : 1);
        printf("  -> fingerprint %.4f  rms %.0f  (delta vs Init %.1f%%)  %s\n",
               fp, rms, delta * 100.0,
               delta > 0.05 ? "SOUND CHANGED" : "!! SOUND DID NOT CHANGE");
    }

    api->destroy_instance(inst);
    return 0;
}
