#include "snapshot_store.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    char dir[]="/tmp/round-snapshot-XXXXXX";assert(mkdtemp(dir));
    char base[100],slot[112];snprintf(base,sizeof(base),"%s/history",dir);
    char data[32]={0};snapshot_meta_t m;
    assert(!snapshot_load(base,data,sizeof(data),&m));
    assert(snapshot_save(base,"first",6,100));
    assert(snapshot_load(base,data,sizeof(data),&m)==6&&!strcmp(data,"first")&&m.captured_at==100);
    assert(snapshot_save(base,"second",7,200));
    assert(snapshot_load(base,data,sizeof(data),&m)==7&&!strcmp(data,"second")&&m.sequence==2);
    assert(snapshot_save(base,"second",7,300));
    assert(snapshot_load(base,data,sizeof(data),&m)==7&&m.sequence==2&&m.captured_at==200);
    assert(!snapshot_load(base,data,2,&m));
    snprintf(slot,sizeof(slot),"%s.1",base);FILE *f=fopen(slot,"wb");assert(f);fputs("partial write",f);fclose(f);
    assert(snapshot_load(base,data,sizeof(data),&m)==6&&!strcmp(data,"first"));
    assert(snapshot_save(base,"third",6,400));
    f=fopen(slot,"r+b");assert(f);assert(!fseek(f,-2,SEEK_END));fputc('X',f);fclose(f);
    assert(snapshot_load(base,data,sizeof(data),&m)==6&&!strcmp(data,"first"));
    assert(!snapshot_save(base,NULL,5,0));
    assert(!snapshot_save(base,data,65537,0));
    unlink(slot);snprintf(slot,sizeof(slot),"%s.0",base);unlink(slot);rmdir(dir);
    puts("PASS snapshots: cold load, bounded reads, dedup, torn-write and corruption fallback");
}
