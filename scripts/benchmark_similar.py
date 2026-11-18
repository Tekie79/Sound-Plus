#!/usr/bin/env python3
"""Native Find Similar controls, exact-range reference timings, and real-effect annotation checks."""
import argparse, hashlib, json, math, shutil, subprocess
from pathlib import Path

def write(p,v):p.write_text(json.dumps(v,indent=2)+'\n')
def stats(v):
    v=sorted(v);return dict(n=len(v),**{k:v[max(0,math.ceil(len(v)*q)-1)] if v else None for k,q in [('p50',.5),('p95',.95),('p99',.99)]})
def main():
    p=argparse.ArgumentParser();p.add_argument('--app',type=Path,required=True);p.add_argument('--fixture',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--controls-only',action='store_true',help='Run native reference/transport regression without timing or relevance workloads');a=p.parse_args()
    out=a.output.resolve()
    if out.exists():raise RuntimeError('Use a fresh output directory')
    out.mkdir(parents=True);manifest=json.loads(a.fixture.read_text());media=Path(manifest['mediaDirectory'])
    shutil.copyfile(media/'clip-006.wav',out/'external-short.wav');shutil.copyfile(media/'clip-041.wav',out/'external-long.wav')
    examples=[]
    for query in manifest['queries']:
        if not query['expectedFiles']:continue
        source=next(f for f in query['expectedFiles'] if not next(x for x in manifest['files'] if x['file']==f).get('composite'))
        examples.append(dict(file=source,startSeconds=0,endSeconds=5,category=query['query'],expectedFiles=[f for f in query['expectedFiles'] if f!=source],expectedRegions=query['expectedRegions']))
    plan=dict(mode='semantic-example',queryCount=200,externalFile=str(out/'external-short.wav'),longFile=str(out/'external-long.wav'),examples=examples,transportControls=True,controlsOnly=a.controls_only)
    write(out/'plan.json',plan)
    command=[str(a.app.resolve()),'--data-dir',str(out/'catalog'),'--add-folder',str(media),'--benchmark-plan',str(out/'plan.json'),'--benchmark-output',str(out/'native.json')]
    with (out/'native.log').open('w') as log:subprocess.run(command,stdout=log,stderr=log,check=True,timeout=240)
    native=json.loads((out/'native.json').read_text());assert native['success'],native['error']
    if a.controls_only:
        result=dict(passed=True,appSha256=hashlib.sha256(a.app.read_bytes()).hexdigest(),controls=native['controls'],audio=native['audio'])
        write(out/'summary.json',result);print(json.dumps(result,indent=2));return
    workloads={w:{k:stats([s[k] for s in native['samples'] if s['workload']==w]) for k in ['dispatchToPaintMs','referenceEncodingMs','referenceRetrievalMs','fusionMs']} for w in ['example-uncached','example-cached']}
    evaluated=[]
    for expected,sample in zip(examples,[s for s in native['samples'] if s['workload']=='example-evaluation']):
        hits=sample['hits'];rank=next((i+1 for i,h in enumerate(hits[:10]) if h['file'] in expected['expectedFiles']),None)
        overlaps=[]
        for e in expected['expectedRegions']:
            for h in hits[:10]:
                if h['file']==e['file']:
                    for r in h['ranges']:
                        first=r['startSample']/h['sourceRate'];last=r['endSample']/h['sourceRate']
                        if first<e['endSeconds'] and last>e['startSeconds']:overlaps.append(dict(file=h['file'],expected=[e['startSeconds'],e['endSeconds']],returned=[first,last]));break
        evaluated.append(expected|dict(rankWithinTen=rank,hits=hits,overlappingLongRegions=overlaps,humanJudgments=[],judgmentSource='ESC-50 annotation proxy; human listening review remains separate'))
    result=dict(appSha256=hashlib.sha256(a.app.read_bytes()).hexdigest(),workloads=workloads,controls=native['controls'],model=native['semantic'],
                uncachedActuallyEncoded=all(not s['referenceCacheHit'] for s in native['samples'] if s['workload']=='example-uncached'),
                cachedActuallyReused=all(s['referenceCacheHit'] for s in native['samples'] if s['workload']=='example-cached'),
                referenceCases=evaluated,annotationHitAtTen=sum(e['rankWithinTen'] is not None for e in evaluated)/len(evaluated),mainPeakRSS=native['counters']['peakResidentBytes'],audio=native['audio'])
    write(out/'summary.json',result);print(json.dumps({k:result[k] for k in ['workloads','controls','uncachedActuallyEncoded','cachedActuallyReused','annotationHitAtTen']},indent=2))
if __name__=='__main__':main()
