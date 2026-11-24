#!/usr/bin/env python3
"""Compare a native stereo mixer QA export with the original source PCM."""

import argparse
import hashlib
import json
import struct
import wave
from pathlib import Path


def read_pcm(path, expected_width):
    with wave.open(str(path), "rb") as stream:
        assert (stream.getframerate(), stream.getnchannels(), stream.getsampwidth()) == (
            48000, 2, expected_width
        ), f"Unexpected PCM format in {path.name}"
        frames = stream.getnframes()
        data = stream.readframes(frames)
    assert len(data) == frames * 2 * expected_width, f"Truncated PCM in {path.name}"
    if expected_width == 2:
        samples = (sample for (sample,) in struct.iter_unpack("<h", data))
    else:
        samples = (int.from_bytes(data[i:i + 3], "little", signed=True) for i in range(0, len(data), 3))
    return frames, [sample / (1 << (8 * expected_width - 1)) for sample in samples]


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def gain(db):
    return 10 ** (db / 20)


def audit(source, master, stem_receipt, track_db, send_db, master_db, post_fader):
    original_frames, original = read_pcm(source, 2)
    receipt = json.loads(stem_receipt.read_text())
    assert receipt["tap"] == "pre-master-edges-v1" and receipt["origin"] == 0
    assert (receipt["rate"], receipt["bits"], receipt["layout"]["roles"]) == (48000, 24, ["L", "R"])
    dry, wet = receipt["files"]
    assert dry["owner"] != wet["owner"] and dry["file"] != wet["file"]
    stems = [stem_receipt.parent / row["file"] for row in (dry, wet)]
    stem_hashes = [sha256(path) for path in stems]
    assert stem_hashes == [dry["sha256"], wet["sha256"]]

    track_gain = gain(track_db)
    send_gain = gain(send_db) * (track_gain if post_fader else 1)
    expected_gains = [track_gain, send_gain, (track_gain + send_gain) * gain(master_db)]
    outputs = [*stems, master]
    results = []
    for path, scale in zip(outputs, expected_gains):
        frames, pcm = read_pcm(path, 3)
        assert frames == receipt["frames"] and frames >= original_frames
        error = max(abs(recorded - expected * scale) for recorded, expected in zip(pcm, original))
        tail_peak = max(map(abs, pcm[len(original):]), default=0)
        assert error < 4e-7 and tail_peak == 0, (path.name, error, tail_peak)
        results.append({"file": path.name, "sha256": sha256(path), "maxSampleError": error,
                        "tailPeak": tail_peak})
    return {"sourceSha256": sha256(source), "sourceFrames": original_frames, "rate": 48000,
            "channels": 2, "renderedFrames": receipt["frames"],
            "trackDb": track_db, "sendDb": send_db, "masterDb": master_db,
            "postFader": post_fader, "outputs": results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("master", type=Path)
    parser.add_argument("stem_receipt", type=Path, help="Receipt ordered source track, then FX Return")
    parser.add_argument("--track-db", type=float, default=-6)
    parser.add_argument("--send-db", type=float, default=-12)
    parser.add_argument("--master-db", type=float, default=-6)
    parser.add_argument("--post-fader", action="store_true")
    args = parser.parse_args()
    print(json.dumps(audit(**vars(args)), indent=2))


if __name__ == "__main__":
    main()
