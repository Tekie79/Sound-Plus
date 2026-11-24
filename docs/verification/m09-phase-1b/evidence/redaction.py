"""Scan only synthetic fixture artifacts, including exact undo/project/package states."""
import sys,hashlib,json,base64
from pathlib import Path
root,fixture,pcm=map(Path,sys.argv[1:])
secret='fixture-secret-NEVER-PERSIST'
needles=[secret.encode(),secret.encode('utf-16-le'),secret.encode('utf-16-be'),base64.b64encode(secret.encode())]
def hit(raw):return any(n in raw for n in needles)
assert all(hit(b'prefix'+n+b'suffix') for n in needles)
e=root/'evidence'
groups={'jobs, catalog and journal':sorted(p for p in fixture.rglob('*') if p.is_file()),
        'undo and redo snapshots':[pcm/f'generated-{s}.soundplus' for s in ('edited','undo','redo')],
        'final audio export':[pcm/'generated-export.wav'],
        'exported portable project':sorted(p for p in (pcm/'generated-package').rglob('*') if p.is_file()),
        'test logs':[e/'sfx.log',e/'diagnostic.json']}
diag=json.loads((e/'diagnostic.json').read_bytes())
for p in (Path.home()/'Library/Logs/DiagnosticReports').glob('SoundPlusTests*.ips'):
    if hashlib.sha256(p.read_bytes()).hexdigest()==diag['rawSha256']:
        groups['raw OS crash report']=[p];break
assert 'raw OS crash report' in groups
result={}
for group,paths in groups.items():
    assert paths
    result[group]=[]
    for p in paths:
        raw=p.read_bytes();assert not hit(raw),f'Canary in {group}'
        result[group].append({'file':p.name,'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest()})
print(json.dumps({'matched':0,'positiveEncodingControls':len(needles),'files':result},indent=2))
