"""Decode RIFF samples independently of JUCE and compare to fixture equations."""
import hashlib
import json
import math
from pathlib import Path
import struct
import sys

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def decode(path):
    data = path.read_bytes()
    assert data[:4] == b'RIFF' and data[8:12] == b'WAVE'
    offset = 12
    chunks = {}
    while offset+8 <= len(data):
        length, = struct.unpack_from('<I', data, offset+4)
        start = offset+8
        assert start+length <= len(data)
        chunks[data[offset:offset+4]] = data[start:start+length]
        offset = start+length+(length%2)
    fmt = chunks[b'fmt ']
    tag, channels, rate, _, align, bits = struct.unpack_from('<HHIIHH',fmt)
    mask = None
    if tag == 65534:
        mask, tag = struct.unpack_from('<IH',fmt,20)
    assert tag == 1 and rate == 48000 and align == channels*(bits//8)
    body = chunks[b'data']
    width = bits//8
    samples = [int.from_bytes(body[p:p+width],'little',signed=True)/(2**(bits-1)) for p in range(0,len(body),width)]
    return samples, channels, mask

def compare(path, channels, frames, peaks, mask=None):
    pcm, ch, actual_mask = decode(path)
    assert (ch,len(pcm)) == (channels,frames*channels)
    if mask is not None: assert actual_mask == mask
    error = max(abs(value-peaks.get(divmod(index,ch),0)) for index,value in enumerate(pcm))
    assert error < 2e-7, (path,error)
    return {'file':path.name,'sha256':sha(path),'frames':frames,'channels':ch,'headerMask':actual_mask,'maxAbsError':error}

def single_wav(folder):
    path, = folder.glob('*.wav')
    return path

def main(run):
    root = run/'work/ir-package'
    observations={}
    pulses={(900,0):.25,(1500,1):-.125}
    def scaled(gain):return {key:v*gain for key,v in pulses.items()}
    for folder, gain in [('unity',1),('attenuated',10**(-6/20))]:
        observations[folder]=compare(single_wav(root/folder),2,8192,scaled(gain))
    for folder, mask in [('5.1-side-v1',0x60f),('5.1-rear-v1',0x3f)]:
        observations[folder]=compare(single_wav(root/folder),6,4096,{(256+c*128,c):(c+1)/8 for c in range(6)},mask)
    stems, = (run/'work/mixer-stems').glob('*/Outputs/*/stems.json')
    receipt=json.loads(stems.read_text())
    assert receipt['origin']==0 and receipt['frames']==14192 and len(set(e['owner'] for e in receipt['files']))==2
    for item, gain in zip(receipt['files'],[1,10**(-6/20)],strict=True):
        p=stems.parent/item['file']
        assert sha(p)==item['sha256']
        observations[item['file']]=compare(p,2,14192,scaled(gain))
    observations['linear stem sum']=compare(single_wav(stems.parents[2]/'Linear'),2,14192,scaled(1+10**(-6/20)))
    before,after=[single_wav(root/name) for name in ['before','after']]
    assert before.read_bytes()==after.read_bytes()
    pcm,_,_=decode(after)
    observations['convolution']={'sha256':sha(after),'peak':max(map(abs,pcm)),'beforeAfterMaxError':0,
        'nontrivialSamples':[(i//2,i%2,v) for i,v in enumerate(pcm) if abs(v)>1e-5][:16]}
    expected_echo = {(900,0):.125,(1029,0):.0625,(1500,1):-.0625,(1629,1):-.03125}
    observations['restored echo']=compare(after,2,8192,expected_echo)
    pack=root/'collected'
    entries=json.loads((pack/'Manifest.json').read_text())['manifest']['files']
    assert sorted(e['file'].split('/')[0] for e in entries)==['IR','Media']
    assert all(sha(pack/e['file'])==e['sha256'] for e in entries)
    observations['portableFiles']=entries
    return observations

if __name__=='__main__':
    print(json.dumps(main(Path(sys.argv[1])),indent=2))
