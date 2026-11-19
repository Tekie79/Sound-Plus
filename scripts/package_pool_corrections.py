#!/usr/bin/env python3
"""Package only the owned correction fixture's explicit relative file inventory."""
import argparse,hashlib,json,pathlib,zipfile
p=argparse.ArgumentParser(description=__doc__);p.add_argument('fixture',type=pathlib.Path);p.add_argument('identity',type=pathlib.Path);p.add_argument('output',type=pathlib.Path);a=p.parse_args()
root=a.fixture.resolve();index=json.loads((root/'index.json').read_text());d=json.loads((root/'document.soundplus').read_text())['payload'];assert d['name']=='Pool corrections QA' and index['physicalInputs']==[0,3]
files={'document.soundplus','index.json','pcm/consumed.wav','pcm/replay.wav'}|set(index['pathMap'].values())
identity=json.loads(a.identity.read_text());payload={}
for name in sorted(files):
 path=(root/name).resolve();assert path.is_relative_to(root) and path.is_file()
 payload[name]=path.read_bytes()
payload['artifacts.json']=a.identity.read_bytes()
payload['audit_pool_corrections.py']=(pathlib.Path(__file__).parent/'audit_pool_corrections.py').read_bytes()
assert sum(map(len,payload.values()))<64*1024*1024
manifest={'kind':'SoundPlus v1.5.1 owned generated QA fixture; no user session/media','compiledSourceTreeSha256':identity['compiledSourceTreeSha256'],'files':{name:hashlib.sha256(data).hexdigest() for name,data in payload.items()},'notes':['Referenced document paths retain their original immutable meaning; index.json maps to relative archive members for this audit.','This archive contains raw consumed/replay and finalized output bytes. Native receiver and human listening qualifications remain separate.']}
payload['manifest.json']=(json.dumps(manifest,indent=2)+'\n').encode();a.output.parent.mkdir(parents=True,exist_ok=True)
with zipfile.ZipFile(a.output,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=9) as z:
 for name,data in payload.items():z.writestr(name,data)
print(json.dumps({'archive':str(a.output),'bytes':a.output.stat().st_size,'members':len(payload),'sha256':hashlib.sha256(a.output.read_bytes()).hexdigest()},indent=2))
