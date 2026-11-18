#!/usr/bin/env python3
"""Verify the bounded generated Resolve QA fixture, not arbitrary SoundPlus designs."""
import argparse
import hashlib
import json
from pathlib import Path
import wave


def pcm(path):
    with wave.open(str(path)) as reader:
        info = dict(frames=reader.getnframes(), sampleRate=reader.getframerate(),
                    channels=reader.getnchannels(), bits=reader.getsampwidth() * 8)
        raw = reader.readframes(reader.getnframes())
    assert info['bits'] == 24 and info['channels'] == 2 and info['sampleRate'] == 48000
    return info, [int.from_bytes(raw[i:i + 3], 'little', signed=True) / 8388608
                  for i in range(0, len(raw), 3)]


def verify(root):
    doc = json.loads((root / 'selection-render-ready.soundplus').read_text())['payload']
    mix = doc['original']
    assert not doc['activeVariation'] and not mix['masterEffects']
    sources = {}
    for layer in mix['layers']:
        assert layer['rate'] == 1 and not layer['reverse'] and not layer['effects']
        assert layer['pan'] == 0 and not layer['mute'] and not layer['solo']
        assert layer.get('fadeInShape', 'linear') == layer.get('fadeOutShape', 'linear') == 'linear'
        assert layer['channelPair'] == 0
        sources[layer['id']] = pcm(Path(layer['source']['path']))[1]
    reports = []
    for receipt in doc['outputs']:
        if receipt['kind'] not in ('design-render', 'layer-render'):
            continue
        path = Path(receipt['path'])
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        assert digest == receipt['sha256'] and not receipt['settings']['normalize'] and not receipt['settings']['dither']
        info, actual = pcm(path)
        layers = mix['layers']
        if receipt['kind'] == 'layer-render':
            layer = next(layer for layer in layers if layer['id'] == doc['selectedLayer'])
            layers = [dict(layer, placement=0, mute=False, solo=False)]
        assert info['frames'] == (120000 if receipt['kind'] == 'layer-render' else 144000)
        expected = [0.] * (info['frames'] * 2)
        master = 10 ** (mix['masterDb'] / 20)
        for layer in layers:
            source = sources[layer['id']]
            duration = layer['last'] - layer['first']
            gain = 10 ** (layer['gainDb'] / 20) * master
            for frame in range(duration):
                fade = min(1, frame / layer['fadeIn']) if layer['fadeIn'] else 1
                fade *= min(1, (duration - frame - 1) / layer['fadeOut']) if layer['fadeOut'] else 1
                for channel in range(2):
                    expected[(frame + layer['placement']) * 2 + channel] += source[(frame + layer['first']) * 2 + channel] * gain * fade
        error = max(abs(a - b) for a, b in zip(actual, expected))
        assert error < 3e-7, (receipt['kind'], error)
        reports.append(dict(kind=receipt['kind'], path=str(path), sha256=digest,
                            finalizedReceiptMatches=True, **info,
                            seconds=info['frames'] / info['sampleRate'], maxPcmError=error,
                            tolerance=3e-7, humanListening='not performed'))
    assert len(reports) == 2
    return reports


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('qa_root', type=Path)
    args = parser.parse_args()
    print(json.dumps(verify(args.qa_root), indent=2))
