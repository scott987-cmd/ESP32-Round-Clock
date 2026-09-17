#include <assert.h>
#include <stdio.h>
#include "reset_notice_state.h"
int main(void)
{
    reset_notice_decision_t d=reset_notice_decide(false,true,false,true,"old","hint","old","");
    assert(d.watch&&!d.reset&&!d.remember_reset&&d.remember_watch);
    d=reset_notice_decide(false,false,false,true,"old","","old","hint");
    assert(!d.watch&&!d.reset&&!d.remember_watch);
    d=reset_notice_decide(false,true,false,true,"old","hint","old","hint");
    assert(!d.watch&&!d.reset); /* No duplicate after disappearing/reappearing. */
    d=reset_notice_decide(true,false,false,true,"hint","","old","hint");
    assert(d.reset&&!d.watch); /* The same post can later be confirmed. */
    d=reset_notice_decide(true,true,true,true,"new","new","old","old");
    assert(!d.reset&&!d.watch&&!d.remember_reset&&!d.remember_watch);
    d=reset_notice_decide(true,false,false,true,"new","","","");
    assert(d.reset); /* First real fresh event need not be suppressed. */
    d=reset_notice_decide(false,false,false,true,"old","","","");
    assert(!d.reset&&d.remember_reset); /* History is remembered, not alerted. */
    puts("PASS independent reset/watch cursors, stale gate, promotion, history, first fresh event");
}
