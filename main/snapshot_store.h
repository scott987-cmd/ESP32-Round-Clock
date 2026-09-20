#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One worker owns each base path. Two slots preserve the previous snapshot if
 * power/storage fails during a write. Payload format version belongs in base. */
typedef struct {uint32_t sequence, captured_at;} snapshot_meta_t;
size_t snapshot_load(const char *base,void *data,size_t capacity,snapshot_meta_t *meta);
bool snapshot_save(const char *base,const void *data,size_t size,uint32_t captured_at);
