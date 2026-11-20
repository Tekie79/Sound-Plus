#!/usr/bin/env python3
"""Audit retained native LV50A PCM windows and finalized WAV (stdlib only)."""
import array, hashlib, json, math, struct, sys, zipfile
from pathlib import Path
root=Path(sys.argv[1]); record=json.loads((root/'lv-pcm-windows.json').read_text()); render=json.loads((root/'lv-render-audit.json').read_text())
def floats(data):
    a=array.array('f');a.frombytes(data)
    if sys.byteorder!='little':a.byteswap()
    assert all(math.isfinite(x) for x in a)
    return a
with zipfile.ZipFile(root/'lv-raw-windows.zip') as z:
    values={}; arrays={}
    for w in record['windows']:
        a=floats(z.read(w['path']));assert len(a)==w['frames']*w['channels'];arrays[w['label']]=a
        rms=math.sqrt(sum(x*x for x in a)/len(a));assert abs(rms-w['rms'])<1e-12;values[w['label']]=20*math.log10(rms)
    assert abs(values['native-minus12']-values['native-minus6']+6)<1e-6
    assert abs(values['undo-minus12']-values['redo-minus6']+6)<1e-6
    assert arrays['before-close']==arrays['after-reopen']
    b=z.read('finalized-render.wav');assert hashlib.sha256(b).hexdigest()==render['sha256']
    pos=12;pcm=None;fmt=None
    while pos+8<=len(b):
        tag,n=struct.unpack_from('<4sI',b,pos)
        if tag==b'fmt ':fmt=struct.unpack_from('<HHIIHH',b,pos+8)
        if tag==b'data':pcm=b[pos+8:pos+8+n]
        pos+=8+n+(n&1)
    assert fmt[0]==3 and fmt[1]==2 and fmt[2]==48000 and fmt[5]==32
    a=floats(pcm);assert len(a)//2==192000
    expected=arrays['initial-minus6'];actual=a[48000*2:96000*2]
    error=max(abs(x-y) for x,y in zip(actual,expected));assert error==render['maximumError']==0
print(json.dumps({'passed':True,'windowRmsDbfs':values,'nativeDeltaDb':values['native-minus6']-values['native-minus12'],'undoRedoDeltaDb':values['redo-minus6']-values['undo-minus12'],'closeReopenIdentical':True,'freshRenderMaximumError':error,'renderFrames':192000,'limits':'Selected deterministic pre-monitor PCM windows; no human/physical-output qualification.'},indent=2))

if (root/'meter-pcm-audit.json').exists():
    m=json.loads((root/'meter-pcm-audit.json').read_text())
    with zipfile.ZipFile(root/'meter-raw-windows.zip') as z:
        for w in m['records']:
            a=floats(z.read(w['entry']));ch=w['channels'];assert len(a)==w['frames']*ch
            peaks=[max(abs(x) for x in a[c::ch]) for c in range(ch)]
            assert peaks==w['peak']
            assert abs(peaks[0]-w['expectedPeak'])<.0001 and abs(peaks[1]-w['expectedPeak'])<.0001
            if ch==6:assert peaks[2:]==[0]*4
    print(json.dumps({'meterPcmPassed':True,'windows':len(m['records']),'namedRearOtherRolesSilent':True}))
