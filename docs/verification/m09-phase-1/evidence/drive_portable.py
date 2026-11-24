"""Link an external QA driver against existing frozen objects, then run with generated PCM."""
from pathlib import Path
import hashlib
import shlex
import struct
import subprocess
import sys
import wave

build, ninja, root = [Path(arg).resolve() for arg in sys.argv[1:]]
src = Path(__file__).with_name('portable.cpp').resolve()
root.mkdir(exist_ok=False)
sources = root / 'sources'
sources.mkdir()
def make_wave(name, channels, data):
    with wave.open(str(sources/name), 'wb') as w:
        w.setparams((channels,2,48000,0,'NONE',''))
        w.writeframes(data)
stereo = bytearray(8192*4)
struct.pack_into('<h',stereo,900*4,8192)
struct.pack_into('<h',stereo,1500*4+2,-4096)
make_wave('stereo.wav',2,stereo)
response = bytearray(512*2)
for i, value in [(0,16384),(129,8192)]: struct.pack_into('<h',response,i*2,value)
make_wave('response.wav',1,response)
# PCM16 WAVEFORMATEXTENSIBLE role-coded impulses, independent RIFF fixture.
for name, mask in [('5.1-rear-v1',0x3f),('5.1-side-v1',0x60f)]:
    fmt = struct.pack('<HHIIHHHHI',65534,6,48000,576000,12,16,22,16,mask) + bytes.fromhex('0100000000001000800000aa00389b71')
    pcm = bytearray(4096*12)
    for c in range(6): struct.pack_into('<h',pcm,12*(256+c*128)+2*c,4096*(c+1))
    body = b'WAVEfmt '+struct.pack('<I',len(fmt))+fmt+b'data'+struct.pack('<I',len(pcm))+pcm
    (sources/(name+'.wav')).write_bytes(b'RIFF'+struct.pack('<I',len(body))+body)
target = 'SoundPlusTests_artefacts/Release/SoundPlusTests'
commands = subprocess.check_output([ninja,'-C',build,'-t','commands',target],text=True).splitlines()
compile_args = shlex.split(next(line for line in commands if ' -c ' in line and line.endswith('/tests/MixerGraphTests.cpp')))
compile_args = compile_args[:compile_args.index('-MD')] + ['-o',str(root/'qa.o'),'-c',str(src)]
link = shlex.split(commands[-1].split(' && ')[1])
link = [a for a in link if not (a.endswith('.o') and '/tests/' in a)]
link.insert(link.index('-o'),str(root/'qa.o'))
link[link.index('-o')+1]=str(root/'qa')
for args in [compile_args,link]:subprocess.run(args,cwd=build,check=True)
print('Frozen core archive SHA-256',hashlib.sha256((build/'libsoundplus_core.a').read_bytes()).hexdigest(),flush=True)
subprocess.run([root/'qa',root],cwd=root,check=True)
