#include "engine.h"
#include "trace.h"

#include <cmath>
#include <cstring>

namespace tb {

Engine::Engine()
{
    for (int i = 0; i < TB_PARAM_COUNT; i++)
        pots[i] = tb_params[i].def;

    voices.reserve(8);
    for (int i = 0; i < 8; i++) {
        voices.emplace_back(analog);
        voices.back().setSampleRate(44100.0);
    }

    auto init = makeInitTable();
    std::atomic_store(&table1, init);
    std::atomic_store(&table2, init);

    syncModSlots();
}

void Engine::syncModSlots()
{
    static const int base[kModSlots] = {
        TB_P_M1_SRC, TB_P_M2_SRC, TB_P_M3_SRC, TB_P_M4_SRC };
    for (int i = 0; i < kModSlots; i++) {
        modSlots[i].src    = (int) pots[base[i] + 0];
        modSlots[i].dst    = (int) pots[base[i] + 1];
        modSlots[i].amount = potBipolar(pots[base[i] + 2]);
        modSlots[i].on     = pots[base[i] + 3] > 0.5f;
    }
}

VoiceContext Engine::makeContext(const std::shared_ptr<Wavetable> &t1,
                                 const std::shared_ptr<Wavetable> &t2)
{
    VoiceContext c;
    c.pots = pots;
    c.modSlots = modSlots;
    c.table1 = t1.get();
    c.table2 = t2.get();
    c.analog = &analog;
    c.modWheel = modWheel;
    c.aftertouch = aftertouch;
    c.pitchBendNorm = pitchBendNorm;
    c.pitchBendSemis = pitchBendNorm * pots[TB_P_PB_RANGE];
    c.bpm = hostBpm;
    return c;
}

Voice *Engine::findVoiceToSteal()
{
    /* idle first; then quietest releasing; then oldest serial */
    Voice *best = nullptr;
    for (auto &v : voices)
        if (!v.isActive()) return &v;

    for (auto &v : voices)
        if (v.isReleasing() && (!best || v.envOutput() < best->envOutput()))
            best = &v;
    if (best) return best;

    for (auto &v : voices)
        if (!best || v.serialNumber() < best->serialNumber())
            best = &v;
    return best;
}

void Engine::noteOn(int note, float vel)
{
    auto t1 = std::atomic_load(&table1);
    auto t2 = std::atomic_load(&table2);
    VoiceContext c = makeContext(t1, t2);

    const bool mono = (int) pots[TB_P_VOICE_MODE] == 1;
    const bool glideOn = (int) pots[TB_P_GLIDE_MODE] != 0;

    if (mono) {
        if (monoStackLen < kStackMax)
            monoStack[monoStackLen++] = note;

        Voice &v = voices[0];
        tbT(EV_NOTE_ON, note, (int) (vel * 127), 0, 1);
        if (v.isActive() && !v.isReleasing()) {
            tbT(EV_V_RETRIG, 0, v.currentNote(), note);
            v.retrigger(c, note, vel);          /* legato / retrig path */
        } else {
            float from = glideOn ? lastPlayedNote : -1.0f;
            v.start(c, note, vel, ++serialCounter, from);
        }
    } else {
        int maxVoices = std::clamp((int) pots[TB_P_VOICES], 1, 8);
        int activeCount = 0;
        for (auto &v : voices) if (v.isActive()) activeCount++;

        Voice *v = nullptr;
        for (auto &vv : voices)
            if (!vv.isActive()) { v = &vv; break; }
        if (!v || activeCount >= maxVoices)
            v = findVoiceToSteal();
        if (!v) return;

        float from = glideOn ? lastPlayedNote : -1.0f;
        tbT(EV_NOTE_ON, note, (int) (vel * 127), (int) (v - voices.data()), 0);
        if (v->isActive())
            tbT(EV_V_STEAL, (int) (v - voices.data()), v->currentNote(), note);
        v->start(c, note, vel, ++serialCounter, from);
    }

    if (note >= 0 && note < 128) {
        /* Demand fresh evidence of zero pressure, but KEEP the armed flag: a
         * host-manufactured duplicate note-on (see UPSTREAM-NOTES) is followed
         * only by the tail of the real press's pressure stream, so clearing
         * the flag here left exactly the stuck note this guards against. */
        atValue[note] = 127;
        atBlock[note] = blockCount;
    }
    lastPlayedNote = (float) note;
}

void Engine::noteOff(int note)
{
    const bool mono = (int) pots[TB_P_VOICE_MODE] == 1;
    tbT(EV_NOTE_OFF, note, mono, sustainDown, monoStackLen);

    if (sustainDown) {
        if (note >= 0 && note < 128) sustained[note] = true;
        if (!mono) return;
    }

    /* Drop it from the stack whatever the mode is NOW: the stack can still
     * hold notes pressed before a mono->poly flip, and a stale entry there
     * comes back as a phantom retrigger the next time mono is selected. */
    for (int i = 0; i < monoStackLen; i++) {
        if (monoStack[i] == note) {
            for (int j = i; j < monoStackLen - 1; j++)
                monoStack[j] = monoStack[j + 1];
            monoStackLen--;
            break;
        }
    }

    if (mono) {
        Voice &v = voices[0];
        if (v.isActive() && v.currentNote() == note) {
            if (monoStackLen > 0) {
                /* return to the most recent held note (last-note priority) */
                auto t1 = std::atomic_load(&table1);
                auto t2 = std::atomic_load(&table2);
                VoiceContext c = makeContext(t1, t2);
                v.retrigger(c, monoStack[monoStackLen - 1], 0.8f);
            } else if (!sustainDown) {
                tbT(EV_OFF_STOP, note, 0, 0);
                v.stop(true);
            } else {
                sustained[note & 127] = true;
            }
        }
    }

    /* Every OTHER voice holding this note is released too, in BOTH modes.
     *
     * This used to be the else-arm of the branch above, which made releasing
     * a note depend on the mode the synth is in when the finger LIFTS rather
     * than when it landed. Flip Poly -> Mono with a chord held -- one encoder
     * turn, or any preset recall that selects Mono -- and every note-off went
     * down the mono arm, which only ever looks at voices[0]. The rest of the
     * chord never got its note-off and sounded on until something stole the
     * voice: the "stuck note that clears when I play more notes" report. */
    int hits = 0;
    for (size_t i = mono ? 1 : 0; i < voices.size(); i++) {
        Voice &v = voices[i];
        if (v.isActive() && !v.isReleasing() && v.currentNote() == note) {
            tbT(EV_OFF_STOP, note, (int) i, 1);
            v.stop(true);
            hits++;
        }
    }
    if (!hits) tbT(EV_OFF_NOMATCH, note, mono);
}

void Engine::allNotesOff(bool allowTail)
{
    for (auto &v : voices)
        if (v.isActive())
            allowTail ? v.stop(true) : v.kill();
    monoStackLen = 0;
    std::memset(sustained, 0, sizeof sustained);
}

void Engine::onMidi(const uint8_t *msg, int len)
{
    if (len < 2) return;
    const uint8_t status = msg[0] & 0xF0;

    switch (status) {
    case 0x90:
        if (len >= 3 && msg[2] > 0) { noteOn(msg[1], (float) msg[2] / 127.0f); break; }
        [[fallthrough]];
    case 0x80:
        noteOff(msg[1]);
        break;
    case 0xB0:
        if (len >= 3) {
            switch (msg[1]) {
            case 1:  modWheel = (float) msg[2] / 127.0f; break;
            case 64: {
                bool down = msg[2] >= 64;
                tbT(EV_CC, 64, msg[2]);
                if (sustainDown && !down) {
                    /* release everything that was sustained */
                    for (int n = 0; n < 128; n++) {
                        if (sustained[n]) {
                            sustained[n] = false;
                            for (auto &v : voices)
                                if (v.isActive() && !v.isReleasing() && v.currentNote() == n)
                                    v.stop(true);
                        }
                    }
                }
                sustainDown = down;
                break;
            }
            case 120: case 123:
                tbT(EV_CC, msg[1], msg[2]);
                allNotesOff(msg[1] == 123);
                break;
            }
        }
        break;
    case 0xA0:                       /* poly aftertouch: Move's pad pressure */
        if (len >= 3) {
            int n = msg[1] & 127;
            atValue[n] = msg[2];
            atBlock[n] = blockCount;
            /* Armed only by real pressure. A controller that opens with a
             * zero-pressure message must not arm the watchdog, or its note
             * would be released while the finger is still down. */
            if (msg[2] > 0) atArmed[n] = true;
            /* highest pad pressure also drives the aftertouch mod source, so
             * poly AT is not merely swallowed */
            aftertouch = (float) msg[2] / 127.0f;
        }
        break;
    case 0xD0:
        aftertouch = (float) msg[1] / 127.0f;
        break;
    case 0xE0:
        if (len >= 3) {
            int v14 = (msg[2] << 7) | msg[1];
            pitchBendNorm = (float) (v14 - 8192) / 8192.0f;
        }
        break;
    }
}

void Engine::renderBlock(int16_t *out, int frames)
{
    auto t1 = std::atomic_load(&table1);
    auto t2 = std::atomic_load(&table2);

    float L[kBlock], R[kBlock];

    int done = 0;
    while (done < frames) {
        int n = std::min(frames - done, kBlock);
        std::memset(L, 0, (size_t) n * sizeof(float));
        std::memset(R, 0, (size_t) n * sizeof(float));

        VoiceContext c = makeContext(t1, t2);
        for (auto &v : voices)
            if (v.isActive())
                v.render(c, L, R, n);

        /* A held pad whose pressure hit zero and then stopped reporting is a
         * finger that left without a note-off arriving. Release it. */
        blockCount++;
        for (auto &v : voices) {
            if (!v.isActive() || v.isReleasing()) continue;
            int nn = v.currentNote();
            if (nn < 0 || nn > 127 || !atArmed[nn]) continue;
            if (atValue[nn] != 0) continue;
            if (blockCount - atBlock[nn] < kPadGoneBlocks) continue;
            tbT(EV_PAD_GONE, nn, (int) (blockCount - atBlock[nn]));
            atArmed[nn] = false;
            noteOff(nn);
        }

        /* every ~250 ms, one line per still-active voice: a voice that keeps
         * appearing here with no matching note-off IS the stuck note */
        if (++snapCounter >= 86) {
            snapCounter = 0;
            for (size_t i = 0; i < voices.size(); i++)
                if (voices[i].isActive())
                    tbT(EV_SNAP, (int) i, voices[i].currentNote(),
                        voices[i].adsrState(), (int) (voices[i].envOutput() * 1000));
        }

        const float master = potSquared(pots[TB_P_VOLUME]) * 0.8f;
        for (int i = 0; i < n; i++) {
            float l = std::clamp(L[i] * master, -1.0f, 1.0f);
            float r = std::clamp(R[i] * master, -1.0f, 1.0f);
            out[(done + i) * 2 + 0] = (int16_t) (l * 32767.0f);
            out[(done + i) * 2 + 1] = (int16_t) (r * 32767.0f);
        }
        done += n;
    }
}

std::shared_ptr<Wavetable> makeInitTable()
{
    /* 64 frames x 2048: pulse morphing 50% -> 6% duty with a saw underlay.
     * Naive shapes on purpose — wtBuild band-limits every level. */
    const int F = 64, S = 2048;
    std::vector<float> data((size_t) F * S);
    for (int f = 0; f < F; f++) {
        float m = (float) f / (F - 1);
        float pw = 0.5f - 0.44f * m;
        for (int i = 0; i < S; i++) {
            float ph = (float) i / S;
            float pulse = ph < pw ? 1.0f : -1.0f;
            float saw = 2.0f * ph - 1.0f;
            data[(size_t) f * S + i] = 0.8f * ((1.0f - m * 0.5f) * pulse * 0.6f +
                                               m * 0.4f * saw);
        }
    }
    /* normalize the source to 0.9 peak so the default patch lands hot */
    float peak = 0.0f;
    for (float v : data) peak = std::max(peak, std::fabs(v));
    if (peak > 0.0f)
        for (float &v : data) v *= 0.9f / peak;

    auto wt = std::make_shared<Wavetable>();
    wtBuild(*wt, data.data(), F, S, 44100.0f, 44100.0f);
    return wt;
}

} // namespace tb
