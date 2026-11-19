#!/usr/bin/env python3
"""Independent RIFF/PCM audit of two completed takes' published WAVs; read-only."""
import argparse, hashlib, json, math, struct
from pathlib import Path


def document(path):
    text=path.read_text();envelope=json.loads(text);start=text.index('"payload"')+len('"payload"');start=text.index(':',start)+1
    while text[start].isspace():start+=1
    payload,end=json.JSONDecoder().raw_decode(text,start)
    canonical=text[start:end]
    assert hashlib.sha256(canonical.encode()).hexdigest()==envelope['sha256'], 'Published canonical payload checksum'
    return payload


def wave(path):
    raw=path.read_bytes(); assert raw[:4]==b'RIFF' and raw[8:12]==b'WAVE'
    assert struct.unpack_from('<I',raw,4)[0]+8<=len(raw)
    result={'path':str(path),'sha256':hashlib.sha256(raw).hexdigest(),'bytes':len(raw)}; data=None
    at=12
    while at+8<=len(raw):
        tag=raw[at:at+4]; n=struct.unpack_from('<I',raw,at+4)[0]; start=at+8
        assert start+n<=len(raw)
        if tag==b'fmt ':
            fmt,ch,rate,_,align,bits=struct.unpack_from('<HHIIHH',raw,start)
            mask=struct.unpack_from('<I',raw,start+20)[0] if fmt==65534 else 0
            subtype=struct.unpack_from('<H',raw,start+24)[0] if fmt==65534 else fmt
            assert 1<=ch<=16 and subtype in (1,3) and bits in (16,24,32) and align==ch*(bits//8)
            result.update(channels=ch,rate=rate,bits=bits,mask=mask,subtype=subtype,align=align)
        if tag==b'data': data=raw[start:start+n]
        at=start+n+(n&1)
    assert data is not None and len(data)%result['align']==0
    result['frames']=len(data)//result['align'];return result,data


def audit(doc):
    take=next(t for t in doc['takes'] if t['id']==doc['activeTake']); assert take['complete']
    output=next(o for o in reversed(doc['outputs']) if o.get('takeId')==take['id'])
    header,data=wave(Path(output['path']));assert header['sha256']==output['sha256']
    layout=output['channelProvenance']['export']; assert len(layout['roles'])==header['channels']
    bits={'L':0,'R':1,'C':2,'LFE':3,'Lb':4,'Rb':5,'Ls':9,'Rs':10}
    expected=sum(1<<bits[r] for r in layout['roles']) if all(r in bits for r in layout['roles']) else 0
    if header['channels']>2: assert header['mask']==expected
    header.update(layout=layout,takeId=take['id'],recipeHash=take['recipeHash'],eventHash=take['eventHash'],events=len(take['events']),bodyEnd=take['bodyEnd'],outputFrames=take['outputFrames'])
    return header,data


def main():
    p=argparse.ArgumentParser();p.add_argument('--document',type=Path,required=True);p.add_argument('--compare',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    first,x=audit(document(a.document));second,y=audit(document(a.compare))
    for key in ['layout','rate','bits','subtype','recipeHash','eventHash','bodyEnd','outputFrames']: assert first[key]==second[key],key
    frames=min(first['frames'],second['frames']);count=frames*first['align'];same=x[:count]==y[:count]
    errors=[0.0]*first['channels']
    if not same:
        width=first['bits']//8;ch=first['channels']
        def sample(b,at):
            if first['subtype']==3:return struct.unpack_from('<f',b,at)[0]
            return int.from_bytes(b[at:at+width],'little',signed=True)/(2**(first['bits']-1))
        for at in range(0,count,width):
            c=(at//width)%ch;v=sample(x,at)-sample(y,at);assert math.isfinite(v);errors[c]=max(errors[c],abs(v))
    result={'first':first,'second':second,'comparedFrames':frames,'comparedSeconds':frames/first['rate'],'perChannelMaximumDifference':dict(zip(first['layout']['roles'],errors)),'exactPcmPrefix':same,'interpretation':'Two published interleaved outputs from the same frozen take, with deliberately different Live edits and metadata/tail presets. This is independent WAV byte/PCM verification, not original capture evidence or human listening.','humanListening':False,'receiverQualification':False}
    a.output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2));assert max(errors)==0

if __name__=='__main__':main()
