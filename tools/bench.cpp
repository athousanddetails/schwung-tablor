/* Tablor phase 1 benchmark — THE go/no-go. Run ON the Move:
 *
 *   taskset 0x7 ./tablor_bench [wavetable.wav]
 *
 * 1. VALIDATES the decimated mip build: a pure-sine frame must reconstruct
 *    as a sine at every level; a saw frame must lose harmonics as levels rise.
 * 2. Measures wavetable build time + memory for a real 2048-frame-size table.
 * 3. Measures realtime factor: 8 voices x 2 oscillators, unison 1/2/4/8,
 *    simple and phase-distorted (bend/formant) paths.
 *
 * The audio budget: 128 frames @ 44100 = 2.90 ms per block.
 */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include "../src/ported/wavetable.h"
#include "../src/ported/wt_oscillator.h"
#include "../src/ported/wav.h"

using namespace tb;

static double nowMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1e3 + (double) ts.tv_nsec / 1e6;
}

static long rssKb()
{
    FILE *f = std::fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long kb = -1;
    while (std::fgets(line, sizeof line, f))
        if (!std::strncmp(line, "VmRSS:", 6)) { std::sscanf(line + 6, "%ld", &kb); break; }
    std::fclose(f);
    return kb;
}

static int g_fail = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
    else         {           printf("  ok: " __VA_ARGS__); printf("\n"); } \
} while (0)

/* ------------------------------------------------------------------ */
/* 1. Validation                                                       */
/* ------------------------------------------------------------------ */

static void validate()
{
    printf("== validation ==\n");
    const int FS = 2048;

    /* Pure sine: must survive every mip level nearly exactly. */
    std::vector<float> sine((size_t) FS);
    for (int i = 0; i < FS; i++)
        sine[(size_t) i] = std::sin(2.0 * M_PI * i / FS);

    Wavetable wtSine;
    WtBuildStats st;
    bool okb = wtBuild(wtSine, sine.data(), 1, FS, 44100.0f, 44100.0f, 6, &st);
    CHECK(okb, "sine table builds (%d levels)", st.levelsPerFrame);
    CHECK(st.levelsPerFrame == 21, "21 mip levels at notesPerTable=6 (got %d)",
          st.levelsPerFrame);

    float maxErr = 0.0f;
    const FrameTable &ft = wtSine.frames[0];
    for (int lvl = 0; lvl < (int) ft.levels.size(); lvl++) {
        const MipLevel &m = ft.levels[(size_t) lvl];
        for (int i = 0; i < m.size; i++) {
            float expect = (float) std::sin(2.0 * M_PI * i / m.size);
            float err = std::fabs(m.data[(size_t) i] - expect);
            if (err > maxErr) maxErr = err;
        }
    }
    CHECK(maxErr < 1e-3f, "sine reconstructs at every level (max err %.2e)", maxErr);

    /* Saw: level sizes must decimate, and high levels must be band-limited. */
    std::vector<float> saw((size_t) FS);
    for (int i = 0; i < FS; i++)
        saw[(size_t) i] = 2.0f * i / FS - 1.0f;

    Wavetable wtSaw;
    wtBuild(wtSaw, saw.data(), 1, FS, 44100.0f, 44100.0f, 6, &st);
    const FrameTable &fs = wtSaw.frames[0];
    int s0 = fs.levels[0].size, sTop = fs.levels[fs.levels.size() - 1].size;
    CHECK(s0 == 2048 && sTop == 32, "decimation: level0=%d top=%d", s0, sTop);

    /* Peak of top level must be well below the raw saw's 1.0 (few harmonics). */
    float peak = 0.0f;
    for (auto v : fs.levels[fs.levels.size() - 1].data)
        peak = std::max(peak, std::fabs(v));
    CHECK(peak < 0.9f && peak > 0.2f, "top level band-limited (peak %.2f)", peak);

    /* Oscillator renders without NaN and with sane amplitude. */
    WTOscillator osc;
    osc.setSampleRate(44100.0);
    osc.setWavetable(&wtSaw);
    osc.noteOn(0.0f);
    WTOscillator::Params p;
    std::vector<float> L(1024, 0.0f), R(1024, 0.0f);
    osc.processAdding(60.0f, p, L.data(), R.data(), 1024);
    float pk = 0.0f; bool finite = true;
    for (auto v : L) { pk = std::max(pk, std::fabs(v)); finite &= std::isfinite(v); }
    CHECK(finite && pk > 0.3f && pk < 2.0f, "osc renders note 60 (peak %.2f)", pk);
}

/* ------------------------------------------------------------------ */
/* 2 + 3. The measurements                                             */
/* ------------------------------------------------------------------ */

struct VoiceSim {
    VoicedWTOscillator osc1 { 8 }, osc2 { 8 };
    float note = 60.0f;
};

int main(int argc, char **argv)
{
    validate();

    /* ---- table: real file if given, else a synthetic 256-frame morph ---- */
    std::vector<float> tableData;
    int frameSize = 2048, nFrames = 0;
    float fileRate = 44100.0f;
    const char *src = "synthetic";

    if (argc > 1) {
        WavData w;
        if (!wavLoad(argv[1], w)) { printf("FAIL: cannot read %s\n", argv[1]); return 1; }
        frameSize = wavInferFrameSize(w);
        if (!frameSize) { printf("FAIL: cannot infer frame size (%zu samples)\n",
                                 w.samples.size()); return 1; }
        nFrames = (int) w.samples.size() / frameSize;
        tableData = std::move(w.samples);
        tableData.resize((size_t) nFrames * (size_t) frameSize);
        fileRate = w.sampleRate;
        src = argv[1];
        printf("\n== table: %s ==\n  %d frames x %d samples @ %.0f Hz (clm %d)\n",
               src, nFrames, frameSize, fileRate, w.clmFrameSize);
    } else {
        nFrames = 256;                       /* Serum-sized worst case */
        tableData.resize((size_t) nFrames * (size_t) frameSize);
        for (int fi = 0; fi < nFrames; fi++) {
            float morph = (float) fi / (nFrames - 1);
            for (int i = 0; i < frameSize; i++) {
                double ph = 2.0 * M_PI * i / frameSize;
                double v = 0.0;
                for (int h = 1; h <= 32; h++)        /* saw-ish, morphing */
                    v += std::sin(ph * h + morph * h) / h;
                tableData[(size_t) fi * frameSize + i] = (float) (v * 0.5);
            }
        }
        printf("\n== table: synthetic 256 x 2048 ==\n");
    }

    long rss0 = rssKb();
    Wavetable wt;
    WtBuildStats st;
    if (!wtBuild(wt, tableData.data(), nFrames, frameSize, 44100.0f, fileRate, 6, &st)) {
        printf("FAIL: build\n");
        return 1;
    }
    long rss1 = rssKb();

    printf("  build: %.0f ms   table bytes: %.1f MB   (per-osc prediction "
           "in PLAN: 12.7 MB for 256 frames)\n",
           st.buildMs, st.bytes / 1e6);
    printf("  RSS: %ld -> %ld MB (delta %.1f MB)\n",
           rss0 / 1024, rss1 / 1024, (rss1 - rss0) / 1024.0);

    /* ---- realtime factor ---- */
    printf("\n== render: 8 voices x 2 osc, 128-frame blocks @ 44100 ==\n");
    printf("  block budget: 2.90 ms; RT factor = audio time / wall time "
           "(>1 = fits, higher = more headroom)\n");

    const int BLOCK = 128;
    const float chord[8] = { 36, 43, 48, 55, 60, 64, 67, 72 };
    float L[BLOCK], R[BLOCK];

    for (int pass = 0; pass < 2; pass++) {
        const bool complexPath = pass == 1;
        printf("  %s path:\n", complexPath ? "bend/formant (worst case)" : "simple");

        for (int uni : { 1, 2, 4, 8 }) {
            VoiceSim voices[8];
            for (int v = 0; v < 8; v++) {
                voices[v].note = chord[v];
                voices[v].osc1.setSampleRate(44100.0);
                voices[v].osc2.setSampleRate(44100.0);
                voices[v].osc1.setWavetable(&wt);
                voices[v].osc2.setWavetable(&wt);
                voices[v].osc1.noteOn(0.0f);
                voices[v].osc2.noteOn(0.25f);
            }

            VoicedWTParams p1, p2;
            p1.voices = p2.voices = uni;
            p1.detune = p2.detune = uni > 1 ? 0.3f : 0.0f;
            p1.spread = p2.spread = uni > 1 ? 0.7f : 0.0f;
            p1.position = 0.3f; p2.position = 0.7f;
            if (complexPath) {
                p1.bend = 0.4f; p1.formant = 0.3f;
                p2.bend = -0.3f; p2.formant = 0.5f;
            }

            const double seconds = 4.0;
            const int blocks = (int) (seconds * 44100.0 / BLOCK);
            /* slow position sweep: forces table switches + crossfade path */
            double t0 = nowMs();
            for (int b = 0; b < blocks; b++) {
                float sweep = 0.5f + 0.5f * (float) std::sin(b * 0.01);
                p1.position = sweep;
                p2.position = 1.0f - sweep;
                std::memset(L, 0, sizeof L);
                std::memset(R, 0, sizeof R);
                for (auto &v : voices) {
                    v.osc1.processAdding(v.note, p1, L, R, BLOCK);
                    v.osc2.processAdding(v.note, p2, L, R, BLOCK);
                }
            }
            double wallMs = nowMs() - t0;
            double audioMs = blocks * (double) BLOCK / 44.1;
            double rt = audioMs / wallMs;
            double blockMs = wallMs / blocks;
            printf("    unison %d: %5.2fx realtime  (%.3f ms/block, %3.0f%% of budget)%s\n",
                   uni, rt, blockMs, 100.0 * blockMs / 2.902,
                   rt < 1.2 ? "  <-- TOO SLOW" : "");
        }
    }

    printf("\n%s (%d validation failures)\n",
           g_fail ? "BENCH INVALID" : "BENCH COMPLETE", g_fail);
    return g_fail ? 1 : 0;
}
