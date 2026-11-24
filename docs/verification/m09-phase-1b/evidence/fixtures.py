"""Deterministic original impulse and channel-role sources, little-endian PCM16."""
import struct,sys,wave
from pathlib import Path
p=Path(sys.argv[1]);p.mkdir()
for rate in [44100,48000,96000]:
    pcm=bytearray(8192*4)
    for c,n,v in [(0,720,4096),(1,727,-3072),(0,8150,-4096),(1,8185,2048)]:
        struct.pack_into('<h',pcm,4*n+2*c,v)
    with wave.open(str(p/f'pulses-{rate}.wav'),'wb') as w:
        w.setparams((2,2,rate,0,'NONE',''));w.writeframes(pcm)
fmt=struct.pack('<HHIIHHHHI',65534,6,48000,576000,12,16,22,16,0x60f)+bytes.fromhex('0100000000001000800000aa00389b71')
data=b''.join(struct.pack('<6h',*[((n*(c+3)+137*c)%24001)-12000 for c in range(6)]) for n in range(48000))
body=b'WAVEfmt '+struct.pack('<I',len(fmt))+fmt+b'data'+struct.pack('<I',len(data))+data
(p/'roles.wav').write_bytes(b'RIFF'+struct.pack('<I',len(body))+body)
