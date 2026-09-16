"""Run on the server as espclock. Remove only the two exact test-only objects."""
import sys
sys.path.insert(0,'/opt/esp32-wallpaper')
import artwork_library as library

for ident,title in [('deb394f02cf0a64648322d19423f4c0e','验证用临时作品'),('358dff392a567e960cc2ad582cf752a8','缓存切换验证')]:
    row=library.get(ident,include_deleted=True)
    assert row['title']==title and row['deleted'], 'Refuse to remove a non-test or non-trashed object'
    with library.connect() as db:
        assert not db.execute('SELECT 1 FROM selections WHERE id=?',(ident,)).fetchone()
        db.execute('DELETE FROM works WHERE id=? AND title=? AND deleted=1',(ident,title))
    for suffix in ('data','thumb','png'):(library.ROOT/(ident+'.'+suffix)).unlink(missing_ok=True)
    print('Removed test-only object:',title)
