#!/usr/bin/env python3
"""Re-audit retained ordered-insert WAV pairs. This is recorded PCM, not listening.
Usage: python3 scripts/audit_ordered_inserts.py <summary.json>
"""
import array
import hashlib
import json
from pathlib import Path
import struct
import sys


def samples(path):
    data = Path(path).read_bytes()
    assert data[:4] == b'RIFF' and data[8:12] == b'WAVE'
    at, fmt, pcm = 12, None, None
    while at + 8 <= len(data):
        tag, length = struct.unpack_from('<4sI', data, at)
        chunk = data[at+8:at+8+length]
        if tag == b'fmt ':
            fmt = struct.unpack_from('<HHIIHH', chunk)
            if fmt[0] == 65534:
                fmt = (struct.unpack_from('<H', chunk, 24)[0],) + fmt[1:]
        if tag == b'data':
            pcm = chunk
        at += 8 + length + (length & 1)
    assert fmt and fmt[0] in (1, 3) and fmt[5] == 32 and pcm
    values = array.array('f' if fmt[0] == 3 else 'i')
    values.frombytes(pcm)
    if sys.byteorder != 'little':
        values.byteswap()
    values = list(values) if fmt[0] == 3 else [x / 2147483648 for x in values]
    return fmt, values, hashlib.sha256(data).hexdigest()


summary = Path(sys.argv[1]).resolve()
record = json.loads(summary.read_text())
def location(value):
    path = Path(value)
    return path if path.is_absolute() else summary.parent / path
rows = []
for case in record['cases']:
    if 'actual' not in case:
        continue
    af, actual, ah = samples(location(case['actual']))
    ef, expected, eh = samples(location(case['expected']))
    assert af == ef and len(actual) == len(expected)
    assert af[1] == len(case['roles']['roles']) and af[2] == 48000
    error = max(abs(a-b) for a, b in zip(actual, expected))
    assert error < 1e-6
    rows.append(dict(layout=case['layout'], trackStockFirst=case['trackStockFirst'],
                     masterStockFirst=case['masterStockFirst'], maximumError=error,
                     actualSHA256=ah, expectedSHA256=eh))
assert len(rows) == 32
print(json.dumps(dict(passed=True, cases=rows, assertions=record['checks'],
                     scope='Recorded actual versus independently computed expected PCM; no listening/native gesture claim.'), indent=2))
