#!/usr/bin/env python3
"""Native capacity timings. Synthetic vectors exercise storage/retrieval, never sound relevance."""
import argparse, array, hashlib, json, math, random, shutil, sqlite3, subprocess, tempfile, time, wave
from pathlib import Path

def write(path,value):path.write_text(json.dumps(value,indent=2)+'\n')
def stats(v):
    v=sorted(v);return dict(n=len(v),**{k:v[max(0,math.ceil(len(v)*q)-1)] if v else None for k,q in [('p50',.5),('p95',.95),('p99',.99)]})
def prepare(out,count,tests):
    if out.exists():raise RuntimeError('Use a fresh fixture directory')
    out.mkdir(parents=True);dbpath=out/'catalog/library.sqlite';dbpath.parent.mkdir()
    subprocess.run([str(tests),'--open-catalog',str(dbpath)],check=True,capture_output=True)
    base=Path(tempfile.mkdtemp(prefix='soundplus-capacity-',dir='/private/tmp'))
    bank=[];rng=random.Random(9417)
    centers=[[rng.gauss(0,1) for _ in range(512)] for _ in range(64)]
    for n in range(4096):
        v=[centers[n%64][d]+.15*rng.gauss(0,1) for d in range(512)];scale=math.sqrt(sum(x*x for x in v))
        bank.append(array.array('f',[x/scale for x in v]).tobytes())
    m=json.loads(Path('resources/clap-model.json').read_text());assets=math.ceil(count/50)
    roots=[('capacity-main','main'),('capacity-narrow','narrow'),('capacity-offline','offline'),('capacity-empty','empty')]
    db=sqlite3.connect(dbpath);db.execute('PRAGMA foreign_keys=ON');db.execute('BEGIN')
    for ident,name in roots:
        (base/name).mkdir();db.execute('INSERT INTO roots(id,path,volume,volume_relative,online) VALUES(?,?,\'\',\'\',?)',(ident,str(base/name),name!='offline'))
    for n in range(assets):
        r=2 if n%100==99 else 1 if n%100==98 else 0;ident,name=roots[r];relative=f'item-{n:05d}.wav';f=base/name/relative
        with wave.open(str(f),'wb') as w:w.setparams((1,2,48000,0,'NONE',''));w.writeframes(b'\0\0'*12000)
        st=f.stat();aid=f'{n+1:032x}';windows=min(50,count-n*50);samples=(windows+1)*5*48000
        db.execute('INSERT INTO assets(id,root_id,relative,name,size,modified,inode,revision,duration,channels,sample_rate,state,online,seen,parent,samples) VALUES(?,?,?,?,?,?,?,1,?,1,48000,\'ready\',1,1,\'\',?)',
                   (aid,ident,relative,relative,st.st_size,st.st_mtime_ns,st.st_ino,samples/48000,samples))
        db.execute('INSERT INTO analysis_jobs(asset_id,revision,root_id,model,preprocessing,state,next_start,max_end,rate) VALUES(?,1,?,?,?,\'complete\',?,?,48000)',(aid,ident,m['id'],m['preprocessing'],samples,samples))
        db.executemany('INSERT INTO embeddings VALUES(?,1,?,?,?,?,48000,?)',[(aid,m['id'],m['preprocessing'],k*240000,k*240000+480000,bank[(n*50+k)%4096]) for k in range(windows)])
    for ident,name in roots:
        st=(base/name).stat();db.execute('INSERT INTO directories VALUES(?,\'\',?,?,\'[]\')',(ident,st.st_mtime_ns,st.st_ino))
    db.commit();db.execute('PRAGMA wal_checkpoint(TRUNCATE)');db.close()
    (base/'offline').rename(base/'offline-retained')
    audition=base/'audition.wav'
    with wave.open(str(audition),'wb') as w:w.setparams((1,2,48000,0,'NONE',''));w.writeframes((b'\0\x08\0\xf8'*24000)*180)
    info=dict(regions=count,assets=assets,catalog=str(dbpath.resolve()),sourceDirectory=str(base),auditionFile=str(audition),
              method='Seed 9417; 64 isotropic Gaussian 512-D centers; 4096 normalized vectors, center + 0.15 Gaussian noise; cyclic exact repeats across regions; float32. Capacity data, not audio embeddings.',
              distinctVectors=min(count,4096),copiesPerVector=count/4096,regionsPerAsset=50,
              media='Physical 0.25 s silent WAV placeholders; catalog source lengths/windows synthetic; never used for relevance or candidate playback. Separate 180 s square-wave audition fixture.',
              filters='98% main, 1% narrow, 1% offline by asset; fourth source empty. Offline vectors remain eligible.',sqliteBytes=dbpath.stat().st_size)
    write(out/'fixture.json',info);return info
def run(app,fixture,out,queries=200,updates=False,audio_fixture=Path('build/semantic/real-effects/evaluation/manifest.json')):
    if out.exists():raise RuntimeError('Use a fresh run directory')
    out.mkdir(parents=True);manifest=json.loads(audio_fixture.read_text())
    positives=[q['query'] for q in manifest['queries'] if q['expectedFiles']]
    templates=['{}','A recording of {}','Find {}','Listen for {}','The sound of {}','An audio clip of {}','I hear {}','A close recording of {}','Background: {}','Please find {}']
    rows=[{'text':t.format(q),'source':('capacity-empty' if i%20==19 else 'capacity-narrow' if i%5==4 else '')} for i,(t,q) in enumerate((t,q) for t in templates for q in positives)][:queries]
    plan=dict(mode='semantic-capacity',queryCount=queries,queries=rows,expectedRegions=fixture['regions'],auditionFile=fixture['auditionFile'])
    if updates:
        media=Path(manifest['mediaDirectory']);folder=Path(tempfile.mkdtemp(prefix='soundplus-capacity-updates-',dir='/private/tmp'))
        for n in range(120):shutil.copyfile(media/f'clip-{n%40+1:03d}.wav',folder/f'update-{n:03d}.wav')
        plan['updateFolder']=str(folder)
    write(out/'plan.json',plan)
    cmd=[str(app),'--data-dir',str(Path(fixture['catalog']).parent),'--benchmark-plan',str((out/'plan.json').resolve()),'--benchmark-output',str((out/'native.json').resolve())]
    start=time.monotonic()
    with (out/'native.log').open('w') as log:subprocess.run(cmd,stdout=log,stderr=log,check=True,timeout=1800)
    report=json.loads((out/'native.json').read_text());assert report['success'],report['error']
    result=dict(appSha256=hashlib.sha256(app.read_bytes()).hexdigest(),fixture=fixture,wallSeconds=time.monotonic()-start,
                firstSemanticPaintMs=report['firstSemanticPaintMs'],modelLoadMs=report['semantic']['modelLoadMs'],mainPeakRSS=report['counters']['peakResidentBytes'],
                index=report['semantic'],workload={w:{'paintMs':stats([s['dispatchToPaintMs'] for s in report['samples'] if s['workload']==w]),
                    **{k:stats([s[k] for s in report['samples'] if s['workload']==w]) for k in ['fetchFilterMs','vectorScoreMs','rankGroupMs','fusionMs']},
                    'pendingAndPlaying':sum(s['analysisPending']>0 and s['auditionActive'] for s in report['samples'] if s['workload']==w),
                    'newRegionHits':sum(s.get('newRegionHit',False) for s in report['samples'] if s['workload']==w)} for w in ['capacity-warm','capacity-audition','capacity-updates']},
                filtered={f:stats([s['dispatchToPaintMs'] for s in report['samples'] if s.get('sourceFilter','')==f]) for f in ['capacity-narrow','capacity-empty']},audio=report['audio'])
    write(out/'summary.json',result);print(json.dumps(result,indent=2),flush=True)
def main():
    p=argparse.ArgumentParser();sub=p.add_subparsers(dest='mode',required=True)
    a=sub.add_parser('prepare');a.add_argument('--regions',type=int,required=True);a.add_argument('--output',type=Path,required=True);a.add_argument('--tests',type=Path,default=Path('build/SoundPlusTests_artefacts/Release/SoundPlusTests'))
    a=sub.add_parser('run');a.add_argument('--app',type=Path,required=True);a.add_argument('--fixture',type=Path,required=True);a.add_argument('--output',type=Path,required=True);a.add_argument('--queries',type=int,default=200);a.add_argument('--updates',action='store_true');a.add_argument('--audio-fixture',type=Path,default=Path('build/semantic/real-effects/evaluation/manifest.json'))
    a=p.parse_args()
    if a.mode=='prepare':print(json.dumps(prepare(a.output,a.regions,a.tests.resolve()),indent=2))
    else:run(a.app.resolve(),json.loads(a.fixture.read_text()),a.output,a.queries,a.updates,a.audio_fixture)
if __name__=='__main__':main()
