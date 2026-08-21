/* Audio-thread-safe event trace, for hunting a bug that only a pair of
 * hands can produce.
 *
 * printf() on the SPI callback is exactly the kind of thing that turns a
 * timing bug into a different timing bug, so nothing here writes a file.
 * The audio thread appends a fixed-size POD record to a ring with one
 * relaxed atomic increment -- no allocation, no locks, no syscalls. The
 * worker thread (already demoted off SCHED_FIFO 90) drains it to disk.
 *
 * Compiled out entirely unless TB_TRACE is defined.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace tb {

enum TbEv : uint16_t {
    EV_NOTE_ON = 1,     /* a=note b=vel   c=voice d=mode(0poly/1mono)      */
    EV_NOTE_OFF,        /* a=note b=mode  c=sustainDown d=monoStackLen     */
    EV_OFF_STOP,        /* a=note b=voice c=arm(0 mono,1 sweep)            */
    EV_OFF_NOMATCH,     /* a=note b=mode                                   */
    EV_V_START,         /* a=voice b=note c=serial                         */
    EV_V_STEAL,         /* a=voice b=oldNote c=newNote                     */
    EV_V_RETRIG,        /* a=voice b=oldNote c=newNote                     */
    EV_V_IDLE,          /* a=voice b=note                                  */
    EV_CC,              /* a=cc   b=val                                    */
    EV_PARAM,           /* a=paramIdx b=value*10                           */
    EV_SNAP,            /* a=voice b=note c=adsrState d=env*1000           */
    EV_TABLE,           /* a=osc  b=ok                                     */
    EV_PAD_GONE,        /* a=note b=blocks since last pressure              */
    EV_MIDI,            /* a=status b=d1 c=d2 d=len -- RAW, at the boundary */
};

struct TbRec {
    uint64_t t;                 /* CLOCK_MONOTONIC ns */
    uint16_t ev;
    int16_t  a, b, c, d;
};

/* power of two */
static constexpr uint32_t kTraceSize = 1u << 15;

struct TbTrace {
    TbRec ring[kTraceSize];
    std::atomic<uint32_t> head { 0 };
    uint32_t tail = 0;                  /* drain cursor: worker only */
};

extern TbTrace g_trace;
extern std::atomic<bool> g_traceOn;

#ifdef TB_TRACE
uint64_t tbNowNs();

/* AUDIO THREAD. One atomic increment and a struct store. */
inline void tbT(uint16_t ev, int a = 0, int b = 0, int c = 0, int d = 0)
{
    if (!g_traceOn.load(std::memory_order_relaxed)) return;
    uint32_t i = g_trace.head.fetch_add(1, std::memory_order_relaxed);
    TbRec &r = g_trace.ring[i & (kTraceSize - 1)];
    r.t = tbNowNs();
    r.ev = ev;
    r.a = (int16_t) a; r.b = (int16_t) b; r.c = (int16_t) c; r.d = (int16_t) d;
}

/* WORKER THREAD. Appends everything new to `path`. */
void tbTraceDrain(const char *path);
#else
inline void tbT(uint16_t, int = 0, int = 0, int = 0, int = 0) {}
inline void tbTraceDrain(const char *) {}
#endif

} // namespace tb
