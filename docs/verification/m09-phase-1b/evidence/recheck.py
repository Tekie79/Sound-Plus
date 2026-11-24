"""Independently read attachment bytes, anchor bindings and replay the archived PCM."""
import collections,hashlib,json,subprocess,sys,tempfile,zipfile
from pathlib import Path
base=Path(sys.argv[1]).resolve()
read=lambda p:json.loads(p.read_bytes())
record=read(base/'run.json');anchor=read(base/'run.identity.json')
assert all(record[k]==v for k,v in anchor.items())
refs=set()
for case,row in record['results'].items():
    for ref in row['evidence']:
        path=base/ref['path'];assert path.resolve().is_relative_to(base/'evidence')
        data=path.read_bytes()
        assert len(data)==ref['bytes'] and hashlib.sha256(data).hexdigest()==ref['sha256']
        assert ref['runId']==anchor['runId'] and ref['candidateSha256']==anchor['candidate']['manifestSha256']
        refs.add(path.relative_to(base).as_posix())
assert refs=={p.relative_to(base).as_posix() for p in (base/'evidence').iterdir() if p.is_file()}
with tempfile.TemporaryDirectory() as tmp:
    with zipfile.ZipFile(base/'evidence/generated-pcm.zip') as z:z.extractall(tmp)
    replay=json.loads(subprocess.check_output([sys.executable,'-B',base/'evidence/pcm_oracle.py',tmp]))
    assert replay==read(base/'evidence/pcm.json')
assert len(record['results'])==399
print(anchor['runId'],len(refs),'unique verified files; all PCM replayed;',dict(collections.Counter(row['status'] for row in record['results'].values())))
