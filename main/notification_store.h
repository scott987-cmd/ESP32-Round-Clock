#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define NOTICE_LIMIT 16
typedef struct {
    unsigned char key[32];
    char text[192];
    uint32_t view, state, unread, fixture;
    int64_t created;
} notification_entry_t;
typedef struct {
    uint32_t version, count;
    notification_entry_t entries[NOTICE_LIMIT];
} notification_history_t;

/* State 0 is active work. Duplicate deliveries must not restore unread state. */
static inline bool notification_history_apply(notification_history_t *history,
                                              const notification_entry_t *item)
{
    unsigned found=history->count;
    for(unsigned i=0;i<history->count;++i)
        if(!memcmp(item->key,history->entries[i].key,32)) { found=i; break; }
    if(found<history->count && item->state==history->entries[found].state &&
       !strcmp(item->text,history->entries[found].text)) return false;
    if(found==history->count && history->count<NOTICE_LIMIT) ++history->count;
    if(found>=NOTICE_LIMIT) {
        found=NOTICE_LIMIT-1;
        while(found>0 && history->entries[found].state==0) --found;
        if(history->entries[found].state==0) return false;
    }
    memmove(&history->entries[1],&history->entries[0],found*sizeof(*item));
    history->entries[0]=*item;
    return true;
}
