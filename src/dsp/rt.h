/* Realtime hygiene for a Schwung module.
 *
 * Schwung's threading contract (docs/MODULES.md, "Threading: there is no
 * control thread"): EVERY plugin entry point — create_instance, set_param,
 * get_param, on_midi, render_block — runs on the SPI audio callback, at
 * SCHED_FIFO 90 on core 3, with ~900 us per block. No file I/O, no
 * allocation, no logging, no unbounded work in any of them.
 *
 * And a thread started from one of those calls INHERITS FIFO 90, which
 * starves Move's own audio (its Link publisher runs at FIFO 35). Every
 * worker must demote itself as its first action.
 */
#pragma once

#include <pthread.h>
#include <sched.h>

namespace tb {

/* MUST be the first thing a worker thread does. */
inline void rtDemoteThisThread()
{
    struct sched_param sp = { };
    sp.sched_priority = 0;
    sched_setscheduler(0, SCHED_OTHER, &sp);   /* drop the inherited FIFO 90 */

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);                          /* leave core 3 to the SPI thread */
    CPU_SET(1, &set);
    CPU_SET(2, &set);
    sched_setaffinity(0, sizeof(set), &set);
}

} // namespace tb
