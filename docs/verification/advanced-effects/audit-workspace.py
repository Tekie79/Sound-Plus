from pathlib import Path
import json,hashlib,sqlite3,sys,xml.etree.ElementTree as E
base=Path(Path('build/advanced-effects/protected-path.txt').read_text().strip());m=json.loads((base/'manifest.json').read_text());work=Path(m['workspace']);sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
media={p:Path(p).is_file() and sha(Path(p))==h for p,h in m['files'].items()}
state={};exceptions={}
for p in base.rglob('*'):
 if not p.is_file() or p.name in ['manifest.json','library.sqlite','library.sqlite-wal','library.sqlite-shm']:continue
 rel=p.relative_to(base);q=work/rel
 if q.is_file() and sha(q)==sha(p):state[str(rel)]='byte-identical';continue
 if p.suffix=='.json' and q.is_file():
  a=json.loads(p.read_text());b=json.loads(q.read_text());a.pop('updated',None);b.pop('updated',None)
  if a==b:state[str(rel)]='same payload; recovery metadata updated only';continue
 exceptions[str(rel)]='missing' if not q.exists() else 'changed'
a=sqlite3.connect(f'file:{base / "library.sqlite"}?mode=ro&immutable=1',uri=True);b=sqlite3.connect(f'file:{work / "library.sqlite"}?mode=ro',uri=True)
tables={}
for name in m['tables']:
 query='SELECT * FROM "'+name.replace('"','""')+'"';x=sorted(a.execute(query).fetchall(),key=repr);y=sorted(b.execute(query).fetchall(),key=repr)
 tables[name]={'rows':len(y),'unchanged':x==y,'backupRows':len(x)}
a.close();b.close()
d=json.loads((work/'designer-autosave'/f'{m["document"]}.soundplus').read_text())['payload'];
receipt={'document':m['document'],'expectedRevision':m['revision'],'actualRevision':d['revision'],'mediaFiles':len(media),'mediaUnchanged':all(media.values()),'failedMedia':[p for p,ok in media.items() if not ok],'retainedStateFiles':len(state),'state':state,'stateExceptions':exceptions,'tables':tables,'catalogUnchanged':all(v['unchanged'] for v in tables.values()),'latestPayloadUnchanged':json.loads((base/'designer-autosave'/f'{m["document"]}.soundplus').read_text())['payload']==d}
Path(sys.argv[1]).write_text(json.dumps(receipt,indent=2)+'\n');print({k:receipt[k] for k in ['actualRevision','mediaFiles','mediaUnchanged','retainedStateFiles','stateExceptions','catalogUnchanged','latestPayloadUnchanged']});assert receipt['mediaUnchanged'] and receipt['catalogUnchanged'] and receipt['latestPayloadUnchanged']
