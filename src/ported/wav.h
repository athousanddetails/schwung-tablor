/* Minimal WAV reader — build path only, never on the audio thread.
 *
 * Reads PCM 16/24/32-bit and float32, first channel only (wavetables are
 * mono; for stereo files channel 0 is the table). Walks RIFF chunks and
 * captures the Serum "clm " chunk ("<!>2048 ...") for the frame size —
 * the same detection gin's getWavetableSize does.
 */
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace tb {

struct WavData {
    std::vector<float> samples;   /* channel 0 */
    float sampleRate = 0.0f;
    int   clmFrameSize = 0;       /* 0 = no clm chunk */
};

inline bool wavLoad(const char *path, WavData &out)
{
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;

    auto rd32 = [&](uint32_t &v) { return std::fread(&v, 4, 1, f) == 1; };

    uint32_t riff = 0, size = 0, wave = 0;
    if (!rd32(riff) || !rd32(size) || !rd32(wave) ||
        riff != 0x46464952u /*RIFF*/ || wave != 0x45564157u /*WAVE*/) {
        std::fclose(f);
        return false;
    }

    uint16_t fmt = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    bool haveFmt = false;

    out = WavData();

    uint32_t id = 0, len = 0;
    while (rd32(id) && rd32(len)) {
        long next = std::ftell(f) + (long) len + (len & 1); /* chunks pad to even */

        if (id == 0x20746d66u) {                            /* "fmt " */
            uint8_t buf[16];
            if (len >= 16 && std::fread(buf, 16, 1, f) == 1) {
                std::memcpy(&fmt, buf + 0, 2);
                std::memcpy(&channels, buf + 2, 2);
                std::memcpy(&rate, buf + 4, 4);
                std::memcpy(&bits, buf + 14, 2);
                haveFmt = channels >= 1;
            }
        } else if (id == 0x206d6c63u) {                     /* "clm " (Serum) */
            std::string s((size_t) len, 0);
            if (len && std::fread(&s[0], len, 1, f) == 1 && s.rfind("<!>", 0) == 0)
                out.clmFrameSize = std::atoi(s.c_str() + 3);
        } else if (id == 0x61746164u && haveFmt) {          /* "data" */
            const int bytesPer = bits / 8;
            if ((fmt == 1 && (bits == 16 || bits == 24 || bits == 32)) ||
                (fmt == 3 && bits == 32)) {
                const uint32_t frames = len / (uint32_t) (bytesPer * channels);
                out.samples.resize(frames);
                std::vector<uint8_t> raw((size_t) bytesPer * channels);
                for (uint32_t i = 0; i < frames; i++) {
                    if (std::fread(raw.data(), raw.size(), 1, f) != 1) break;
                    const uint8_t *p = raw.data();          /* channel 0 */
                    float v = 0.0f;
                    if (fmt == 3) {
                        std::memcpy(&v, p, 4);
                    } else if (bits == 16) {
                        int16_t s16; std::memcpy(&s16, p, 2);
                        v = (float) s16 / 32768.0f;
                    } else if (bits == 24) {
                        int32_t s32 = (int32_t) ((uint32_t) p[0] << 8 |
                                                 (uint32_t) p[1] << 16 |
                                                 (uint32_t) p[2] << 24) >> 8;
                        v = (float) s32 / 8388608.0f;
                    } else {                                /* PCM 32 */
                        int32_t s32; std::memcpy(&s32, p, 4);
                        v = (float) s32 / 2147483648.0f;
                    }
                    out.samples[i] = v;
                }
            }
        }
        if (std::fseek(f, next, SEEK_SET) != 0) break;
    }
    std::fclose(f);

    out.sampleRate = (float) rate;
    return haveFmt && !out.samples.empty();
}

inline bool wavIsPow2(int v) { return v > 0 && (v & (v - 1)) == 0; }

/* Frame-size inference: clm chunk first, then divisibility, largest first.
 *
 * The list used to stop at 256, which quietly excluded every short table.
 * A single cycle of 128 samples divides by nothing in {2048..256}, so it
 * inferred nothing and the caller's 2048 fallback made nFrames 0 -- the load
 * failed and the table was silent. A file whose real cycle is short but whose
 * LENGTH happens to divide by 2048 was worse: it loaded, with each "frame"
 * spanning many real cycles, which is the buzz.
 *
 * Returns 0 when no power-of-two cycle fits at all -- notably Adventure Kid's
 * own AKWF format, which is 600 samples per cycle. That is not a failure and
 * the caller must not treat it as one: see the resampling path in the loader. */
inline int wavInferFrameSizeForLength(int n)
{
    for (int fs : { 2048, 1024, 512, 256, 128, 64, 32 })
        if (n >= fs && n % fs == 0)
            return fs;
    return 0;
}

inline int wavInferFrameSize(const WavData &w)
{
    if (w.clmFrameSize >= 32 && w.clmFrameSize <= 4096 && wavIsPow2(w.clmFrameSize))
        return w.clmFrameSize;
    return wavInferFrameSizeForLength((int) w.samples.size());
}

/* Resample every frame to `target` samples, linearly.
 *
 * wtBuild needs a power-of-two frame of at least 32; a 600-sample AKWF cycle
 * is neither. Stretching each cycle to a power of two is what makes those
 * playable at all, and costs nothing audible -- wtBuild band-limits the result
 * afterwards regardless. */
inline std::vector<float> wavResampleFrames(const std::vector<float> &in,
                                            int nFrames, int frameSize,
                                            int target)
{
    std::vector<float> out((size_t) nFrames * (size_t) target);
    for (int f = 0; f < nFrames; f++) {
        const float *src = in.data() + (size_t) f * (size_t) frameSize;
        float *dst = out.data() + (size_t) f * (size_t) target;
        for (int i = 0; i < target; i++) {
            float pos = (float) i * (float) frameSize / (float) target;
            int   i0  = (int) pos;
            float t   = pos - (float) i0;
            int   i1  = (i0 + 1) % frameSize;      /* wrap: it is a cycle */
            if (i0 >= frameSize) i0 = frameSize - 1;
            dst[i] = src[i0] + t * (src[i1] - src[i0]);
        }
    }
    return out;
}

} // namespace tb
