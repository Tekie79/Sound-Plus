#!/usr/bin/env python3
"""Recompute selected native-run PCM; never a human listening claim."""
import array, hashlib, json, math, struct, sys, zipfile
from pathlib import Path
r=Path(sys.argv[1]);record=json.loads((r/'pcm-records.json').read_text());values={}
with zipfile.ZipFile(r/'native-pcm.zip') as z:
 for row in record:
  raw=z.read(row['label']+'.f32');assert hashlib.sha256(raw).hexdigest()==row['sha256']
  a=array.array('f');a.frombytes(raw)
  if sys.byteorder!='little':a.byteswap()
  assert len(a)==row['frames']*row['channels'] and all(math.isfinite(x) for x in a)
  rms=math.sqrt(sum(x*x for x in a)/len(a));assert abs(rms-row['rms'])<1e-12;values[row['label']]=rms
  if row['label'].startswith('youlean-'):
   expected=array.array('h');expected.frombytes(z.read(row['reference']))
   if sys.byteorder!='little':expected.byteswap()
   assert len(expected)==row['frames']*2
   for n in range(row['frames']):
    for c in range(row['channels']):
     target=expected[n*2+c]/32768 if c<2 else 0
     assert a[n*row['channels']+c]==target
 nativeDelta=20*math.log10(values['lv-native-minus6']/values['lv-native-minus12'])
 historyDelta=20*math.log10(values['lv-redo-reordered-minus6']/values['lv-undo-minus12'])
 assert abs(nativeDelta-6)<1e-6 and abs(historyDelta-6)<1e-6
 a=z.read('lv-render-before-recall.wav');b=z.read('lv-render-fresh-recall.wav');assert a==b
 at=12;fmt=pcm=None
 while at+8<=len(a):
  tag,n=struct.unpack_from('<4sI',a,at)
  if tag==b'fmt ':fmt=struct.unpack_from('<HHIIHH',a,at+8)
  if tag==b'data':pcm=a[at+8:at+8+n]
  at+=8+n+(n&1)
 assert fmt[:3]==(3,2,48000) and fmt[5]==32 and len(pcm)//8==192000
before=json.loads((r/'lv-endpoint-before.json').read_text());after=json.loads((r/'lv-endpoint-after-reorder.json').read_text());assert before['generation']==after['generation']
print(json.dumps({'passed':True,'nativeTrimDeltaDb':nativeDelta,'undoRedoDeltaDb':historyDelta,'reorderRetainedGeneration':before['generation'],'freshRendersByteIdentical':True,'renderFrames':192000,'renderChannels':2,'renderRate':48000,'meterWindowsExactDecodedSource':sum(k.startswith('youlean-') for k in values),'scope':'Recorded selected pre-monitor PCM; native gestures and meter screenshots described separately. No physical/listening/receiver claim.'},indent=2))
