"""Read-only release check against the real encrypted archive."""
import hashlib,json,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'server'))
import story_service
from device_api_client import request

ident='bb14c7ad79efa805fe4cc3e21b717eb4'
book=json.loads(request('/v1/stories?id='+ident));story_service.validate(book)
for i,node in enumerate(book['nodes']):
    data=request('/v1/stories?id='+ident+'&scene='+str(i))
    assert len(data)==node['bytes'] and hashlib.sha256(data).hexdigest()==node['audio']
bad=json.loads(Path('artifacts/expansion/story-generation/generated.json').read_text())
try:story_service.validate(bad)
except ValueError:pass
else:raise AssertionError('Previously rejected nature story must not pass release validation')
Path('artifacts/expansion/story-release.json').write_text(json.dumps({'id':ident,'book':book,'all_audio_hashes_verified':True,'previous_bad_candidate_rejected':True},ensure_ascii=False,indent=2))
print('PASS real release book, matching illustrations, all five PCM hashes, rejected candidate regression')
