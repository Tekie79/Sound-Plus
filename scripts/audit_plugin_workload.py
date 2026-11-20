#!/usr/bin/env python3
"""Recalculate finite-plugin workload summaries from retained polling/timing records."""
import argparse
import json
import math
from pathlib import Path


def distribution(values):
    values = sorted(values)
    assert all(math.isfinite(x) and x >= 0 for x in values)
    result = {"count": len(values)}
    if values:
        for name, fraction in [("p50", .5), ("p95", .95), ("p99", .99)]:
            result[name] = values[math.ceil(fraction * len(values)) - 1]
    return result


def same(actual, expected):
    for key, value in actual.items():
        assert key in expected and math.isclose(value, expected[key], rel_tol=1e-7, abs_tol=1e-6), (key, value, expected)


def audit(load):
    rows = [r for r in load["samples"] if r["phase"] == "process"]
    assert [r["iteration"] for r in rows] == list(range(len(rows)))
    ticks = distribution([r["tickMs"] for r in rows])
    same(ticks, load["tickMs"])
    requests = load["searchRequests"]
    assert len({r["requestId"] for r in requests}) == len(requests)
    published = []
    for r in requests:
        assert r["status"] in ["published", "superseded", "notObservedAtEnd"]
        if r["status"] == "published":
            elapsed = r["observedPublishedMs"] - r["requestedMs"]
            assert math.isclose(elapsed, r["publicationMs"], abs_tol=1e-6)
            published.append(r["publicationMs"])
    search = distribution(published)
    same(search, load["searchPublicationMs"])
    previous = overlap = active = 0
    for r in rows:
        assert r["headersRead"] - previous == r["headersDelta"]
        assert r["headersDelta"] >= 0
        previous = r["headersRead"]
        overlap += r["indexWorkers"] > 0 or r["backlog"] > 0 or r["headersDelta"] > 0
        active += r["pluginBusy"]
    assert overlap == load["indexOverlapSamples"]
    assert active == load["processingActiveSamples"]
    waves = load["waveforms"]
    records = waves["requestToUiAdoptionRecords"]
    for r in records:
        assert math.isclose(r["adoptedMs"] - r["submittedMs"], r["durationMs"], abs_tol=1e-6)
    wave = distribution([r["durationMs"] for r in records])
    summary = dict(waves["requestToUiAdoptionMs"])
    summary["count"] = summary.pop("n")
    same(wave, summary)
    assert len(records) <= waves["timingRecordLimit"] == 512
    assert waves["retainedSources"] <= 48 and waves["retainedGeometries"] <= 96
    assert waves["displaySources"] <= 160 and waves["maxDisplayPairs"] <= 2048
    start, end = load["start"], load["end"]
    assert end["sourceSample"] > start["sourceSample"]
    for state in [r["audio"] for r in load["samples"]] + [end]:
        assert state["playGeneration"] == start["playGeneration"] and state["playing"]
        assert state["underruns"] == start["underruns"] == 0
    assert math.isfinite(load["workerMs"]) and load["workerMs"] > 0
    return {"passed": True, "estimator": "nearest-rank: sorted[ceil(p*N)-1]",
            "scanPollingSamples": sum(r["phase"] == "scan" for r in load["samples"]),
            "processPollingSamples": len(rows), "tickMs": ticks,
            "searchRequestCount": len(requests), "searchPublicationMs": search,
            "searchStatuses": {s: sum(r["status"] == s for r in requests)
                               for s in ["published", "superseded", "notObservedAtEnd"]},
            "indexOverlapSamples": overlap, "processingActiveSamples": active,
            "waveformAdoptionMs": wave,
            "waveformRequestsTotal": waves["requests"], "waveformDecodesTotal": waves["decodes"],
            "waveformRequestsDuringLoad": waves["requests"] - load["waveformsBeforeLoad"]["requests"],
            "waveformDecodesDuringLoad": waves["decodes"] - load["waveformsBeforeLoad"]["decodes"],
            "clockFramesAdvanced": end["sourceSample"] - start["sourceSample"],
            "underruns": 0, "workerMs": load["workerMs"], "workerPeakBytes": load["workerPeakBytes"],
            "qualification": "One bounded concurrent run. Polling/timing records are not independent PCM experiments. "
                             "Search latency ends at observed UI publication; waveform records span this pool's fixture lifetime."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("load", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = json.dumps(audit(json.loads(args.load.read_text())), indent=2) + "\n"
    if args.output:
        args.output.write_text(result)
    print(result, end="")


if __name__ == "__main__":
    main()
