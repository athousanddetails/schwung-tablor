/* Tablor voice — the per-voice signal path, structured after
 * WavetableVoice.cpp in the original (FigBug/Wavetable, BSD-3):
 *
 *   osc1/osc2 (unison WT) ─┬─ preFilter ── Filter ──┐
 *   sub, noise ────────────┴─ postFilter ───────────┴─ VCA ADSR ─ out
 *
 * Per-source pre/post-filter routing, per-voice filter ADSR, 3 per-voice
 * LFOs, glide via noteSmoother. Parameters are read from the engine's pot
 * array each block through param_map.h; the mod matrix contributes offsets.
 */
#pragma once

#include "../ported/wt_oscillator.h"
#include "../ported/analog_tables.h"
#include "../ported/filter.h"
#include "../ported/adsr.h"
#include "../ported/lfo.h"
#include "param_map.h"
#include "modmatrix.h"
#include "params.h"

#include <cstdlib>
#include <cstring>

namespace tb {

inline float velocityToGain(float velocity, float sensitivity)
{
    float v = velocity * sensitivity + 1.0f - sensitivity;
    float g = v * std::pow(25.0f, v) * 0.04f;
    return std::clamp(g, 0.0f, 1.0f);
}

inline constexpr int kBlock = 128;

/* Everything a voice needs from the engine each block. */
struct VoiceContext {
    const float *pots = nullptr;            /* TB_PARAM_COUNT raw values   */
    const ModSlot *modSlots = nullptr;      /* kModSlots                   */
    const Wavetable *table1 = nullptr;
    const Wavetable *table2 = nullptr;
    AnalogTables *analog = nullptr;
    float modWheel = 0.0f;                  /* 0..1  */
    float aftertouch = 0.0f;                /* 0..1  */
    float pitchBendSemis = 0.0f;            /* +/- pb_range already applied */
    float pitchBendNorm = 0.0f;             /* -1..1 for the mod matrix     */
    float bpm = 120.0f;
};

class Voice {
public:
    explicit Voice(AnalogTables &analog) : sub(analog), noise(analog) {}

    void setSampleRate(double sr)
    {
        sampleRate = sr;
        osc1.setSampleRate(sr); osc2.setSampleRate(sr);
        sub.setSampleRate(sr); noise.setSampleRate(sr);
        filter.setSampleRate(sr);
        filterADSR.setSampleRate(sr);
        adsr.setSampleRate(sr);
        for (auto &l : lfos) l.setSampleRate(sr);
        noteSmoother.setSampleRate(sr);
    }

    bool isActive() const  { return active; }
    bool isReleasing() const { return adsr.getState() == AnalogADSR::State::release; }
    int  currentNote() const { return midiNote; }
    uint32_t serialNumber() const { return serial; }
    float envOutput() const { return adsr.getOutput(); }

    void start(const VoiceContext &c, int note, float vel, uint32_t serial_,
               float glideFromNote)
    {
        midiNote = note;
        velocity = vel;
        serial = serial_;
        active = true;
        fastKill = false;
        randomMod = (float) rand() / (float) RAND_MAX * 2.0f - 1.0f;

        osc1.setWavetable(c.table1);
        osc2.setWavetable(c.table2);

        const float *P = c.pots;
        const int glideMode = (int) P[TB_P_GLIDE_MODE];
        glissando  = glideMode == 1;
        portamento = glideMode == 2;
        if (glideFromNote >= 0.0f && (glissando || portamento)) {
            noteSmoother.setTime(potGlideTime(P[TB_P_GLIDE]));
            noteSmoother.setValueUnsmoothed(glideFromNote / 127.0f);
            noteSmoother.setValue((float) note / 127.0f);
        } else {
            noteSmoother.setValueUnsmoothed((float) note / 127.0f);
        }

        filter.reset();
        filterADSR.reset();
        for (auto &l : lfos) l.reset();

        updateParams(c, 0);

        /* oscillator phases: gin's retrig logic */
        for (int o = 0; o < 2; o++) {
            auto &osc = o == 0 ? osc1 : osc2;
            bool retrig = P[o == 0 ? TB_P_WT1_RETRIG : TB_P_WT2_RETRIG] > 0.5f;
            int uni = (int) P[o == 0 ? TB_P_WT1_UNI : TB_P_WT2_UNI];
            float phases[8];
            for (int i = 0; i < 8; i++) {
                if (uni <= 1)
                    phases[i] = retrig ? 0.0f : (float) rand() / (float) RAND_MAX;
                else if (retrig)
                    phases[i] = 1.0f / (float) uni * (float) i;
                else
                    phases[i] = (float) rand() / (float) RAND_MAX;
            }
            osc.noteOn(phases, 8);
        }
        sub.noteOn(0.0f);
        noise.noteOn(0.0f);

        filterADSR.noteOn();
        for (int i = 0; i < 3; i++) {
            bool retrig = P[lfoParam(i, 7)] > 0.5f;   /* lfoN_retrig */
            lfos[i].noteOn(retrig ? -1.0f : (float) rand() / (float) RAND_MAX);
        }

        adsr.reset();
        adsr.noteOn();
    }

    /* Mono-mode retrigger without phase reset. */
    void retrigger(const VoiceContext &c, int note, float vel)
    {
        const float *P = c.pots;
        midiNote = note;
        velocity = vel;
        if (glissando || portamento) {
            noteSmoother.setTime(potGlideTime(P[TB_P_GLIDE]));
            noteSmoother.setValue((float) note / 127.0f);
        } else {
            noteSmoother.setValueUnsmoothed((float) note / 127.0f);
        }
        bool legato = P[TB_P_LEGATO] > 0.5f;
        if (!legato) {
            if (P[TB_P_FLT_RETRIG] > 0.5f) filterADSR.noteOn();
            if (P[TB_P_VCA_RETRIG] > 0.5f) adsr.noteOn();
        }
    }

    void stop(bool allowTail)
    {
        adsr.noteOff();
        filterADSR.noteOff();
        if (!allowTail) {
            active = false;
        }
    }

    void kill() { fastKill = true; adsr.noteOff(); filterADSR.noteOff(); }

    void render(const VoiceContext &c, float *outL, float *outR, int n)
    {
        if (!active) return;
        updateParams(c, n);

        float preL[kBlock] = {}, preR[kBlock] = {};
        float postL[kBlock] = {}, postR[kBlock] = {};

        const float *P = c.pots;

        if (oscGain[0] > 0.0f)
            osc1.processAdding(oscNote[0], vp1,
                               P[TB_P_RT_WT1] > 0.5f ? preL : postL,
                               P[TB_P_RT_WT1] > 0.5f ? preR : postR, n);
        if (oscGain[1] > 0.0f)
            osc2.processAdding(oscNote[1], vp2,
                               P[TB_P_RT_WT2] > 0.5f ? preL : postL,
                               P[TB_P_RT_WT2] > 0.5f ? preR : postR, n);

        bool subPre = P[TB_P_RT_SUBNOISE] > 0.5f;
        if (subParams.leftGain + subParams.rightGain > 0.0f)
            sub.processAdding(subNote, subParams,
                              subPre ? preL : postL, subPre ? preR : postR, n);
        if (noiseParams.leftGain + noiseParams.rightGain > 0.0f)
            noise.processAdding(60.0f, noiseParams,
                                subPre ? preL : postL, subPre ? preR : postR, n);

        /* velocity */
        float vgain = velocityToGain(velocity, ampVelTrack) * ampModGain;
        for (int i = 0; i < n; i++) {
            preL[i] *= vgain;  preR[i] *= vgain;
            postL[i] *= vgain; postR[i] *= vgain;
        }

#ifdef TB_DEBUG_FILTER
        {
            float pre = 0; for (int i = 0; i < n; i++) pre += preL[i] * preL[i];
            filter.process(preL, preR, n);
            float post = 0; for (int i = 0; i < n; i++) post += preL[i] * preL[i];
            static int dbg = 0;
            if (dbg++ % 100 == 0)
                printf("[voice] filt f=%.1f preRMS=%.4f postRMS=%.4f rt1=%.0f gain0=%.2f\n",
                       filter.getFrequency(), std::sqrt(pre / n), std::sqrt(post / n),
                       P[TB_P_RT_WT1], oscGain[0]);
        }
#else
        filter.process(preL, preR, n);
#endif

        for (int i = 0; i < n; i++) {
            postL[i] += preL[i];
            postR[i] += preR[i];
        }

        adsr.processMultiplying(postL, postR, n);

        if (adsr.getState() == AnalogADSR::State::idle)
            active = false;

        for (int i = 0; i < n; i++) {
            outL[i] += postL[i];
            outR[i] += postR[i];
        }

        noteSmoother.process(n);
    }

private:
    static int lfoParam(int lfo, int field)
    {
        /* fields: 0 shape, 1 rate, 2 sync, 3 beat, 4 depth, 5 phase,
         * 6 offset, 7 retrig — contiguous per LFO in params.h */
        static const int base[3] = { TB_P_LFO1_SHAPE, TB_P_LFO2_SHAPE, TB_P_LFO3_SHAPE };
        return base[lfo] + field;
    }

    void updateParams(const VoiceContext &c, int blockSize)
    {
        const float *P = c.pots;

        /* ---- LFOs first (they feed the matrix) ---- */
        static const float kBeats[16] = {
            0.125f, 1.0f/6, 0.25f, 1.0f/3, 0.375f, 0.5f, 2.0f/3, 0.75f,
            1.0f, 4.0f/3, 1.5f, 2.0f, 8.0f/3, 3.0f, 4.0f, 8.0f };

        ModSources src;
        for (int i = 0; i < 3; i++) {
            LFO::Parameters lp;
            lp.waveShape = (LFO::WaveShape) ((int) P[lfoParam(i, 0)] + 1); /* skip none */
            float rate = potLfoRate(P[lfoParam(i, 1)]);
            float rateMod = modOff.o[DST_LFO1_RATE + i];
            if (rateMod != 0.0f)
                rate = std::clamp(rate * std::pow(2.0f, rateMod * 3.0f), 0.01f, 60.0f);
            if (P[lfoParam(i, 2)] > 0.5f) {  /* sync */
                float beats = kBeats[(int) P[lfoParam(i, 3)] & 15];
                rate = c.bpm / 60.0f / beats;
            }
            lp.frequency = rate;
            lp.depth  = pot01(P[lfoParam(i, 4)]);
            lp.phase  = pot01(P[lfoParam(i, 5)]);
            lp.offset = potBipolar(P[lfoParam(i, 6)]);
            lfos[i].setParameters(lp);
            if (blockSize > 0) lfos[i].process(blockSize);
            src.values[SRC_LFO1 + i] = lfos[i].getOutput();
        }

        /* ---- envelopes ---- */
        filterADSR.setAttack(potEnvTime(P[TB_P_FLT_A]));
        filterADSR.setDecay(potEnvTime(P[TB_P_FLT_D]));
        filterADSR.setSustainLevel(pot01(P[TB_P_FLT_S]));
        filterADSR.setRelease(potEnvTime(P[TB_P_FLT_R]));
        if (blockSize > 0) filterADSR.process(blockSize);

        adsr.setAttack(potEnvTime(P[TB_P_VCA_A]));
        adsr.setDecay(potEnvTime(P[TB_P_VCA_D]));
        adsr.setSustainLevel(pot01(P[TB_P_VCA_S]));
        adsr.setRelease(fastKill ? 0.01f : potEnvTime(P[TB_P_VCA_R]));

        /* ---- mod matrix ---- */
        src.values[SRC_FILTER_EG]  = filterADSR.getOutput();
        src.values[SRC_VCA_EG]     = adsr.getOutput();
        src.values[SRC_VELOCITY]   = velocity;
        src.values[SRC_NOTE]       = (float) midiNote / 127.0f;
        src.values[SRC_MODWHEEL]   = c.modWheel;
        src.values[SRC_AFTERTOUCH] = c.aftertouch;
        src.values[SRC_PITCHBEND]  = c.pitchBendNorm;
        src.values[SRC_RANDOM]     = randomMod;
        modOff.compute(c.modSlots, src);

        /* ---- oscillators ---- */
        float baseNote = noteSmoother.getCurrentValue() * 127.0f;
        if (glissando) baseNote = (float) (int) (baseNote + 0.5f);
        baseNote += c.pitchBendSemis;

        for (int o = 0; o < 2; o++) {
            const int kTable  = o == 0 ? TB_P_WT1_TABLE : TB_P_WT2_TABLE;
            const int kPos    = o == 0 ? TB_P_WT1_POS : TB_P_WT2_POS;
            const int kLevel  = o == 0 ? TB_P_WT1_LEVEL : TB_P_WT2_LEVEL;
            const int kTune   = o == 0 ? TB_P_WT1_TUNE : TB_P_WT2_TUNE;
            const int kFine   = o == 0 ? TB_P_WT1_FINE : TB_P_WT2_FINE;
            const int kUni    = o == 0 ? TB_P_WT1_UNI : TB_P_WT2_UNI;
            const int kDet    = o == 0 ? TB_P_WT1_DETUNE : TB_P_WT2_DETUNE;
            const int kSpread = o == 0 ? TB_P_WT1_SPREAD : TB_P_WT2_SPREAD;
            const int kPan    = o == 0 ? TB_P_WT1_PAN : TB_P_WT2_PAN;
            const int kBend   = o == 0 ? TB_P_WT1_BEND : TB_P_WT2_BEND;
            const int kForm   = o == 0 ? TB_P_WT1_FORMANT : TB_P_WT2_FORMANT;
            (void) kTable;

            VoicedWTParams &vp = o == 0 ? vp1 : vp2;
            const int dPos = o == 0 ? DST_WT1_POS : DST_WT2_POS;
            const int dLvl = o == 0 ? DST_WT1_LEVEL : DST_WT2_LEVEL;
            const int dTun = o == 0 ? DST_WT1_TUNE : DST_WT2_TUNE;
            const int dBnd = o == 0 ? DST_WT1_BEND : DST_WT2_BEND;
            const int dFrm = o == 0 ? DST_WT1_FORMANT : DST_WT2_FORMANT;
            const int dPan = o == 0 ? DST_WT1_PAN : DST_WT2_PAN;

            oscNote[o] = baseNote + P[kTune]
                       + P[kFine] / 100.0f
                       + modOff.o[dTun] * 12.0f;

            vp.voices   = std::clamp((int) P[kUni], 1, 4);
            vp.position = std::clamp(pot01(P[kPos]) + modOff.o[dPos], 0.0f, 1.0f);
            vp.gain     = std::clamp(potSquared(P[kLevel]) + modOff.o[dLvl], 0.0f, 1.0f);
            vp.pan      = std::clamp(potBipolar(P[kPan]) + modOff.o[dPan], -1.0f, 1.0f);
            vp.detune   = potDetune(P[kDet]);
            vp.spread   = pot01(P[kSpread]);
            vp.bend     = std::clamp(potBipolar(P[kBend]) + modOff.o[dBnd], -1.0f, 1.0f);
            vp.formant  = std::clamp(potBipolar(P[kForm]) + modOff.o[dFrm], -1.0f, 1.0f);
            oscGain[o] = vp.gain;
        }

        /* ---- sub + noise ---- */
        subNote = baseNote + P[TB_P_SUB_TUNE];
        float subLvl = std::clamp(potSquared(P[TB_P_SUB_LEVEL]) + modOff.o[DST_SUB_LEVEL],
                                  0.0f, 1.0f);
        float subPan = potBipolar(P[TB_P_SUB_PAN]);
        switch ((int) P[TB_P_SUB_WAVE]) {
        case 0: subParams.wave = Wave::sine; break;
        case 1: subParams.wave = Wave::triangle; break;
        case 2: subParams.wave = Wave::sawUp; break;
        case 3: subParams.wave = Wave::pulse; subParams.pw = 0.50f; break;
        case 4: subParams.wave = Wave::pulse; subParams.pw = 0.25f; break;
        default: subParams.wave = Wave::pulse; subParams.pw = 0.125f; break;
        }
        subParams.leftGain  = subLvl * (1.0f - subPan);
        subParams.rightGain = subLvl * (1.0f + subPan);

        float nzLvl = std::clamp(potSquared(P[TB_P_NOISE_LEVEL]) + modOff.o[DST_NOISE_LEVEL],
                                 0.0f, 1.0f);
        float nzPan = potBipolar(P[TB_P_NOISE_PAN]);
        noiseParams.wave = (int) P[TB_P_NOISE_TYPE] == 0 ? Wave::whiteNoise : Wave::pinkNoise;
        noiseParams.leftGain  = nzLvl * (1.0f - nzPan);
        noiseParams.rightGain = nzLvl * (1.0f + nzPan);

        /* ---- amp ---- */
        ampVelTrack = pot01(P[TB_P_VCA_VEL]);
        ampModGain = std::clamp(1.0f + modOff.o[DST_AMP], 0.0f, 2.0f);

        /* ---- filter (the original's law, verbatim) ---- */
        {
            const float filterWidth = 12.0f * std::log2(20000.0f / 440.0f) + 69.0f; /* midi(20k) */
            float env = filterADSR.getOutput();
            float sens = pot01(P[TB_P_FLT_VEL]);
            sens = velocity * sens + 1.0f - sens;

            float n = potFilterNote(P[TB_P_FLT_FREQ]);
            n += ((float) midiNote - 60.0f) * pot01(P[TB_P_FLT_KEY]);
            n += env * sens * potBipolar(P[TB_P_FLT_ENV]) * filterWidth;
            n += modOff.o[DST_FLT_FREQ] * filterWidth;

            float f = midiNoteToHz(n);
            float maxFreq = std::min(20000.0f, (float) (sampleRate / 2.0));
            f = std::clamp(f, 4.0f, maxFreq);

            float res = std::clamp(pot01(P[TB_P_FLT_RES]) + modOff.o[DST_FLT_RES],
                                   0.0f, 1.0f);
            float q = kFilterQ / (1.0f - res * 0.99f);

            static const Filter::Type kTypes[8] = {
                Filter::lowpass, Filter::lowpass, Filter::highpass, Filter::highpass,
                Filter::bandpass, Filter::bandpass, Filter::notch, Filter::notch };
            int t = (int) P[TB_P_FLT_TYPE] & 7;
            filter.setType(kTypes[t]);
            filter.setSlope((t & 1) ? Filter::db24 : Filter::db12);
            filter.setParams(f, q);
        }
    }

    VoicedWTOscillator osc1 { 4 }, osc2 { 4 };
    VoicedWTParams vp1, vp2;
    StereoOscillator sub, noise;
    StereoOscillator::Params subParams, noiseParams;
    Filter filter;
    AnalogADSR filterADSR, adsr;
    LFO lfos[3];
    ValueSmoother<float> noteSmoother;
    ModOffsets modOff;

    double sampleRate = 44100.0;
    float oscNote[2] = { 60.0f, 60.0f }, oscGain[2] = {};
    float subNote = 48.0f;
    float velocity = 0.0f, ampVelTrack = 1.0f, ampModGain = 1.0f, randomMod = 0.0f;
    int midiNote = -1;
    uint32_t serial = 0;
    bool active = false, fastKill = false, glissando = false, portamento = false;
};

} // namespace tb
