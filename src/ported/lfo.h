/* LFO — de-JUCE'd port of gin::LFO (BSD-3, Roland Rabien).
 *
 * 18 shapes, phase offset, fade-in, delay, depth/offset. The sample-and-hold
 * and noise shapes read a table of 1000 random points generated at
 * construction from juce::Random seeded with 1 — we replicate JUCE's LCG
 * (the java.util.Random recurrence) so those shapes are sample-identical to
 * the original synth.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace tb {

/* juce::Random's exact recurrence. */
class JuceRandom {
public:
    explicit JuceRandom(int64_t seedValue) : seed(seedValue) {}
    int nextInt()
    {
        seed = (int64_t) ((((uint64_t) seed) * 0x5deece66dULL + 11) & 0xffffffffffffULL);
        return (int) (seed >> 16);
    }
    float nextFloat()
    {
        float r = (float) (uint32_t) nextInt() / ((float) 0xffffffffu + 1.0f);
        return r < 1.0f ? r : 1.0f - 1.0e-7f;   /* JUCE clamps below 1 */
    }
private:
    int64_t seed;
};

class LFO {
public:
    enum class WaveShape : int {
        none, sine, triangle, sawUp, sawDown, square, squarePos,
        sampleAndHold, noise,
        stepUp3, stepUp4, stepUp8, stepDown3, stepDown4, stepDown8,
        pyramid3, pyramid5, pyramid9,
    };

    static constexpr int maxRandomPhase = 1000;

    struct Parameters {
        WaveShape waveShape = WaveShape::sine;
        float frequency = 0, phase = 0, offset = 0, depth = 0, delay = 0, fade = 0;
    };

    LFO()
    {
        JuceRandom rnd(1);
        for (int i = 0; i < maxRandomPhase; i++)
            randomPoints[i] = rnd.nextFloat() * 2.0f - 1.0f;
    }

    void setSampleRate(double sr) { sampleRate = sr; }

    void setParameters(const Parameters &p)
    {
        auto oldShape = parameters.waveShape;
        parameters = p;
        if ((oldShape != WaveShape::sampleAndHold && oldShape != WaveShape::noise) &&
            (parameters.waveShape == WaveShape::sampleAndHold ||
             parameters.waveShape == WaveShape::noise))
            phase = (float) (rand() % maxRandomPhase);
    }

    void reset()
    {
        output = 0.0f; phase = 0.0f; curPhase = 0.0f;
        curFade = 1.0f; fadeDelta = 0.0f; delaySteps = 0;
    }

    void noteOn(float phase_ = -1.0f)
    {
        float maxPhase = isRandomShape() ? (float) maxRandomPhase : 1.0f;

        curFade = (parameters.fade <= 0) ? 1.0f : 0.0f;
        curPhase = 0.0f;

        if (isRandomShape())
            phase = phase_ < 0.0f ? (float) (rand() % maxRandomPhase) : phase_;
        else
            phase = phase_ < 0.0f ? 0.0f : phase_;

        fadeDelta  = (float) (1.0f / (sampleRate * parameters.fade));
        delaySteps = (int) std::lround(sampleRate * parameters.delay);

        float p = std::fmod(phase + parameters.phase, maxPhase);
        if (p < 0) p += maxPhase;
        curPhase = p;

        updateCurrentValue();
    }

    float process(int numSamples)
    {
        float step = 0.0f;
        if (parameters.frequency > 0.0001f)
            step = (float) (parameters.frequency / sampleRate);

        for (int i = 0; i < numSamples; i++) {
            if (delaySteps > 0) {
                delaySteps--;
            } else {
                curFade = std::clamp(curFade + fadeDelta, 0.0f, 1.0f);
                float maxPhase = isRandomShape() ? (float) maxRandomPhase : 1.0f;

                phase += step;
                while (phase >= maxPhase) phase -= maxPhase;

                float p = std::fmod(phase + parameters.phase, maxPhase);
                if (p < 0) p += maxPhase;
                curPhase = p;
            }
        }
        return updateCurrentValue();
    }

    float getOutput() const
    {
        return std::clamp(curFade * output * parameters.depth + parameters.offset,
                          -1.0f, 1.0f);
    }
    float getOutputUnclamped() const
    {
        return curFade * output * parameters.depth + parameters.offset;
    }
    float getCurrentPhase() const { return phase; }

private:
    bool isRandomShape() const
    {
        return parameters.waveShape == WaveShape::sampleAndHold ||
               parameters.waveShape == WaveShape::noise;
    }
    static float lerp(float t, float a, float b) { return a + t * (b - a); }

    float updateCurrentValue()
    {
        if (delaySteps == 0) {
            switch (parameters.waveShape) {
            case WaveShape::none:     output = 0; break;
            case WaveShape::sine:     output = std::sin(curPhase * 2.0f * (float) M_PI); break;
            case WaveShape::triangle: {
                float p = std::fmod(curPhase + 0.25f, 1.0f);
                output = (p < 0.5f) ? (4.0f * p - 1.0f) : (-4.0f * p + 3.0f);
                break;
            }
            case WaveShape::sawUp:     output = curPhase * 2.0f - 1.0f; break;
            case WaveShape::sawDown:   output = (1.0f - curPhase) * 2.0f - 1.0f; break;
            case WaveShape::square:    output = (curPhase < 0.5f) ? 1.0f : -1.0f; break;
            case WaveShape::squarePos: output = (curPhase < 0.5f) ? 1.0f : 0.0f; break;
            case WaveShape::sampleAndHold:
                output = randomPoints[(int) curPhase];
                break;
            case WaveShape::noise: {
                int p = (int) curPhase;
                float t = curPhase - (float) p;
                output = lerp(t, randomPoints[p],
                              randomPoints[p + 1 < maxRandomPhase ? p + 1 : 0]);
                break;
            }
            case WaveShape::stepUp3:   output =  (float) ((int) (curPhase * 3)) / 3.0f * 6.0f / 2.0f - 1; break;
            case WaveShape::stepUp4:   output =  (float) ((int) (curPhase * 4)) / 4.0f * 8.0f / 3.0f - 1; break;
            case WaveShape::stepUp8:   output =  (float) ((int) (curPhase * 8)) / 8.0f * 16.0f / 7.0f - 1; break;
            case WaveShape::stepDown3: output = -((float) ((int) (curPhase * 3)) / 3.0f * 6.0f / 2.0f - 1); break;
            case WaveShape::stepDown4: output = -((float) ((int) (curPhase * 4)) / 4.0f * 8.0f / 3.0f - 1); break;
            case WaveShape::stepDown8: output = -((float) ((int) (curPhase * 8)) / 8.0f * 16.0f / 7.0f - 1); break;
            case WaveShape::pyramid3: {
                static const float vals[] = { 0.0f, 1.0f, 0.0f, -1.0f };
                output = vals[(int) (phase * 4) & 3];
                break;
            }
            case WaveShape::pyramid5: {
                static const float vals[] = { 0.0f, 0.5f, 1.0f, 0.5f, 0.0f, -0.5f, -1.0f, -0.5f };
                output = vals[(int) (phase * 8) & 7];
                break;
            }
            case WaveShape::pyramid9: {
                static const float vals[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 0.75f, 0.5f, 0.25f, 0.0f,
                                              -0.25f, -0.5f, -0.75f, -1.0f, -0.75f, -0.5f, -0.25f };
                output = vals[(int) (phase * 16) & 15];
                break;
            }
            }
        }
        return getOutput();
    }

    Parameters parameters;
    double sampleRate = 0.0;
    float phase = 0.0f, curPhase = 0.0f, output = 0.0f;
    float fadeDelta = 0.0f, curFade = 1.0f;
    int delaySteps = 0;
    float randomPoints[maxRandomPhase];
};

} // namespace tb
