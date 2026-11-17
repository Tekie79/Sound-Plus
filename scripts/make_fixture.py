#!/usr/bin/env python3
"""Generate neutral, deterministic audio. No downloaded or licensed recordings."""
import argparse
import array
import hashlib
import json
import math
from pathlib import Path
import sys
import wave

parser = argparse.ArgumentParser()
parser.add_argument("output", type=Path, nargs="?", default=Path("build/demo-library"))
parser.add_argument("--count", type=int, default=12)
args = parser.parse_args()
if args.count < 2:
    parser.error("--count must be at least 2")
if args.output.exists() and any(args.output.iterdir()):
    parser.error("Choose an empty output directory; existing files are never overwritten")
manifest = []
for index in range(args.count):
    channels = 1 + index % 2
    rate = (44100, 48000, 96000)[index % 3]
    seconds = 30.0 if index == 0 else 0.5 + (index % 4) * 0.25
    name = "soft mechanical click.wav" if index < 2 else f"neutral tone {index:04d}.wav"
    file = args.output / ("A" if index % 2 == 0 else "B") / name
    file.parent.mkdir(parents=True, exist_ok=True)
    samples = array.array("h")
    frames = round(rate * seconds)
    for frame in range(frames):
        envelope = min(1.0, frame / (rate * 0.01), (frames - 1 - frame) / (rate * 0.01))
        for channel in range(channels):
            samples.append(round(3500 * envelope * math.sin(2 * math.pi * (220 + index * 17 + channel * 70) * frame / rate)))
    if sys.byteorder != "little":
        samples.byteswap()
    with wave.open(str(file), "wb") as audio:
        audio.setparams((channels, 2, rate, 0, "NONE", "not compressed"))
        audio.writeframes(samples.tobytes())
    manifest.append({"path": str(file.relative_to(args.output)), "seconds": seconds, "channels": channels,
                     "sample_rate": rate, "sha256": hashlib.sha256(file.read_bytes()).hexdigest()})
(args.output / "fixture_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(f"Generated {len(manifest)} recordings in {args.output.resolve()}")
