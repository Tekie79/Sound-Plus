#!/usr/bin/env python3
"""Independent RIFF/raw PCM and speaker oracle audit for v1.4.1 evidence."""
import argparse, array, gzip, hashlib, json, math, struct
from pathlib import Path

def digest(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def wav(path):
    b=path if isinstance(path,bytes) else Path(path).read_bytes(); assert b[:4]==b'RIFF' and b[8:12]==b'WAVE'
    at=12; fmt=None; data=None
    while at+8<=len(b):
        name,n=struct.unpack_from('<4sI',b,at);chunk=b[at+8:at+8+n]
        if name==b'fmt ':fmt=chunk
        if name==b'data':data=chunk
        at+=8+n+(n&1)
    tag,ch,rate,_,align,bits=struct.unpack_from('<HHIIHH',fmt)
    if tag==65534:tag=struct.unpack_from('<H',fmt,24)[0]
    assert tag==3 and bits==32,(tag,bits)
    samples=array.array('f');samples.frombytes(data);assert len(samples)%ch==0
    return samples,ch,rate

def oracle(degrees):
    # Independent literal 5.1 Side ring, periodic angle and equal-power arc.
    speakers=[(-90,4),(-30,0),(0,2),(30,1),(90,5)]
    angle=math.remainder(degrees,360)
    for i,(left,lc) in enumerate(speakers):
        right,rc=speakers[(i+1)%len(speakers)]
        if i==len(speakers)-1:right+=360
        a=angle if angle>=left else angle+360
        if left<=a<=right:
            phase=(a-left)/(right-left)*math.pi/2;g=[0.]*6
            g[lc]=math.cos(phase);g[rc]=math.sin(phase);return g
    raise AssertionError(angle)

def audit(result, archive=None):
    def read(key):
        if archive:
            return gzip.decompress((archive/('R12-'+key+'.gz')).read_bytes())
        return Path(result['R12'][key]).read_bytes()
    def checksum(key): return hashlib.sha256(read(key)).hexdigest()
    r=result['R12'];t=r['nativeTake'];frames=t['outputFrames'];events=[{'frame':0,'controls':{'spatial':[{'azimuth':0}]}}]+t['events']
    raw=array.array('f');raw.frombytes(read('nativeRaw'))
    replay=array.array('f');replay.frombytes(read('nativeReplayRaw'))
    assert len(raw)>=frames*6 and len(replay)>=frames*6
    parity=max(abs(raw[i]-replay[i]) for i in range(frames*6))
    assert parity<1e-7
    doc=json.loads(read('nativeDocument'))['payload']
    receipt=doc['outputs'][-1]
    render_bytes=gzip.decompress((archive/'R12-native-render.wav.gz').read_bytes()) if archive else Path(receipt['path']).read_bytes()
    rendered,ch,rate=wav(render_bytes)
    assert ch==6 and rate==48000 and len(rendered)>=frames*6
    render_error=max(abs(raw[i]-rendered[i]) for i in range(frames*6))
    assert render_error<1e-7 and receipt['processing']=='stock-layout-takes-v2'
    assert hashlib.sha256(render_bytes).hexdigest()==receipt['sha256']
    windows=[];maximum=0
    for i,e in enumerate(events):
        end=events[i+1]['frame'] if i+1<len(events) else frames
        first=e['frame']+512 # beyond declared 10ms/480-frame smoothing
        if end<=first:continue
        az=e['controls']['spatial'][0]['azimuth'];g=oracle(az)
        err=max(abs(raw[n*6+c]-.2*g[c]) for n in range(first,end) for c in range(6))
        energy=[sum(raw[n*6+c]**2 for c in (0,1,2,4,5)) for n in range(first,end)]
        assert err<2e-6 and max(abs(x-.04) for x in energy)<2e-6
        assert max(abs(raw[n*6+3]) for n in range(first,end))==0
        maximum=max(maximum,err);windows.append({'eventFrame':e['frame'],'first':first,'end':end,'azimuth':az,'maximumError':err,'minimumEnergy':min(energy)})
    assert len(windows)>=10 and windows[-1]['azimuth']==-130
    old,ch,rate=wav(read('oldRender'));assert ch==6 and rate==48000
    assert max(abs(x) for x in old[-512*6:])==0
    assert checksum('oldRender')=='83aa8ed02bb3f76e616be61beed5406986f1f8936d2f5ae655db485870277b12'
    return {'kind':'Independent Python literal-speaker oracle; actual consumed native PCM',
            'frames':frames,'channels':6,'rate':48000,'nativeReplayMaximumError':parity,
            'nativeRenderMaximumError':render_error,'renderPath':receipt['path'],'renderSha256':receipt['sha256'],
            'steadyOracleMaximumError':maximum,'windows':windows,'oldVersionSha256':checksum('oldRender'),
            'oldFinal512FramesSilent':True,'humanListening':False,'physicalSurroundMapping':False,
            'files':{k:{'path':r[k],'sha256':checksum(k)} for k in ('nativeRaw','nativeReplayRaw','nativeDocument','correctedDocument','oldRender')}}

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('results',type=Path);p.add_argument('output',type=Path);p.add_argument('--archive',type=Path,help='Read portable compressed evidence instead of original build paths');a=p.parse_args()
    result=json.loads(a.results.read_text());out=audit(result,a.archive);a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(json.dumps(out,indent=2)+'\n');print(json.dumps({k:v for k,v in out.items() if k not in ('windows','files')},indent=2))
