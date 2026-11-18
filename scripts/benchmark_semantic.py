#!/usr/bin/env python3
"""Native semantic paint benchmark, real-effect evaluation and per-process offline replay."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import time

PROFILE='(version 1)(allow default)(deny network*)'

def write(path,value):path.write_text(json.dumps(value,indent=2,ensure_ascii=False)+'\n')
def stats(values):
    values=sorted(values)
    return {'n':len(values),**{key:(values[max(0,math.ceil(len(values)*q)-1)] if values else None) for key,q in [('p50',.5),('p95',.95),('p99',.99)]}}

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--app',type=Path,required=True)
    parser.add_argument('--model',type=Path,required=True);parser.add_argument('--fixture',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    app=args.app.resolve();model=args.model.resolve();manifest=json.loads(args.fixture.read_text());out=args.output.resolve()
    if out.exists() and any(out.iterdir()):raise RuntimeError('Choose a fresh output directory')
    out.mkdir(parents=True,exist_ok=True)
    templates=['{}','A recording of {}','Find the sound of {}','Listen for {}','An audio clip of {}',
               'The sound of {}','I am looking for {}','A close recording of {}','There is {}','Background sound: {}']
    positives=[q for q in manifest['queries'] if q['expectedFiles']]
    queries=[]
    for template in templates:
        for q in positives:
            text=q['query'] if template=='{}' else template.format(q['query'][0].lower()+q['query'][1:])
            queries.append({'text':text,'source':'media' if len(queries)%25==24 else ''})
    assert len(queries)==200
    evaluation=[{'text':q['query'],'source':''} for q in manifest['queries']]
    common={'expectedFiles':manifest['fileCount'],'trace':True,'queryCount':200,'queries':queries,'evaluationQueries':evaluation,
            'longFilename':'clip-041.wav','matchQuery':'A wooden door slowly creaking as it moves.',
            'eventStartSeconds':20,'eventEndSeconds':25}
    def run(mode,catalog,add=False,offline=False):
        plan={**common,'mode':mode}
        if mode in ['semantic-reopen','semantic-offline']:plan.update(queryCount=len(evaluation),queries=evaluation)
        write(out/(mode+'-plan.json'),plan)
        command=[str(app),'--data-dir',str(catalog),'--model-dir',str(model),'--benchmark-plan',str(out/(mode+'-plan.json')),
                 '--benchmark-output',str(out/(mode+'.json'))]
        if add:command+=['--add-folder',manifest['mediaDirectory']]
        if offline:command=['/usr/bin/sandbox-exec','-p',PROFILE]+command
        launch=time.monotonic()*1000
        with (out/(mode+'.log')).open('w') as log:
            process=subprocess.Popen(command,stdout=log,stderr=log)
            peak_disk=0;deadline=time.monotonic()+240
            while process.poll() is None:
                peak_disk=max(peak_disk,sum(f.stat().st_size for f in catalog.glob('library.sqlite*') if f.is_file()))
                if time.monotonic()>deadline:process.kill();process.wait();raise RuntimeError(f'{mode} timed out; inspect its log')
                time.sleep(.05)
        if process.returncode:raise RuntimeError(f'{mode} exited {process.returncode}; inspect its log')
        report=json.loads((out/(mode+'.json')).read_text())
        if not report['success']:raise RuntimeError(report['error'])
        report['launchToFirstSemanticPaintMs']=(report['startMonotonicMs']+report['firstSemanticPaintMs']-launch) if report['firstSemanticPaintMs'] else None
        report['peakCatalogAndWalBytesSampled50ms']=peak_disk
        write(out/(mode+'.json'),report)
        print(mode,'passed',flush=True)
        return report
    full=run('semantic',out/'catalog',True)
    pure=run('semantic-analyze',out/'throughput-catalog',True)
    reopened=run('semantic-reopen',out/'catalog')
    with (out/'network-denial-probe.log').open('w') as log:
        probe=subprocess.run(['/usr/bin/sandbox-exec','-p',PROFILE,'/usr/bin/curl','--connect-timeout','2','-I','https://huggingface.co'],stdout=log,stderr=log)
    if probe.returncode==0:raise RuntimeError('The network-denial probe unexpectedly connected')
    offline=run('semantic-offline',out/'catalog',offline=True)
    def group(workload,key='dispatchToPaintMs'):
        return stats([s[key] for s in full['samples'] if s['workload']==workload])
    cases=[]
    for query,sample in zip(manifest['queries'],[s for s in full['samples'] if s['workload']=='evaluation']):
        hits=sample['hits'];rank=next((i+1 for i,h in enumerate(hits[:10]) if h['file'] in query['expectedFiles']),None)
        overlap=[]
        for expected in query['expectedRegions']:
            for hit in hits[:10]:
                if hit['file']!=expected['file']:continue
                for region in hit['ranges']:
                    first=region['startSample']/hit['sourceRate'];last=region['endSample']/hit['sourceRate']
                    if first<expected['endSeconds'] and last>expected['startSeconds']:
                        overlap.append({'file':hit['file'],'expected':[expected['startSeconds'],expected['endSeconds']],
                                        'returned':[first,last]});break
        cases.append({**query,'retrieved':hits,'expectedHitRankWithin10':rank,'longRegionOverlapsWithin10':overlap,
                      'listenerJudgments':query.get('listenerJudgments',[]),'judgmentStatus':'ANNOTATION_PROXY_ONLY; listening review pending'})
    write(out/'relevance.json',{'corpus':str(args.fixture.resolve()),'queries':cases,'humanListenerCount':0,
        'positiveQueries':len(positives),'annotationHitAt10':sum(c['expectedHitRankWithin10'] is not None for c in cases if c['expectedFiles'])/len(positives),
        'annotationHitAt1':sum(c['expectedHitRankWithin10']==1 for c in cases if c['expectedFiles'])/len(positives),
        'qualification':'43-file workflow set; unknown CLAP training overlap; does not satisfy 200 held-out queries/two listeners'})
    ai=pure['semantic'];seconds=(ai['lastWindowPublishedMs']-ai['firstWindowStartedMs'])/1000
    summary={'appSha256':hashlib.sha256(app.read_bytes()).hexdigest(),'model':ai['model'],'preprocessing':ai['preprocessing'],
        'fixture':{k:manifest[k] for k in ['fileCount','audioSeconds','bytes','revision','license']},
        'environment':{'os':subprocess.check_output(['sw_vers'],text=True).strip(),'cpu':subprocess.check_output(['sysctl','-n','machdep.cpu.brand_string'],text=True).strip(),
            'model':subprocess.check_output(['sysctl','-n','hw.model'],text=True).strip(),'ramBytes':int(subprocess.check_output(['sysctl','-n','hw.memsize'],text=True)),
            'configuration':'Release arm64; ORT 1.22.1 CPU/MLAS, two intra-op threads; no GPU/ANE used'},
        'cacheState':'Fresh processes/catalogs for import; existing catalog for reopen/offline; model OS cache uncontrolled; warm query runs repeat three times',
        'uiSubmitToSemanticPaintMs':{w:group(w) for w in ['analyzing-and-auditioning','warm-semantic','audition-semantic']},
        'perRunWarm':[stats([s['dispatchToPaintMs'] for s in full['samples'] if s['workload']=='warm-semantic' and s['run']==run]) for run in range(3)],
        'warmComponentsMs':{key:group('warm-semantic',key) for key in ['textPreprocessMs','textEncodeMs','workerRoundTripMs','fetchFilterMs','vectorScoreMs','rankGroupMs','fusionMs']},
        'warmTextCacheHits':sum(s['textCacheHit'] for s in full['samples'] if s['workload']=='warm-semantic'),
        'modelLoadMs':[r['semantic']['modelLoadMs'] for r in [full,pure,reopened,offline]],
        'reopenLaunchToFirstSemanticPaintMs':reopened['launchToFirstSemanticPaintMs'],
        'pureAnalysis':{'files':ai['files'],'windows':ai['regions'],'elapsedSeconds':seconds,'windowsPerSecond':ai['windowsThisSession']/seconds,
            'newSourceSeconds':ai['sourceSecondsThisSession'],'sourceAudioHoursPerWallHour':ai['sourceSecondsThisSession']/seconds,
            'overlap':'10-second windows, 5-second stride; last partial window retained; repeatpad to 10 seconds',
            'vectorBytes':ai['vectorBytes'],'mainPeakResidentBytes':pure['counters']['peakResidentBytes'],
            'helperPeakResidentBytes':ai['helperPeakResidentBytes']},
        'nativeControls':full['controls'],'offlineNetworkDenied':True,'networkDenialProfile':PROFILE,
        'offlineSemanticQueries':len(offline['samples']),'reopenCounters':reopened['counters'],
        'reopenWindowsThisSession':reopened['semantic']['windowsThisSession'],
        'fullMainPeakResidentBytes':full['counters']['peakResidentBytes'],'fullAIHelperPeakResidentBytes':full['semantic']['helperPeakResidentBytes'],
        'audioHelperPeakResidentBytes':full['audio'].get('peakResidentBytes'),'fifoStarvation':full['audio']['underruns'],
        'peakCatalogAndWalBytesSampled50ms':max(full['peakCatalogAndWalBytesSampled50ms'],pure['peakCatalogAndWalBytesSampled50ms']),
        'warmTargetP95AtMost300ms':group('warm-semantic')['p95']<=300}
    write(out/'summary.json',summary)
    print(json.dumps({k:summary[k] for k in ['uiSubmitToSemanticPaintMs','pureAnalysis','warmTargetP95AtMost300ms','offlineSemanticQueries']},indent=2))

if __name__=='__main__':main()
