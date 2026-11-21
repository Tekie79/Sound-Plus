#!/usr/bin/env python3
"""Audit retained plugin-take artifacts; does not claim listening or native gestures."""
import array, hashlib, json, math, struct, sys, zipfile
from pathlib import Path

def wav(raw):
    assert raw[:4] == b'RIFF' and raw[8:12] == b'WAVE'
    at=12;fmt=pcm=None;mask=None
    while at+8<=len(raw):
        tag,n=struct.unpack_from('<4sI',raw,at)
        if tag==b'fmt ':
            fmt=struct.unpack_from('<HHIIHH',raw,at+8)
            if fmt[0]==65534:mask=struct.unpack_from('<I',raw,at+28)[0]
        if tag==b'data':pcm=raw[at+8:at+8+n]
        at+=8+n+(n&1)
    assert fmt and pcm is not None and fmt[0] in (3,65534) and fmt[5]==32
    data=array.array('f');data.frombytes(pcm)
    if sys.byteorder!='little':data.byteswap()
    assert all(math.isfinite(x) for x in data)
    return fmt,len(data)//fmt[1],math.sqrt(sum(x*x for x in data)/len(data)),mask

root=Path(sys.argv[1])
record=json.loads((root/'artifact-records.json').read_text())
with zipfile.ZipFile(root/'artifacts.zip') as archive:
    for row in record:
        raw=archive.read(row['entry'])
        assert hashlib.sha256(raw).hexdigest()==row['sha256']
        if row['entry'].endswith('.wav'):
            fmt,frames,rms,mask=wav(raw)
            if fmt[1]>2:
                expected={"lcr-v1":7,"quad-v1":0x33,"5.0-v1":0x607,"5.1-side-v1":0x60f,"5.1-rear-v1":0x3f,"7.1-v1":0x63f}
                layout=row["entry"].split("Finalized-",1)[1].split("/",1)[0]
                assert mask==expected[layout],(layout,mask)
            assert fmt[1]==row['channels'] and fmt[2]==row['rate'] and frames==row['frames']
            assert abs(rms-row['rms'])<1e-12
    native=json.loads((root/'native-summary.json').read_text())
    assert native['lvRendersByteIdentical']
    assert archive.read(native['lvBefore'])==archive.read(native['lvAfter'])
    for row in native['takes']:
        doc=json.loads(archive.read(row['document']))['payload'];t=doc['takes'][0]
        assert doc['version']==10 and doc['dsp']=='plugin-takes-v1'
        assert t['complete'] and t['kept'] and len(t['events'])>=1
        assert t['recipe']['recipeVersion']==3 and t['recipe']['processing']=='plugin-ordered-takes-v1'
        assert t['bodyEnd']==t['outputFrames']
        assert len(doc['original']['layers'])==3
        assert any(l['source']['path']==row['printedSource'] for l in doc['original']['layers'])
print(json.dumps({'passed':True,'artifacts':len(record),'nativeTakes':2,
 'scope':'Retained file hashes, finite PCM, dimensions, immutable repeat output, kept recipes/events and referenced print-back; no listening/physical-surround/native-drag claim.'},indent=2))
