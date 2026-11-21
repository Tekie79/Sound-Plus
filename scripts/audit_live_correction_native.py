#!/usr/bin/env python3
"""Recompute selected final-correction native PCM observations; no listening claim.
Usage: python3 scripts/audit_live_correction_native.py docs/verification/live-corrections/native
"""
import array
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import zipfile

root = Path(sys.argv[1])
record = json.loads((root / 'lv-audit.json').read_text())
values = {}
with zipfile.ZipFile(root / 'native-pcm.zip') as archive:
    for window in record['windows']:
        raw = archive.read('lv-' + window['label'] + '.f32')
        samples = array.array('f')
        samples.frombytes(raw)
        if sys.byteorder != 'little':
            samples.byteswap()
        assert len(samples) == window['frames'] * window['channels']
        assert all(math.isfinite(x) for x in samples)
        rms = math.sqrt(sum(x*x for x in samples) / len(samples))
        assert abs(rms - window['rms']) < 1e-12
        values[window['label']] = 20 * math.log10(rms)
    assert abs(values['native-minus6'] - values['native-minus12'] - 6) < 1e-6
    assert abs(values['redo-minus6'] - values['undo-minus12'] - 6) < 1e-6
    assert archive.read('lv-after-close.f32') == archive.read('lv-after-reopen.f32')
    renders = [archive.read('render-' + str(i) + '.wav') for i in range(2)]
    assert renders[0] == renders[1]
    for data, saved in zip(renders, record['renders']):
        assert hashlib.sha256(data).hexdigest() == saved['sha256']
        position = 12
        fmt = pcm = None
        while position + 8 <= len(data):
            tag, size = struct.unpack_from('<4sI', data, position)
            if tag == b'fmt ':
                fmt = struct.unpack_from('<HHIIHH', data, position+8)
            if tag == b'data':
                pcm = data[position+8:position+8+size]
            position += 8 + size + (size & 1)
        assert fmt[0:3] == (3, 2, 48000) and fmt[5] == 32
        assert len(pcm) // 8 == 192000
    meter = json.loads((root / 'meter-pcm.json').read_text())
    for row in meter['records']:
        raw = archive.read(row['path'])
        assert hashlib.sha256(raw).hexdigest() == row['sha256']
        actual, expected = array.array('f'), array.array('h')
        actual.frombytes(raw)
        expected.frombytes(archive.read(row['reference']))
        if sys.byteorder != 'little':
            actual.byteswap()
            expected.byteswap()
        channels = row['channels']
        assert len(actual) == row['frames'] * channels
        assert max(abs(actual[i*channels+c] - expected[i*2+c]/32768)
                   for i in range(row['frames']) for c in range(2)) == 0
        assert all(actual[i*channels+c] == 0
                   for i in range(row['frames']) for c in range(2, channels))
before = json.loads((root / 'old-before-state.json').read_text())['slots'][0]['settings']
after = json.loads((root / 'old-after-drag-state.json').read_text())['slots'][0]['settings']
assert before['stateHash'] == after['stateHash']
assert before['parameters'] == after['parameters']
print(json.dumps({'passed': True, 'nativeTrimDeltaDb': values['native-minus6'] - values['native-minus12'],
                  'undoRedoDeltaDb': values['redo-minus6'] - values['undo-minus12'],
                  'closeReopenSelectedWindowsIdentical': True, 'freshApplicationRendersIdentical': True,
                  'renderFrames': 192000, 'renderChannels': 2, 'renderRate': 48000,
                  'meterWindowsExactDecodedSource': len(meter['records']),
                  'oldSkoolNativeInput': 'Unconfirmed: captured state remained unchanged',
                  'limits': 'Selected pre-monitor windows; no human listening, physical output, stateful LV tail or receiver qualification.'}, indent=2))
