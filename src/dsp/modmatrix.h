/* Fixed-slot modulation matrix — written fresh for Tablor (gin's ModMatrix is
 * welded to juce::AudioProcessor; a synth with 8 visible slots doesn't need
 * its generality).
 *
 * 8 slots x (source, destination, bipolar amount, on). Evaluated per voice
 * per block. Source and destination enums MUST match tools/gen_params.py's
 * MOD_SRC / MOD_DST option order — the DSP receives them as indexes.
 */
#pragma once

namespace tb {

enum ModSrc {
    SRC_NONE = 0, SRC_LFO1, SRC_LFO2, SRC_FILTER_EG, SRC_VCA_EG,
    SRC_VELOCITY, SRC_NOTE, SRC_MODWHEEL, SRC_AFTERTOUCH, SRC_PITCHBEND,
    SRC_RANDOM, SRC_COUNT
};

enum ModDst {
    DST_NONE = 0,
    DST_WT1_POS, DST_WT2_POS, DST_WT1_LEVEL, DST_WT2_LEVEL,
    DST_WT1_TUNE, DST_WT2_TUNE, DST_WT1_BEND, DST_WT2_BEND,
    DST_WT1_FORMANT, DST_WT2_FORMANT, DST_WT1_PAN, DST_WT2_PAN,
    DST_FLT_FREQ, DST_FLT_RES, DST_SUB_LEVEL, DST_NOISE_LEVEL, DST_AMP,
    DST_LFO1_RATE, DST_LFO2_RATE, DST_COUNT
};

struct ModSlot {
    int   src = SRC_NONE;
    int   dst = DST_NONE;
    float amount = 0.0f;    /* -1..1 */
    bool  on = false;
};

constexpr int kModSlots = 4;

/* Per-voice source snapshot for one block. All values -1..1 or 0..1. */
struct ModSources {
    float values[SRC_COUNT] = {};
};

/* Sum of amount-weighted sources per destination for one voice-block. */
struct ModOffsets {
    float o[DST_COUNT] = {};

    void compute(const ModSlot *slots, const ModSources &src)
    {
        for (int d = 0; d < DST_COUNT; d++) o[d] = 0.0f;
        for (int i = 0; i < kModSlots; i++) {
            const ModSlot &s = slots[i];
            if (!s.on || s.src == SRC_NONE || s.dst == DST_NONE) continue;
            o[s.dst] += src.values[s.src] * s.amount;
        }
    }
};

} // namespace tb
