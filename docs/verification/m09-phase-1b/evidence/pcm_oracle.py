"""Independent byte-level oracle for all outputs; needs only the Python standard library."""
import hashlib,json,math,struct,sys
from pathlib import Path
root=Path(sys.argv[1])
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
def wav(p):
    raw=p.read_bytes();assert raw[:4]==b'RIFF' and raw[8:12]==b'WAVE'
    chunks={};i=12
    while i+8<=len(raw):
        k,n=struct.unpack_from('<4sI',raw,i);chunks[k]=raw[i+8:i+8+n];i+=8+n+n%2
    fmt=chunks[b'fmt '];_,channels,rate,_,width,bits=struct.unpack_from('<HHIIHH',fmt)
    mask=struct.unpack_from('<I',fmt,20)[0] if fmt[:2]==b'\xfe\xff' else None
    data=chunks[b'data'];step=bits//8
    assert len(data)%width==0 and bits in (16,24)
    samples=[int.from_bytes(data[k:k+step],'little',signed=True)/(2**(bits-1)) for k in range(0,len(data),step)]
    return channels,rate,mask,samples
def floats(p):
    raw=p.read_bytes();return struct.unpack('<'+'f'*(len(raw)//4),raw)
def err(a,b):
    assert len(a)==len(b) and all(map(math.isfinite,a))
    return max(abs(x-y) for x,y in zip(a,b))
result={'delay':[],'automation':[],'library':[]}
for rate in [44100,48000,96000]:
    _,_,_,src=wav(root/f'pulses-{rate}.wav')
    for mode in range(3):
        out=src+[0.]*(2*12288-len(src))
        for i,value in enumerate(src):
            if not value:continue
            if mode==1:out[i]+=value
            if mode==0:
                index=i+2*(rate//100);amp=value
                while index<len(out):
                    out[index]+=amp;index+=2*(rate//100);amp*=.5
        rows=[]
        for block in [1,32,61,257,512]:
            p=root/f'delay-{rate}-{mode}-{block}.f32';actual=floats(p)
            diff=err(actual,out)
            rows.append(dict(block=block,frames=len(actual)//2,error=diff,tailError=err(actual[16384:],out[16384:]),sha256=sha(p)))
        result['delay'].append(dict(rate=rate,mode=['delay + lookahead','delay bypass','return mute'][mode],partitions=rows))
for i,mask in enumerate([0,1,3,2,0]):
    n=f'automation-{i}-{mask}';pcm=floats(root/(n+'.f32'));_,rate,_,export=wav(root/(n+'.wav'))
    _,_,_,src=wav(root/'pulses-48000.wav')
    # A first point at frame zero seeds a second point at the 8192-frame design end,
    # returning to the base parameter. Interpolate in dB before summing the pre tap.
    expected=[]
    for i,v in enumerate(src):
        phase=(i//2)/8192
        dry=(-6*(1-phase)) if mask&1 else 0
        wet=(-3-9*phase) if mask&2 else -12
        expected.append(v*(10**(dry/20)+10**(wet/20)))
    result['automation'].append(dict(mask=mask,frames=len(export)//2,rate=rate,sessionSha256=sha(root/(n+'.soundplus')),wavSha256=sha(root/(n+'.wav')),playError=err(pcm,expected),exportError=err(export,expected),difference=err(pcm,export)))
source=root/'roles.wav';channels,rate,mask,src=wav(source);assert (channels,rate,mask)==(6,48000,0x60f)
for gen,(start,end) in enumerate([(100,47000),(37200,37513)],1):
    command=json.loads((root/f'audition-{start}.json').read_bytes());delivery=json.loads((root/f'transfer-{start}.json').read_bytes())
    assert command['playGeneration']==gen and command['id']==delivery['source'] and command['revision']==delivery['revision']
    assert (command['startSample'],command['endSample'],command['auditionPair'])==(start,end,2)
    assert (delivery['first'],delivery['last'],delivery['samples'])==(start,end,end-start)
    audition=floats(root/f'audition-{start}.f32');excerpt=root/f'excerpt-{start}.wav';c,r,m,full=wav(excerpt)
    assert (c,r,m)==(channels,rate,mask) and sha(excerpt)==delivery['sha256']
    expected=src[start*6:end*6];picked=[v for i,v in enumerate(expected) if i%6>=4]
    result['library'].append(dict(start=start,end=end,generation=gen,sourceId=command['id'],pair='Ls/Rs',sourceSha256=sha(source),deliverySha256=sha(excerpt),deliveredMask=hex(m),auditionError=err(audition,picked),deliveryError=err(full,expected)))
print(json.dumps(result,indent=2))
assert max(v['error'] for r in result['delay'] for v in r['partitions'])<2.5e-7
assert max(max(r['playError'],r['exportError'],r['difference']) for r in result['automation'])<2.5e-7
assert all(r['auditionError']==r['deliveryError']==0 for r in result['library'])
