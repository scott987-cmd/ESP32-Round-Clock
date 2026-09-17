#pragma once
#include <stdbool.h>
#include <string.h>

/* Hints and confirmations have independent cursors. A disappearing hint must
 * not replace its cursor with the previous completed reset's id. */
typedef struct {bool reset,watch,remember_reset,remember_watch;} reset_notice_decision_t;
static inline reset_notice_decision_t reset_notice_decide(bool active,bool watch,bool stale,bool remember,
    const char *signal,const char *hint,const char *previous_signal,const char *previous_hint)
{
    bool changed=signal[0]&&strcmp(signal,previous_signal);
    bool hint_changed=hint[0]&&strcmp(hint,previous_hint);
    return (reset_notice_decision_t){
        .reset=changed&&active&&!stale,
        .watch=hint_changed&&watch&&!stale,
        .remember_reset=changed&&remember&&!stale,
        .remember_watch=hint_changed&&watch&&!stale,
    };
}
