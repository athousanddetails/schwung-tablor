/* Phase 2 DSP validation — numeric golden checks for every ported block.
 *
 * The checks assert MEASURED behaviour (frequency response in dB, envelope
 * timing, waveform values), not just "doesn't crash" — the ER-99 lesson.
 * Runs natively inside the build container AND cross-compiled on the Move.
 */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include "../src/ported/filter.h"
#include "../src/ported/adsr.h"
#include "../src/ported/analog_tables.h"
#include "../src/ported/lfo.h"
#include "../src/ported/wt_oscillator.h"
#include "../src/ported/wav.h"

using namespace tb;

static int g_fail = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
    else         {           printf("  ok: " __VA_ARGS__); printf("\n"); } \
} while (0)

static double nowMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1e3 + (double) ts.tv_nsec / 1e6;
}

/* Steady-state gain of `filter` at `hz`: feed a sine, RMS after settling. */
static float filterGainDb(Filter &f, float hz, float sr = 44100.0f)
{
    f.reset();
    const int settle = (int) (sr * 0.2f), measure = (int) (sr * 0.5f);
    std::vector<float> l((size_t) (settle + measure)), r(l.size());
    for (size_t i = 0; i < l.size(); i++)
        l[i] = r[i] = std::sin(2.0 * M_PI * hz * (double) i / sr);
    f.process(l.data(), r.data(), (int) l.size());
    double acc = 0.0;
    for (int i = settle; i < settle + measure; i++)
        acc += (double) l[(size_t) i] * l[(size_t) i];
    double rms = std::sqrt(acc / measure);
    double inRms = 1.0 / std::sqrt(2.0);
    return (float) (20.0 * std::log10(rms / inRms + 1e-12));
}

static void testFilter()
{
    printf("== filter ==\n");
    Filter f;
    f.setSampleRate(44100.0);

    /* LP24 @ 1 kHz: flat passband, ~-6 dB corner (2 cascaded -3 dB stages),
     * -24 dB/oct slope. */
    f.setType(Filter::lowpass);
    f.setSlope(Filter::db24);
    f.setParams(1000.0f, kFilterQ);
    float g100 = filterGainDb(f, 100.0f);
    float g1k  = filterGainDb(f, 1000.0f);
    float g2k  = filterGainDb(f, 2000.0f);
    float g4k  = filterGainDb(f, 4000.0f);
    CHECK(std::fabs(g100) < 0.5f, "LP24 passband flat (100 Hz: %+.2f dB)", g100);
    CHECK(g1k > -7.5f && g1k < -4.5f, "LP24 corner ~-6 dB (1 kHz: %+.2f dB)", g1k);
    float slope = g2k - g4k;
    CHECK(slope > 20.0f && slope < 28.0f, "LP24 slope ~24 dB/oct (2k->4k: %.1f dB)", slope);

    /* LP12: ~-3 dB corner, ~12 dB/oct. */
    f.setSlope(Filter::db12);
    f.setParams(1000.0f, kFilterQ);
    g1k = filterGainDb(f, 1000.0f);
    slope = filterGainDb(f, 2000.0f) - filterGainDb(f, 4000.0f);
    CHECK(g1k > -4.0f && g1k < -2.0f, "LP12 corner ~-3 dB (%+.2f dB)", g1k);
    CHECK(slope > 10.0f && slope < 14.0f, "LP12 slope ~12 dB/oct (%.1f dB)", slope);

    /* Resonance: the voice's q law, res=100% -> q = Q/(1-0.99) — expect a
     * tall peak at the corner. */
    f.setParams(1000.0f, kFilterQ / (1.0f - 0.99f * 0.9f));
    float peak = filterGainDb(f, 1000.0f);
    CHECK(peak > 6.0f, "resonance peaks at corner (%+.1f dB at 90%% res)", peak);

    /* HP24 mirrors LP24. */
    f.setType(Filter::highpass);
    f.setSlope(Filter::db24);
    f.setParams(1000.0f, kFilterQ);
    float hLo = filterGainDb(f, 250.0f), hHi = filterGainDb(f, 4000.0f);
    CHECK(hLo < -40.0f, "HP24 rejects below corner (250 Hz: %+.1f dB)", hLo);
    CHECK(std::fabs(hHi) < 1.0f, "HP24 passes above corner (4 kHz: %+.2f dB)", hHi);

    /* Bandpass peaks at center; notch nulls it. */
    f.setType(Filter::bandpass);
    f.setSlope(Filter::db12);
    f.setParams(1000.0f, kFilterQ);
    float bC = filterGainDb(f, 1000.0f), bLo = filterGainDb(f, 125.0f);
    CHECK(bC > -1.5f && bLo < -15.0f, "BP peaks at center (1k %+.1f, 125 %+.1f dB)", bC, bLo);

    f.setType(Filter::notch);
    f.setParams(1000.0f, kFilterQ);
    float nC = filterGainDb(f, 1000.0f), nLo = filterGainDb(f, 125.0f);
    CHECK(nC < -25.0f && std::fabs(nLo) < 1.0f, "notch nulls center (1k %+.1f, 125 %+.1f dB)", nC, nLo);
}

static void testADSR()
{
    printf("== adsr ==\n");
    AnalogADSR env;
    env.setSampleRate(44100.0);
    env.setAttack(0.1f);
    env.setDecay(0.2f);
    env.setSustainLevel(0.5f);
    env.setRelease(0.15f);

    env.noteOn();
    /* attack: reach ~1.0 around the set time (analog curve hits 1.0 at t_a) */
    int i = 0;
    while (env.getState() == AnalogADSR::State::attack && i < 44100) { env.process(); i++; }
    float tA = (float) i / 44100.0f;
    CHECK(tA > 0.07f && tA < 0.13f, "attack reaches peak in ~0.1 s (%.3f s)", tA);
    CHECK(env.getOutput() >= 0.999f, "peak is 1.0 (%.4f)", env.getOutput());

    /* decay to sustain */
    i = 0;
    while (env.getState() == AnalogADSR::State::decay && i < 44100) { env.process(); i++; }
    float tD = (float) i / 44100.0f;
    CHECK(tD > 0.1f && tD < 0.3f, "decay lands near 0.2 s (%.3f s)", tD);
    CHECK(std::fabs(env.getOutput() - 0.5f) < 0.02f, "sustain level 0.5 (%.3f)", env.getOutput());

    /* sustain holds */
    env.process(4410);
    CHECK(std::fabs(env.getOutput() - 0.5f) < 0.02f, "sustain holds (%.3f)", env.getOutput());

    /* release to idle */
    env.noteOff();
    i = 0;
    while (env.getState() == AnalogADSR::State::release && i < 44100) { env.process(); i++; }
    float tR = (float) i / 44100.0f;
    CHECK(tR > 0.05f && tR < 0.25f, "release lands near 0.15 s (%.3f s)", tR);
    CHECK(env.getState() == AnalogADSR::State::idle && env.getOutput() == 0.0f,
          "ends idle at 0");

    /* attack==0 fast path */
    AnalogADSR fast;
    fast.setSampleRate(44100.0);
    fast.setAttack(0.0f);
    fast.noteOn();
    CHECK(fast.getOutput() == 1.0f && fast.getState() == AnalogADSR::State::decay,
          "attack=0 jumps to peak");

    /* multiply path actually multiplies */
    AnalogADSR mul;
    mul.setSampleRate(44100.0);
    mul.setAttack(0.5f);
    mul.noteOn();
    float l[64], r[64];
    for (auto &v : l) v = 1.0f;
    for (auto &v : r) v = 1.0f;
    mul.processMultiplying(l, r, 64);
    CHECK(l[63] > l[0] && l[63] < 0.05f, "VCA multiply follows early attack (%.4f)", l[63]);
}

static void testLFO()
{
    printf("== lfo ==\n");
    LFO lfo;
    lfo.setSampleRate(44100.0);

    LFO::Parameters p;
    p.waveShape = LFO::WaveShape::sine;
    p.frequency = 2.0f;
    p.depth = 1.0f;
    lfo.setParameters(p);
    lfo.reset();
    lfo.noteOn(0.0f);

    /* quarter period of 2 Hz = 0.125 s = 5512.5 samples -> sin ~ 1 */
    lfo.process(5513);
    CHECK(lfo.getOutput() > 0.99f, "sine peaks at quarter period (%.3f)", lfo.getOutput());
    lfo.process(5512);
    CHECK(std::fabs(lfo.getOutput()) < 0.01f, "sine zero at half period (%.3f)", lfo.getOutput());

    /* sawUp ramps -1 -> +1 */
    p.waveShape = LFO::WaveShape::sawUp;
    lfo.setParameters(p);
    lfo.reset(); lfo.noteOn(0.0f);
    CHECK(std::fabs(lfo.getOutput() - (-1.0f)) < 0.01f, "sawUp starts at -1 (%.3f)", lfo.getOutput());
    lfo.process(11025);   /* half period */
    CHECK(std::fabs(lfo.getOutput()) < 0.01f, "sawUp mid at 0 (%.3f)", lfo.getOutput());

    /* depth + offset + clamp */
    p.waveShape = LFO::WaveShape::square;
    p.depth = 0.5f; p.offset = 0.75f;
    lfo.setParameters(p);
    lfo.reset(); lfo.noteOn(0.0f);
    CHECK(lfo.getOutput() == 1.0f, "clamped at +1 (0.75 + 0.5)");
    CHECK(std::fabs(lfo.getOutputUnclamped() - 1.25f) < 1e-5f,
          "unclamped 1.25 (%.3f)", lfo.getOutputUnclamped());

    /* fade-in */
    p.waveShape = LFO::WaveShape::square;
    p.depth = 1.0f; p.offset = 0.0f; p.fade = 0.1f;
    lfo.setParameters(p);
    lfo.reset(); lfo.noteOn(0.0f);
    lfo.process(2205);    /* half the fade */
    float mid = std::fabs(lfo.getOutput());
    CHECK(mid > 0.3f && mid < 0.7f, "fade halfway (%.2f)", mid);

    /* delay holds output at 0 */
    p.fade = 0.0f; p.delay = 0.1f;
    lfo.setParameters(p);
    lfo.reset(); lfo.noteOn(0.0f);
    lfo.process(2205);
    CHECK(lfo.getOutput() == 0.0f, "silent during delay");

    /* S&H deterministic + bounded */
    p.delay = 0.0f;
    p.waveShape = LFO::WaveShape::sampleAndHold;
    LFO a, b;
    a.setSampleRate(44100.0); b.setSampleRate(44100.0);
    a.setParameters(p); b.setParameters(p);
    a.noteOn(3.0f); b.noteOn(3.0f);
    bool same = true, bounded = true;
    for (int i = 0; i < 50; i++) {
        float va = a.process(500), vb = b.process(500);
        same &= (va == vb);
        bounded &= (va >= -1.0f && va <= 1.0f);
    }
    CHECK(same && bounded, "S&H deterministic and bounded");
}

/* Sample & Hold must STEP, and hold. It was reported as sounding like a saw;
 * the cause turned out to be the LFO param wiring in voice.h (depth read the
 * sync switch), but the shape itself is worth pinning so a future edit to the
 * phase handling cannot quietly turn it into a ramp -- which is exactly what
 * the neighbouring `noise` shape is, since that one lerps between its random
 * points. */
static void testLfoSampleAndHold()
{
    printf("== lfo sample & hold ==\n");
    const int PERIOD = 22050;              /* 2 Hz at 44100 */
    LFO lfo;
    lfo.setSampleRate(44100.0);
    LFO::Parameters p;
    p.waveShape = LFO::WaveShape::sampleAndHold;
    p.frequency = 2.0f;
    p.depth = 1.0f;
    lfo.setParameters(p);
    lfo.reset();
    lfo.noteOn(0.0f);

    /* Sample 8 times inside ONE period: a hold means all 8 are identical. */
    float first = lfo.getOutput(), maxDev = 0.0f;
    for (int i = 0; i < 8; i++) {
        lfo.process(PERIOD / 16);
        maxDev = std::max(maxDev, std::fabs(lfo.getOutput() - first));
    }
    CHECK(maxDev < 0.0001f, "S&H holds its value within a period (drift %.5f)", maxDev);

    /* Across several periods it must actually take different values, and jump
     * rather than ramp: a saw would creep by ~2/period in one direction. */
    float vals[6];
    for (int i = 0; i < 6; i++) { lfo.process(PERIOD); vals[i] = lfo.getOutput(); }
    int distinct = 0, monotonic = 0;
    for (int i = 0; i < 6; i++) {
        bool seen = false;
        for (int j = 0; j < i; j++) if (std::fabs(vals[i] - vals[j]) < 0.0001f) seen = true;
        if (!seen) distinct++;
        if (i && vals[i] > vals[i - 1]) monotonic++;
    }
    CHECK(distinct >= 4, "S&H picks fresh values each period (%d distinct of 6)", distinct);
    CHECK(monotonic < 5, "S&H is not a ramp (%d of 5 steps rose)", monotonic);
}

/* Short and odd-length wavetables must PLAY, and play at the right pitch.
 *
 * Reported as "shorter wavetable lengths aren't playing" and "the playback is
 * kind of buzzy - makes me wonder if there is a preferential length". Both came
 * from frame-size inference that stopped at 256: a short cycle inferred
 * nothing and the 2048 fallback made nFrames 0 (silent), and a file whose
 * LENGTH divided by 2048 loaded with each frame spanning many real cycles
 * (buzz). Adventure Kid's own AKWF format is 600 samples per cycle and hit the
 * first case exactly. */
static void testShortWavetables()
{
    printf("== short wavetables ==\n");

    /* the inference itself */
    WavData w;
    w.sampleRate = 44100.0f;
    w.samples.assign(600, 0.0f);
    CHECK(wavInferFrameSize(w) == 0,
          "a 600-sample AKWF cycle fits no power-of-two frame (got %d)",
          wavInferFrameSize(w));
    w.samples.assign(128, 0.0f);
    CHECK(wavInferFrameSize(w) == 128, "a 128-sample cycle is found (got %d)",
          wavInferFrameSize(w));
    w.samples.assign(4096, 0.0f);
    CHECK(wavInferFrameSize(w) == 2048,
          "4096 samples still reads as two 2048 frames (got %d)",
          wavInferFrameSize(w));

    /* a 600-sample sine cycle, resampled, must survive as a sine */
    std::vector<float> cyc(600);
    for (int i = 0; i < 600; i++)
        cyc[(size_t) i] = std::sin((float) i / 600.0f * 2.0f * (float) M_PI);
    std::vector<float> up = wavResampleFrames(cyc, 1, 600, 2048);
    float worst = 0.0f;
    for (int i = 0; i < 2048; i++) {
        float want = std::sin((float) i / 2048.0f * 2.0f * (float) M_PI);
        worst = std::max(worst, std::fabs(up[(size_t) i] - want));
    }
    CHECK(up.size() == 2048 && worst < 0.01f,
          "a 600-sample cycle stretches to 2048 without distorting it (worst %.4f)",
          worst);

    /* and it must build and play at the right pitch */
    Wavetable wt;
    bool built = wtBuild(wt, up.data(), 1, 2048, 44100.0f, 44100.0f);
    CHECK(built && wt.size() == 1, "the stretched cycle builds a table");

    if (built) {
        WTOscillator osc;
        osc.setSampleRate(44100.0);
        osc.setWavetable(&wt);
        WTOscillator::Params op;
        op.position = 0.0f; op.leftGain = 1.0f; op.rightGain = 1.0f;
        float l[8192] = { 0 }, r[8192] = { 0 };
        osc.processAdding(69.0f, op, l, r, 8192);   /* A4 = 440 Hz */
        int crossings = 0;
        for (int i = 1; i < 8192; i++)
            if (l[i - 1] <= 0.0f && l[i] > 0.0f) crossings++;
        float hz = (float) crossings * 44100.0f / 8192.0f;
        CHECK(hz > 420.0f && hz < 460.0f,
              "it sounds at the note it was asked for (%.0f Hz for A4)", hz);
    }
}

static void testAnalogTables()
{
    printf("== analog tables ==\n");
    double t0 = nowMs();
    AnalogTables tables(44100.0);
    double buildMs = nowMs() - t0;
    printf("  build: %.0f ms, %.2f MB\n", buildMs, tables.bytes() / 1e6);
    CHECK(buildMs < 250.0, "analog tables build fast (%.0f ms; was 2743 additive)", buildMs);
    CHECK(tables.bytes() < 2e6, "analog tables decimated (%.2f MB)", tables.bytes() / 1e6);

    /* Spectrum-built tables must equal the additive formula wherever the
     * table can hold every harmonic. Note 60's level is built for top note
     * 60.5 (311 Hz, ~70 harmonics — well under the table Nyquist). */
    float maxErr = 0.0f;
    const float refNote = 60.5f, refHz = midiNoteToHz(60.5f);
    for (int i = 0; i < 64; i++) {
        float ph = (float) i / 64.0f;
        float gotT = tables.processTriangle(60.0f, ph);
        float wantT = waves::triangle(ph, refHz, 44100.0f);
        float gotS = tables.processSawUp(60.0f, ph);
        float wantS = waves::sawUp(ph, refHz, 44100.0f);
        maxErr = std::max({ maxErr, std::fabs(gotT - wantT), std::fabs(gotS - wantS) });
    }
    (void) refNote;
    CHECK(maxErr < 0.01f, "FFT-built tables match additive formula (err %.4f)", maxErr);

    /* Square via saws ~ +/-1; pulse pw=0.5 has ~0 DC. */
    float sq = tables.processSquare(60.0f, 0.1f);
    CHECK(sq > 0.7f && sq < 1.3f, "square high half ~ +1 (%.2f)", sq);
    double dc = 0.0;
    for (int i = 0; i < 512; i++)
        dc += tables.processPulse(60.0f, (float) i / 512.0f, 0.5f);
    CHECK(std::fabs(dc / 512.0) < 0.02, "pulse pw=0.5 DC ~ 0 (%.4f)", dc / 512.0);

    /* Noise: white RMS ~ 0.1 (gin's stddev), pink bounded. */
    WhiteNoise wn;
    PinkNoise pn;
    double wAcc = 0.0; float pPeak = 0.0f;
    for (int i = 0; i < 44100; i++) {
        float w = wn.nextSample(); wAcc += (double) w * w;
        pPeak = std::max(pPeak, std::fabs(pn.nextSample()));
    }
    float wRms = (float) std::sqrt(wAcc / 44100.0);
    CHECK(wRms > 0.08f && wRms < 0.12f, "white noise RMS ~ 0.1 (%.3f)", wRms);
    CHECK(pPeak > 0.05f && pPeak < 1.5f, "pink noise bounded (peak %.2f)", pPeak);

    /* Sub oscillator renders sanely. */
    StereoOscillator sub(tables);
    sub.setSampleRate(44100.0);
    sub.noteOn(0.0f);
    StereoOscillator::Params sp;
    sp.wave = Wave::square;
    float l[512] = {0}, r[512] = {0};
    sub.processAdding(48.0f, sp, l, r, 512);
    float pk = 0.0f; bool finite = true;
    for (auto v : l) { pk = std::max(pk, std::fabs(v)); finite &= std::isfinite(v); }
    CHECK(finite && pk > 0.5f && pk < 2.0f, "sub osc renders square (peak %.2f)", pk);
}

int main()
{
    testFilter();
    testADSR();
    testLFO();
    testLfoSampleAndHold();
    testShortWavetables();
    testAnalogTables();
    printf("\n%s (%d failures)\n", g_fail ? "DSP TESTS FAILED" : "DSP TESTS PASSED", g_fail);
    return g_fail ? 1 : 0;
}
