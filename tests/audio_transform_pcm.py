"""Independent RIFF/PCM oracle for source snapshots and non-destructive edits."""
import math
import pathlib
import struct
import subprocess
import sys
import tempfile
import wave

base = pathlib.Path(sys.argv[2])
base.mkdir(parents=True, exist_ok=True)
root = pathlib.Path(tempfile.mkdtemp(dir=base))
samples = [i % 8191 - 4095 for i in range(6 * 44100)]

def wav(name, channels, frames):
    with wave.open(str(root / name), "wb") as out:
        out.setparams((channels, 2, 44100, 0, "NONE", ""))
        out.writeframes(struct.pack("<%dh" % len(frames), *frames))

def floats(name):
    data = (root / name).read_bytes()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE"
    i = 12
    while i + 8 <= len(data):
        tag, count = struct.unpack_from("<4sI", data, i)
        if tag == b"data":
            return [v[0] for v in struct.iter_unpack("<f", data[i+8:i+8+count])]
        i += 8 + count + count % 2
    raise ValueError("missing PCM")

wav("original.wav", 1, samples)
wav("returned.wav", 1, [16384] * len(samples))
# WAVEFORMATEXTENSIBLE explicitly locates side surrounds (mask 0x60f).
payload = struct.pack("<6h", 0, 0, 0, 0, 0, 8192) * 44100
fmt = struct.pack("<HHIIHHHHI", 0xfffe, 6, 44100, 44100*12, 12, 16, 22, 16, 0x60f)
fmt += bytes.fromhex("0100000000001000800000aa00389b71")
chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(payload)) + payload
(root/"surround.wav").write_bytes(b"RIFF" + struct.pack("<I",len(chunks)+4) + b"WAVE" + chunks)
subprocess.run([sys.argv[1], "--audio-transform", str(root)], check=True)
pcm = floats("composite.wav")
orig = [v / 32768 for v in samples]
assert floats("excerpt.wav") == orig[1001:2001]
assert pcm[:300] == orig[:300] and pcm[600:] == orig[600:]
assert len(pcm) == len(orig)
for i in range(300, 600):
    blend = min(1, min(i - 300, 599 - i) / 8)
    assert math.isclose(pcm[i], orig[i] + blend * (.5 - orig[i]), abs_tol=1e-7)
fold = floats("fold.wav")
assert max(map(abs,fold[::2])) == 0 and all(math.isclose(v, .125 / math.sqrt(2), abs_tol=1e-7) for v in fold[1::2])
print("Independent PCM passed for all 264600 output frames, 1000 excerpt frames and right-only downmix")
