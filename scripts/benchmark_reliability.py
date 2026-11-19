#!/usr/bin/env python3
"""Owned real-WAV corpus and visible native reliability workload; never mutates originals."""
import argparse,array,hashlib,json,math,shutil,sqlite3,subprocess,time,wave
from pathlib import Path

def write(p,v):p.write_text(json.dumps(v,indent=2)+'\n')
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def wav(p,rate,channels,seconds,kind,seed):
    p.parent.mkdir(parents=True,exist_ok=True);frames=round(rate*seconds)
    with wave.open(str(p),'wb') as w:
        w.setnchannels(channels);w.setsampwidth(2);w.setframerate(rate)
        for start in range(0,frames,8192):
            data=array.array('h')
            for i in range(start,min(start+8192,frames)):
                value=0 if kind==0 else (22000 if i%(rate*2+seed)==seed else 0) if kind==1 else int(5000*math.sin(i*(.02+seed*.0003))*(.4+.6*math.sin(i/rate*.7)**2))
                for c in range(channels):data.append(value if c==0 or kind!=1 else -value if c==1 else 0)
            w.writeframes(data.tobytes())

def prepare(a):
    root=a.output.resolve()
    if root.exists():raise RuntimeError('Use a fresh owned fixture directory')
    root.mkdir(parents=True);templates=root/'templates';corpus=root/'corpus';corpus.mkdir()
    pool=[]
    for n in range(128):
        p=templates/f'template-{n}.wav';wav(p,[8000,22050,44100,48000,96000][n%5],[1,2,4][n%3],[.125,.25,.5,1.][n%4],n%4,n);pool.append(p)
    rows=[]
    for n in range(a.files):
        name='duplicate effect.wav' if n%11==0 else ('long-effect-name-'*8+f'{n:05}.wav') if n%97==0 else f'effect-{n:05}-'+['impact','rain','metal','room'][n%4]+'.wav'
        p=corpus/f'bank-{n//100:03}'/(f'take-{n:05}/'+name if n%11==0 else name);p.parent.mkdir(parents=True,exist_ok=True)
        if n<24:wav(p,[8000,44100,48000][n%3],[1,2,4][n%3],3600 if n==0 else [15,30,60,120][n%4],n%4,n)
        else:shutil.copyfile(pool[n%len(pool)],p)
        with wave.open(str(p)) as w:rows.append({'relative':str(p.relative_to(root)),'rate':w.getframerate(),'channels':w.getnchannels(),'frames':w.getnframes(),'bytes':p.stat().st_size,'sha256':digest(p)})
    assert len(set(r['relative'] for r in rows))==a.files and len(list(corpus.rglob('*.wav')))==a.files
    offline=root/'offline';offline.mkdir();shutil.copyfile(pool[5],offline/'offline effect.wav')
    batches=[]
    for b in range(64):
        d=root/'incoming'/f'batch-{b:03}';d.mkdir(parents=True)
        for n in range(16):shutil.copyfile(pool[(b*16+n)%128],d/f'added-effect-{n:03}.wav')
        batches.append(str(d))
    subprocess.run([str(a.tests.resolve()),'--prepare-reliability-workload',str(root/'design')],check=True)
    write(root/'manifest.json',{'kind':'SoundPlus owned generated reliability corpus','files':len(rows),'corpus':str(corpus),'offline':str(offline),'rows':rows,'totalCorpusBytes':sum(r['bytes'] for r in rows),'indexBatches':batches,'recipe':'128 short PCM templates copied to real independent files (intentional repeated PCM); 24 independently generated long WAVs including 1 hour, silence, sparse impulses, sine/envelopes. 8/22.05/44.1/48/96 kHz mono/stereo/4ch. 40-minute active design source. No metadata-only assets.'})
    print(json.dumps({'files':len(rows),'bytes':sum(r['bytes'] for r in rows),'fixture':str(root)},indent=2))

def run(a):
    root=a.fixture.resolve();manifest=json.loads((root/'manifest.json').read_text());out=a.output.resolve()
    if len(set(r['relative'] for r in manifest['rows']))!=manifest['files'] or len(list((root/'corpus').rglob('*.wav')))!=manifest['files']:raise RuntimeError('Corpus manifest/filesystem count mismatch')
    if out.exists():raise RuntimeError('Use a fresh evidence directory')
    out.mkdir(parents=True);catalog=out/'catalog';catalog.mkdir()
    if a.warm_from:
        previous=a.warm_from.resolve();src=sqlite3.connect((previous/'catalog/library.sqlite').as_uri()+'?mode=ro',uri=True);dest=sqlite3.connect(catalog/'library.sqlite');src.backup(dest);src.close()
        # Only this owned benchmark's incoming roots are excluded for equivalent repeated indexing.
        roots=[r[0] for r in dest.execute('select path from roots')]
        if not all(str(root) in p for p in roots):raise RuntimeError('Warm source contains non-fixture roots')
        dest.execute('PRAGMA foreign_keys=ON')
        extra=[r[0] for r in dest.execute('select id from roots where path not in (?,?)',(manifest['corpus'],manifest['offline']))]
        for root_id in extra:
            dest.execute('delete from assets where root_id=?',(root_id,))
            dest.execute('delete from directories where root_id=?',(root_id,))
            dest.execute('delete from roots where id=?',(root_id,))
        if dest.execute('pragma foreign_key_check').fetchall():raise RuntimeError('Warm catalog integrity failed')
        dest.commit();dest.close()
        shutil.copytree(previous/'catalog/waveforms',catalog/'waveforms')
    plan={'recordTake':a.record_take,'mode':'semantic-designer-reliability','durationMs':a.seconds*1000,'bufferFrames':a.buffer_frames,'cycleMs':a.cycle_ms,'designFile':str(root/'design/workload.soundplus'),'corpusFolder':manifest['corpus'],'corpusFiles':manifest['files'],'offlineFolder':manifest['offline'],'indexBatches':manifest['indexBatches']}
    write(out/'plan.json',plan)
    result=None
    try:
        with (out/'native.log').open('w') as log:
            result=subprocess.run([str(a.app.resolve()),'--data-dir',str(catalog),'--benchmark-plan',str(out/'plan.json'),'--benchmark-output',str(out/'native.json')],stdout=log,stderr=log,timeout=a.seconds+360)
    finally:
        offline=Path(manifest['offline']);absent=offline.with_name(offline.name+'.disconnected')
        if absent.exists() and not offline.exists():absent.rename(offline)
    if not (out/'native.json').exists():
        write(out/'summary.json',{'success':False,'error':'Application exited without a final workload report','exitCode':result.returncode if result else None})
        raise RuntimeError('Application exited without final workload report')
    report=json.loads((out/'native.json').read_text());summary={k:report.get(k) for k in ['success','error','elapsedMs','audio','audioHealth','waveforms','counters','semantic']};summary['reliability']=report['reliability'];summary['reliability'].pop('progress',None)
    for key in ['queries','selectedWaveforms','viewports']:summary['reliability'].pop(key,None)
    summary.update({'appSha256':digest(a.app),'corpusFiles':manifest['files'],'corpusBytes':manifest['totalCorpusBytes'],'condition':'warm derivatives, fresh process/catalog copy; OS file cache uncontrolled' if a.warm_from else 'cold derivative directory; OS file cache uncontrolled','exitCode':result.returncode})
    with sqlite3.connect(catalog/'library.sqlite') as db:summary['catalogFilesAtEnd']=db.execute('select count(*) from assets').fetchone()[0]
    if a.record_take:
        document=report.get('recordedTakeDocument',{});take=next((t for t in document.get('takes',[]) if t['id']==document.get('activeTake')),None)
        summary['recording']={'take':{k:take.get(k) for k in ['id','name','complete','kept','bodyEnd','outputFrames','captureRate','recipeHash','eventHash']} if take else None,'events':len(take.get('events',[])) if take else 0,'namedDocument':str(out/'Sustained QA Take.soundplus'),'outputPath':report.get('takeOutput'),'sourceFingerprints':take.get('sources',[]) if take else []}
    write(out/'summary.json',summary);print(json.dumps(summary,indent=2))
    if not report['success']:raise RuntimeError(report['error'])

p=argparse.ArgumentParser();sub=p.add_subparsers(dest='action',required=True)
q=sub.add_parser('prepare');q.add_argument('--output',type=Path,required=True);q.add_argument('--files',type=int,default=10000);q.add_argument('--tests',type=Path,default=Path('build/SoundPlusTests_artefacts/Release/SoundPlusTests'))
q=sub.add_parser('run');q.add_argument('--fixture',type=Path,required=True);q.add_argument('--output',type=Path,required=True);q.add_argument('--seconds',type=int,default=120);q.add_argument('--cycle-ms',type=int,default=1500);q.add_argument('--buffer-frames',type=int,choices=[256,512],default=512);q.add_argument('--warm-from',type=Path);q.add_argument('--record-take',action='store_true');q.add_argument('--app',type=Path,default=Path('build/SoundPlus_artefacts/Release/SoundPlus.app/Contents/MacOS/SoundPlus'))
a=p.parse_args();prepare(a) if a.action=='prepare' else run(a)
