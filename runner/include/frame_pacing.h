#ifndef SATURN_FRAME_PACING_H
#define SATURN_FRAME_PACING_H
#include <stdint.h>

/* Keep the presentation clock independent of ordinary work/sleep jitter.
 * A long host stall starts a fresh timeline instead of accumulating an
 * unbounded backlog. This schedules waits only; no emulated field is skipped. */
static inline uint64_t saturn_next_field_deadline(uint64_t previous,
                                                 uint64_t now,
                                                 uint64_t period)
{
    uint64_t next = previous ? previous + period : now + period;
    if (now > next && now - next > period * 2u)
        next = now + period;
    return next;
}
#endif
