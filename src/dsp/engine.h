/* Tablor engine — voice allocation, poly/mono, glide, MIDI, master out.
 * Owns the wavetables, the analog tables and the 8 voices.
 */
#pragma once

#include "voice.h"

#include <atomic>
#include <memory>
#include <vector>

namespace tb {

class Engine {
public:
    Engine();

    /* Pots are written by set_param (UI thread) and read by the audio
     * thread; aligned float stores are atomic on aarch64. */
    float pots[TB_PARAM_COUNT] = {};
    ModSlot modSlots[kModSlots];

    /* Refresh mod slots from the pot array (call after any m*_ change). */
    void syncModSlots();

    void setHostBpm(float bpm) { hostBpm = bpm; }

    void onMidi(const uint8_t *msg, int len);
    void renderBlock(int16_t *outInterleavedLr, int frames);

    /* Wavetable slots — phase 4 swaps these from the loader thread. */
    void setTable(int osc, std::shared_ptr<Wavetable> t)
    {
        if (osc == 0) std::atomic_store(&table1, std::move(t));
        else          std::atomic_store(&table2, std::move(t));
    }

    void allNotesOff(bool allowTail);

private:
    void noteOn(int note, float vel);
    void noteOff(int note);
    Voice *findVoiceToSteal();
    VoiceContext makeContext(const std::shared_ptr<Wavetable> &t1,
                             const std::shared_ptr<Wavetable> &t2);

    AnalogTables analog { 44100.0 };
    std::vector<Voice> voices;
    std::shared_ptr<Wavetable> table1, table2;

    /* mono mode state: held-note stack, last-note priority */
    static constexpr int kStackMax = 32;
    int  monoStack[kStackMax];
    int  monoStackLen = 0;

    int  snapCounter = 0;                 /* TB_TRACE: voice snapshot pacing */

    /* ---- lost-note-off safety net ------------------------------------
     * Move's pads stream poly aftertouch (0xA0) for every held pad, about
     * every 29 ms, and it decays to 0 as the finger lifts. Traced on the
     * device, a note-off sometimes never arrives at all while pads are
     * played and an encoder is turned -- the note-on and the whole
     * aftertouch decay are delivered, the note-off simply is not, and the
     * voice sings on until something steals it.
     *
     * So a note whose pad pressure reached zero and then went SILENT is a
     * finger that is gone. Armed only for notes that actually sent poly
     * aftertouch, so a MIDI keyboard (which sends none) is untouched, and
     * it needs both zero pressure and a long silence.
     *
     * 1.5 s, not the 400 ms tried first: Move sends a stray zero mid-press
     * (measured on hardware, pressure 0 arriving 800 ms before the real
     * note-off), so a short window cuts notes that are still held. This is
     * a backstop for a host bug -- see docs/UPSTREAM-NOTES.md -- and a note
     * that hangs 1.5 s beats a note that dies under the finger. */
    static constexpr uint32_t kPadGoneBlocks = 517;   /* ~1.5 s of 2.9 ms blocks */
    uint32_t blockCount = 0;
    uint32_t atBlock[128] = {};      /* blockCount when pressure last arrived */
    uint8_t  atValue[128] = {};      /* that pressure                          */
    bool     atArmed[128] = {};      /* has this note ever sent poly AT?       */
    float lastPlayedNote = -1.0f;
    uint32_t serialCounter = 0;

    /* controller state */
    float modWheel = 0.0f, aftertouch = 0.0f;
    float pitchBendNorm = 0.0f;
    float hostBpm = 120.0f;
    bool  sustainDown = false;
    bool  sustained[128] = {};
};

/* Built-in "Init" wavetable: 64-frame PWM morph, generated in memory so the
 * synth makes sound with no files installed. Band-limiting happens in
 * wtBuild, so the naive source waveform is fine. */
std::shared_ptr<Wavetable> makeInitTable();

} // namespace tb
