#!/usr/bin/env python3
"""Run native search-to-paint measurements during an eight-layer stock design.

Uses an isolated backup of an existing indexed catalog and generated original
audio. It does not modify the source catalog or its media and makes no sound
relevance claim. Requires the existing optional local semantic model.
"""
import argparse, hashlib, json, math, sqlite3, subprocess, tempfile, wave, array, shutil
from pathlib import Path

def write(path, data):
    path.write_text(json.dumps(data, indent=2) + '\n')

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--catalog',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--app',type=Path,default=Path('build/SoundPlus_artefacts/Release/SoundPlus.app/Contents/MacOS/SoundPlus'))
    p.add_argument('--tests',type=Path,default=Path('build/SoundPlusTests_artefacts/Release/SoundPlusTests'))
    p.add_argument('--interactions',action='store_true',help='Measure continuous real canvas-handler edits during search/indexing')
    p.add_argument('--stock-effects',action='store_true',help='Measure actual inspector stock-effect sliders and selection commands')
    p.add_argument('--performance-controls',action='store_true',help='Exercise the shared blend pad and four macro controls')
    p.add_argument('--waveform-scroll',action='store_true',help='Scroll visible file previews during the native workload')
    p.add_argument('--waveform-cache-from',type=Path,help='Reuse derivative files in a fresh process for warm measurement')
    p.add_argument('--fixture-from',type=Path,help='Reuse an unchanged generated workload document/source identities')
    a=p.parse_args();a.interactions = a.interactions or a.stock_effects or a.performance_controls;out=a.output.resolve()
    if out.exists():raise RuntimeError('Choose a fresh evidence directory')
    out.mkdir(parents=True);catalog=Path(tempfile.mkdtemp(prefix='soundplus-designer-performance-',dir='/private/tmp'))
    source=sqlite3.connect(a.catalog.resolve().as_uri()+'?mode=ro',uri=True)
    dest=sqlite3.connect(catalog/'library.sqlite');source.backup(dest);dest.close();source.close()
    if a.waveform_cache_from:
        shutil.copytree(a.waveform_cache_from,catalog/'waveforms')
    if a.fixture_from:
        (out/'fixture').mkdir()
        shutil.copy2(a.fixture_from/'workload.soundplus',out/'fixture/workload.soundplus')
    else:
        subprocess.run([str(a.tests.resolve()),'--prepare-designer-workload',str(out/'fixture')],check=True)
    sounds=['heavy rain','glass breaking','crickets in the night','a chainsaw running','a metal impact','a door closing','birds singing','water splashing','a quiet room','wind blowing','footsteps on gravel','an engine idling','a small dog barking','fire crackling','waves on a beach','thunder rumbling','a wooden creak','traffic passing','a saw cutting','a crowd talking']
    templates=['{}','The sound of {}','Find {}','A recording of {}','I hear {}','Listen for {}','A close recording of {}','Background: {}','An audio clip of {}','Please find {}']
    queries=[{'text':t.format(s)} for t in templates for s in sounds]
    plan=dict(mode='semantic-designer-perform' if a.performance_controls else 'semantic-designer-stock' if a.stock_effects else 'semantic-designer-edit' if a.interactions else 'semantic-designer',designFile=str(out/'fixture/workload.soundplus'),queryCount=len(queries),queries=queries)
    if a.waveform_scroll:plan['waveformScroll']=True
    if a.interactions:
        incoming=out/'incoming';incoming.mkdir()
        for n in range(30):
            samples=array.array('h',(int(5000*math.sin(i*(.02+n*.001))) for i in range(48000*5)))
            with wave.open(str(incoming/f'new-effect-{n:02}.wav'),'wb') as writer:
                writer.setnchannels(1);writer.setsampwidth(2);writer.setframerate(48000);writer.writeframes(samples.tobytes())
        plan['updateFolder']=str(incoming)
    write(out/'plan.json',plan)
    with (out/'native.log').open('w') as log:
        subprocess.run([str(a.app.resolve()),'--data-dir',str(catalog),'--benchmark-plan',str(out/'plan.json'),'--benchmark-output',str(out/'native.json')],stdout=log,stderr=log,check=True,timeout=180)
    report=json.loads((out/'native.json').read_text())
    if not report['success']:raise RuntimeError(report['error'])
    values=sorted(s['dispatchToPaintMs'] for s in report['samples']);percentiles={k:values[math.ceil(len(values)*q)-1] for k,q in [('p50',.5),('p95',.95),('p99',.99)]}
    end=report['audio'];start=report['controls']['audioAtStart'];wall=report['controls']['playbackWallMs']/1000
    summary=dict(timestamp=report['thermalSamples'][-1] if report['thermalSamples'] else None,appSha256=hashlib.sha256(a.app.read_bytes()).hexdigest(),catalogCopy=str(catalog),queries=len(values),paintMs=percentiles,allQueriesDuringEightLayerPlayback=all(s['auditionActive'] and s['audioId']==end['id'] and s['designLayers']==8 for s in report['samples']),nativePlaybackWallSeconds=wall,audioProcessCpuPercentOfOneCore=(end['cpuSeconds']-start['cpuSeconds'])/wall*100,audio=end,source='Three generated 40-second WAVs: 44.1 kHz mono, 48 kHz four-channel, 96 kHz stereo. Eight active layers, linked rates 1 / 0.875, reverse, fades, 24 layer inserts + two master inserts.',monitorGain=.04 if a.interactions else .05,interface='Visible Designer canvas and inspector alongside compact Library browser; 200 queries x three runs')
    if a.interactions:
        interaction=report['interactions'];heards=[float(s['inputToCallbackMs']) for s in interaction['heardEdits']];visual=list(interaction['pointerToPaintMs'])
        def distribution(values):
            values=sorted(values)
            return dict(n=len(values),**{k:values[math.ceil(len(values)*q)-1] for k,q in [('p50',.5),('p95',.95),('p99',.99)]}) if values else dict(n=0)
        summary['pointerHandlerToPaintMs']=distribution(visual);summary['editToCallbackMs']=distribution(heards)
        summary['editingSamplesWithIndexing']=report['controls']['editingSamplesWithIndexing']
        summary['editingProtocol']='Four seconds of gain-line drags (+/-20 px), then four seconds of fade-duration handle drags (+/-35 px) around a 20-second fade so its envelope affects the current mix; input driver every 25 ms, independent of audio update coalescing. Index 30 generated 5-second mono 48 kHz PCM16 files concurrently.'
        if a.stock_effects:
            summary['editingProtocol']='Ten seconds of real inspector slider notifications at 25 ms intervals: layer high-pass cutoff, delay time, reverb size, master cutoff, master reverb size, two seconds each. Same prepared graph and playback generation throughout, while indexing and searching.'
            summary['selectionCommands']=report['controls']['selectionCommands']
            summary['stockPhaseEditToCallbackMs']={str(phase):distribution([float(x['inputToCallbackMs']) for x in interaction['heardEdits'] if x['stockPhase']==phase]) for phase in range(5)}
        summary['measurementBoundary']=('Actual inspector slider notification -> last coalesced UI edit -> first tagged device callback PCM. No OS pointer or slider-to-paint timing was measured.' if a.stock_effects else 'Visible native canvas mouse-handler driver -> painted frame; last coalesced UI edit -> first tagged device callback PCM.')+' Excludes OS input dispatch and physical DAC/acoustic delay.'
    if a.performance_controls:
        summary['editingProtocol']='Ten seconds of blend-pad position updates and four actual macro slider notifications at 25 ms input intervals; shared parameter coalescing. Eight layers, four blend assignments, five mapped continuous targets; search and 30-file indexing overlap.'
        summary['measurementBoundary']='View control handler / actual slider notification -> last coalesced edit -> first tagged device callback PCM. Excludes OS dispatch and physical acoustic delay.'
        summary['interface']='Visible Perform workspace and compact Library browser, shared transport and output controls'
        summary['continuity']={k:report['controls'].get(k) for k in ['samePlaybackGeneration','sameHelper','sameGraphEpoch','noUnderruns','editingOverlappedIndexing','workspaceSwitchPreservesRecipeAndPlayhead']}
        if not all(summary['continuity'].values()):raise RuntimeError('Performance workload continuity assertion failed: '+str(summary['continuity']))
    with sqlite3.connect(catalog/'library.sqlite') as db:
        summary['catalogFiles']=db.execute('select count(*) from assets').fetchone()[0]
    summary['waveforms']=report.get('waveforms',{})
    summary['rowPreviewPaints']=report.get('rowPreviewPaints',0)
    summary['waveformScrolls']=report.get('waveformScrolls',0)
    summary['pendingWaveformScrolls']=report.get('pendingWaveformScrolls',0)
    summary['waveformScrollPaints']=report.get('waveformScrollPaints',0)
    summary['displayScale']=report.get('displayScale')
    summary['waveformCacheCondition']='Warm derivative files copied; fresh process/in-memory pool' if a.waveform_cache_from else 'Cold derivative directory; OS file cache uncontrolled'
    if a.waveform_scroll and (summary['rowPreviewPaints']==0 or summary['waveformScrolls']<10 or summary['pendingWaveformScrolls']!=600 or summary['waveformScrollPaints']<10):
        raise RuntimeError('Visible waveform/scroll workload did not execute')
    write(out/'summary.json',summary);print(json.dumps(summary,indent=2))

if __name__=='__main__':main()
