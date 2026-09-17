#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Protected by audio_bus's critical section; no hardware work under this lock. */
typedef uint64_t audio_lease_t;
typedef struct { audio_lease_t sequence, current; } audio_focus_t;
static inline audio_lease_t audio_focus_request(audio_focus_t *s)
{
    if (++s->sequence == 0) ++s->sequence;
    return s->current = s->sequence;
}
static inline bool audio_focus_current(const audio_focus_t *s, audio_lease_t lease)
{
    return lease != 0 && s->current == lease;
}
static inline void audio_focus_release(audio_focus_t *s, audio_lease_t lease)
{
    if (audio_focus_current(s, lease)) s->current = 0;
}
