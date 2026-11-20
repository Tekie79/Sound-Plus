#!/usr/bin/env python3
"""Recompute first affected DSP-block and observed UI-publication latency.
Usage: audit_live_insert_events.py events.jsonl [events.jsonl ...]
Only real-device playing, nondeferred, nonopaque rows with matching event clocks
are timed. Repeated captures of one event are counted once per generation.
"""
import json, math, sys
from pathlib import Path
rows=[json.loads(line) for name in sys.argv[1:] for line in Path(name).read_text().splitlines() if line.strip()]
seen=set(); samples=[]
for r in rows:
    t=r.get('eventTicks',0); first=r.get('firstDspEventTicks',0)
    if not r.get('device',{}).get('playing') or r.get('opaqueObservation') or r.get('deferredByGesture') or not t or first!=t: continue
    k=(r['generation'],t)
    if k in seen: continue
    a=r.get('firstDspStartTicks',0); u=r.get('uiReceivedTicks',0); f=r['tickFrequency']
    if a<t or u<t: continue
    seen.add(k);samples.append({'generation':k[0],'eventTicks':t,'kind':'native' if r.get('appliedTicks')==t else 'inline','modelSequence':r.get('modelSequence'),'nativeSequence':r.get('nativeSequence'),'device':r['device'],'controlToDspMs':1000*(a-t)/f,'controlToUiReceiptMs':1000*(u-t)/f})
def distribution(key, subset=None):
    v=sorted(x[key] for x in (samples if subset is None else subset))
    return {'n':len(v),**({p:v[math.ceil(q*len(v))-1] for p,q in [('p50',.5),('p95',.95)]} if v else {})}
print(json.dumps({'estimator':'nearest-rank; matching first affected event; deduplicated per generation','controlToDspMs':distribution('controlToDspMs'),'controlToUiReceiptMs':distribution('controlToUiReceiptMs'),'byKind':{kind:{key:distribution(key,[x for x in samples if x['kind']==kind]) for key in ['controlToDspMs','controlToUiReceiptMs']} for kind in ['native','inline']},'samples':samples,'limits':'UI receipt is not pixel paint; pre-monitor PCM is not physical output or human listening.'},indent=2))
