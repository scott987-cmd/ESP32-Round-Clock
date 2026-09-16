"""Compile the production C merge function; mutation must break unread semantics."""
import json
import subprocess
import tempfile
from pathlib import Path

header=Path('main/notification_store.h').read_text()
harness=r'''
#include <assert.h>
#include <stdio.h>
int main(void) {
    notification_history_t h={.version=2};
    notification_entry_t e={.state=0,.view=10}; e.key[0]=1;
    strcpy(e.text,"working");
    assert(notification_history_apply(&h,&e)); assert(h.count==1);
    assert(!notification_history_apply(&h,&e)); assert(h.count==1);
    e.state=1; e.unread=1; strcpy(e.text,"done");
    assert(notification_history_apply(&h,&e)); assert(h.count==1 && h.entries[0].unread==1);
    h.entries[0].unread=0;
    assert(!notification_history_apply(&h,&e)); assert(h.entries[0].unread==0);
    e.key[1]=1; /* same text, genuinely distinct source event */
    assert(notification_history_apply(&h,&e)); assert(h.count==2 && h.entries[0].unread==1);
    e.key[1]=0; memset(&h,0,sizeof(h));
    e.state=0; strcpy(e.text,"new work");
    assert(notification_history_apply(&h,&e)); assert(h.count==1 && h.entries[0].state==0);
    for(int i=2;i<=30;++i) { e.key[0]=i; e.state=1; assert(notification_history_apply(&h,&e)); }
    assert(h.count==16);
    int active=0; for(int i=0;i<16;++i) active+=h.entries[i].key[0]==1;
    assert(active==1); /* active work survives history overflow */
    memset(&h,0,sizeof(h));
    for(int i=1;i<=16;++i) {e.key[0]=i; e.state=0; assert(notification_history_apply(&h,&e));}
    notification_history_t before=h; e.key[0]=17;
    assert(!notification_history_apply(&h,&e)); assert(!memcmp(&before,&h,sizeof(h)));
    e.key[0]=8; e.state=2;
    assert(notification_history_apply(&h,&e)); assert(h.count==16 && h.entries[0].state==2);
    e.key[0]=17; e.state=1;
    assert(notification_history_apply(&h,&e)); assert(h.count==16 && h.entries[0].key[0]==17);
    puts("PASS dedup, unread preservation, transitions, bounded history, active protection, full queue update");
}
'''
results=[]
with tempfile.TemporaryDirectory(prefix='round-notice-test-') as folder:
    for mutation in (False,True):
        source=header.replace('#pragma once','')
        if mutation:
            source=source.replace('if(found<history->count && item->state==history->entries[found].state &&',
                                  'if(false && found<history->count && item->state==history->entries[found].state &&')
        exe=str(Path(folder)/('mutant' if mutation else 'production'))
        subprocess.run(['cc','-std=c11','-Wall','-Werror','-x','c','-o',exe,'-'],input=(source+harness).encode(),check=True)
        test=subprocess.run([exe],capture_output=True)
        passed=test.returncode!=0 if mutation else test.returncode==0
        results.append({'mutation':mutation,'passed':passed,'returncode':test.returncode})
        print(test.stdout.decode(),end='')
        print('Mutation rejected as expected' if mutation and passed else 'Production checks passed' if passed else 'FAILED')
        assert passed, test.stderr.decode()
out=Path('artifacts/notifications');out.mkdir(parents=True,exist_ok=True)
(out/'store-results.json').write_text(json.dumps(results,indent=2))
