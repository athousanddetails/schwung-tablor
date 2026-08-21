#include "trace.h"

#include <ctime>

namespace tb {

TbTrace g_trace;
std::atomic<bool> g_traceOn { false };

#ifdef TB_TRACE

uint64_t tbNowNs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);   /* vDSO: no syscall */
    return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

static const char *evName(uint16_t ev)
{
    switch (ev) {
    case EV_NOTE_ON:    return "NOTE_ON   ";
    case EV_NOTE_OFF:   return "NOTE_OFF  ";
    case EV_OFF_STOP:   return "  off_stop";
    case EV_OFF_NOMATCH:return "  off_MISS";
    case EV_V_START:    return "  v_start ";
    case EV_V_STEAL:    return "  v_STEAL ";
    case EV_V_RETRIG:   return "  v_retrig";
    case EV_V_IDLE:     return "  v_idle  ";
    case EV_CC:         return "CC        ";
    case EV_PARAM:      return "PARAM     ";
    case EV_SNAP:       return "    snap  ";
    case EV_TABLE:      return "TABLE     ";
    case EV_PAD_GONE:   return "PAD_GONE  ";
    case EV_MIDI:       return "midi_in   ";
    }
    return "?         ";
}

void tbTraceDrain(const char *path)
{
    uint32_t head = g_trace.head.load(std::memory_order_relaxed);
    if (head == g_trace.tail) return;

    /* if the audio thread lapped us, skip what we lost rather than print junk */
    if (head - g_trace.tail > kTraceSize) g_trace.tail = head - kTraceSize;

    FILE *f = fopen(path, "a");
    if (!f) { g_trace.tail = head; return; }

    static uint64_t t0 = 0;
    for (; g_trace.tail != head; g_trace.tail++) {
        const TbRec &r = g_trace.ring[g_trace.tail & (kTraceSize - 1)];
        if (!t0) t0 = r.t;
        double ms = (double) (r.t - t0) / 1e6;
        fprintf(f, "%10.2f %s a=%-5d b=%-5d c=%-5d d=%-5d\n",
                ms, evName(r.ev), r.a, r.b, r.c, r.d);
    }
    fclose(f);
}

#endif

} // namespace tb
