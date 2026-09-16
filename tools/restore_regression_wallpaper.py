"""One-shot rollback of the identified test-created wallpaper; run on server."""
import hashlib,json,sys
from pathlib import Path
sys.path.insert(0,'/opt/esp32-wallpaper')
import artwork_library as a

original='97db003d07a5f139743bf4cb2a864c94'
accidental='ebbf6689886ce004edcbbc82ace7ca5a'
old=a.get(original);new=a.get(accidental)
assert new['title']=='。' and new['created'].startswith('2026-09-16T04:01:20')
raw=a.read(original)
assert hashlib.sha256(raw).hexdigest()=='aaa99456766ba2736d93568d89422eac063d23d13f3c044172714f9b1cd86038'
state=a.ROOT.parent
assert (state/'current.rgb565').read_bytes()==a.read(accidental)
with a.connect() as db:
    assert [r[0] for r in db.execute('SELECT id FROM selections')]==[accidental]
    a.atomic(state/'current.rgb565',raw)
    a.atomic(state/'current.png',a.read(original,'png'))
    a.atomic(state/'metadata.json',old['meta'].encode())
    db.execute('DELETE FROM selections WHERE kind=? AND id=?',('wallpaper',accidental))
    db.execute('UPDATE works SET deleted=1 WHERE id=?',(accidental,))
assert (state/'current.rgb565').read_bytes()==raw
print('Restored original current wallpaper and daily policy; test-generated wallpaper kept in recoverable trash.')
