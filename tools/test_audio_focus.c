#include <assert.h>
#include <stdio.h>
#include "audio_focus.h"

int main(void)
{
    audio_focus_t s={0};
    assert(!audio_focus_current(&s,0));
    audio_lease_t music=audio_focus_request(&s);
    audio_lease_t word=audio_focus_request(&s);
    assert(!audio_focus_current(&s,music));
    assert(audio_focus_current(&s,word));
    audio_focus_release(&s,music); /* Late HTTP worker cleanup. */
    assert(audio_focus_current(&s,word));
    audio_focus_release(&s,0);
    assert(audio_focus_current(&s,word));
    audio_lease_t mic=audio_focus_request(&s);
    audio_focus_release(&s,word); /* Hidden application's stop callback. */
    assert(audio_focus_current(&s,mic));
    audio_focus_release(&s,mic);
    assert(!audio_focus_current(&s,mic));
    for(unsigned i=0;i<100000;i++) {
        audio_lease_t old=audio_focus_request(&s),fresh=audio_focus_request(&s);
        audio_focus_release(&s,old);
        assert(audio_focus_current(&s,fresh));
    }
    s.sequence=UINT64_MAX;
    assert(audio_focus_request(&s)==1); /* Zero is always invalid. */
    puts("PASS audio focus: latest wins, stale cleanup, cancellation, wrap");
    return 0;
}
