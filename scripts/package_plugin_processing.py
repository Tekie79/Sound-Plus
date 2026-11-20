#!/usr/bin/env python3
"""Package only generated QA fixture evidence, preserving immutable original JSON."""
import argparse,hashlib,json,pathlib,shutil,struct,tempfile,zipfile
sha=lambda b:hashlib.sha256(b).hexdigest()
p=argparse.ArgumentParser();p.add_argument('fixture',type=pathlib.Path);p.add_argument('artifacts',type=pathlib.Path);p.add_argument('archive',type=pathlib.Path);args=p.parse_args()
with tempfile.TemporaryDirectory() as tmp:
 root=pathlib.Path(tmp);fixture=args.fixture
 for name in ['source.wav','input.wav','processed.wav','result.json','job.json','receipt.json','processing.soundplus-process']:shutil.copy2(fixture/name,root/name)
 result=json.loads((root/'result.json').read_text());receipt=json.loads((root/'receipt.json').read_text());mapping={result['input']['selection']['source']['path']:'source.wav',result['input']['path']:'input.wav',result['path']:'processed.wav'}
 for item in [receipt]+receipt['files']:
  name='outputs/'+pathlib.Path(item['path']).name;(root/name).parent.mkdir(exist_ok=True);shutil.copy2(item['path'],root/name);mapping[item['path']]=name
 (root/'index.json').write_text(json.dumps({'pathMap':mapping},indent=2)+'\n');shutil.copy2(args.artifacts,root/'artifacts.json');shutil.copy2('scripts/audit_plugin_processing.py',root/'audit.py');shutil.copy2('tests/PluginFixture.cpp',root/'fixture-source.cpp')
 # Independent literal expected gain/polarity, preserving the generated ordered WAV header.
 wav=bytearray((root/'input.wav').read_bytes());pos=12
 while pos+8<=len(wav):
  tag,n=struct.unpack_from('<4sI',wav,pos)
  if tag==b'data':
   for offset in range(pos+8,pos+8+n,4):struct.pack_into('<f',wav,offset,-.5*struct.unpack_from('<f',wav,offset)[0])
   break
  pos+=8+n+(n&1)
 (root/'expected.wav').write_bytes(wav)
 artifacts=json.loads(args.artifacts.read_text());files={str(f.relative_to(root)):sha(f.read_bytes()) for f in sorted(root.rglob('*')) if f.is_file()};(root/'manifest.json').write_text(json.dumps({'version':1,'compiledSourceTreeSha256':artifacts['compiledSourceTreeSha256'],'files':files},indent=2)+'\n')
 args.archive.parent.mkdir(parents=True,exist_ok=True)
 with zipfile.ZipFile(args.archive,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
  for f in sorted(root.rglob('*')):
   if f.is_file():z.write(f,str(f.relative_to(root)))
 print(json.dumps({'archive':str(args.archive),'sha256':sha(args.archive.read_bytes()),'bytes':args.archive.stat().st_size,'members':len(files)+1},indent=2))
