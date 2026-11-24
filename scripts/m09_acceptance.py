#!/usr/bin/env python3
"""Validate the consolidated case registry and create or inspect a candidate run."""

import argparse
import hashlib
import json
import re
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REGISTRY = ROOT / "docs/acceptance/m09-cases.json"
PRIOR = ROOT / "docs/verification/m00/case-results.json"
STATUSES = {"Not Run", "Pass", "Fail", "Partial", "Blocked", "Not Applicable"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def catalog():
    registry = read(REGISTRY)
    rows = registry["cases"]
    cases = {r["id"]: r for r in rows}
    require(registry["version"] == 1 and registry["defaultStatus"] == "Not Run", "Registry version or initial state")
    require(len(cases) == len(rows) == 399, "Missing or duplicate case")
    require(all(all(row.get(k) for k in ("steps", "expected", "owner", "method", "priority")) for row in rows), "Incomplete case")
    old = read(PRIOR)
    prior = {r["id"]: r["status"] for group in ("mixer", "transfer", "milestoneCases") for r in old[group]}
    linked = {r["id"]: r["priorM00"]["status"] for r in rows if "priorM00" in r}
    require(linked == prior, "Historical receipt mapping changed")
    print(f"{len(cases)} cases, {len(prior)} historical links, {sum(v != 'Pass' for v in prior.values())} previously unfinished")
    return cases


def new_run(path, cases, commit, binary):
    require(re.fullmatch(r"[0-9a-f]{40}", commit or ""), "Provide a full candidate Git SHA")
    require(binary and binary.is_file(), "Provide the installed candidate executable")
    with binary.open("rb") as source:
        digest = hashlib.file_digest(source, "sha256").hexdigest() if hasattr(hashlib, "file_digest") else stream_hash(source)
    run = {
        "version": 1, "registrySha256": hashlib.sha256(REGISTRY.read_bytes()).hexdigest(),
        "buildCommit": commit, "binarySha256": digest, "platform": "", "catalog": "",
        "results": {key: {"status": "Not Run", "actual": "", "owner": "", "runUtc": "", "evidence": []} for key in cases},
    }
    with path.open("x", encoding="utf-8") as out:
        json.dump(run, out, ensure_ascii=False, indent=2)
        out.write("\n")


def stream_hash(source):
    digest = hashlib.sha256()
    for block in iter(lambda: source.read(1024 * 1024), b""):
        digest.update(block)
    return digest.hexdigest()


def check_run(path, cases):
    run = read(path)
    require(run["version"] == 1 and run["registrySha256"] == hashlib.sha256(REGISTRY.read_bytes()).hexdigest(), "Registry identity mismatch")
    require(re.fullmatch(r"[0-9a-f]{40}", run["buildCommit"]) and re.fullmatch(r"[0-9a-f]{64}", run["binarySha256"]), "Candidate identity mismatch")
    require(set(run["results"]) == cases.keys(), "Run must include every registered case")
    counts = Counter()
    for key, row in run["results"].items():
        status = row.get("status")
        require(status in STATUSES, f"{key}: unknown status")
        counts[status] += 1
        if status == "Not Run":
            continue
        require(run.get("platform") and run.get("catalog"), f"{key}: identify the host and QA catalog")
        require(all(row.get(k) for k in ("owner", "actual", "runUtc")), f"{key}: incomplete observation or blocker")
        if status in {"Pass", "Fail", "Partial", "Not Applicable"}:
            require(isinstance(row.get("evidence"), list) and row["evidence"], f"{key}: missing evidence or scope decision")
            for ref in row["evidence"]:
                require(isinstance(ref, str) and ref.strip() and (path.parent / ref).is_file(), f"{key}: missing local evidence {ref!r}")
    print(", ".join(f"{status} {counts[status]}" for status in sorted(counts)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--new-run", type=Path)
    group.add_argument("--check-run", type=Path)
    parser.add_argument("--commit")
    parser.add_argument("--binary", type=Path)
    args = parser.parse_args()
    try:
        cases = catalog()
        if args.new_run:
            new_run(args.new_run, cases, args.commit, args.binary)
        if args.check_run:
            check_run(args.check_run, cases)
    except (ValueError, KeyError, OSError) as exc:
        parser.exit(1, f"{exc}\n")


if __name__ == "__main__":
    main()
