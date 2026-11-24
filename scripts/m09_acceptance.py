#!/usr/bin/env python3
"""Validate the historical registry, open a candidate run or check its observations."""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import uuid

import m09_candidate as candidate
from m09_candidate import exclusive, require
from release_macos import digest, ReleaseError

ROOT = Path(__file__).resolve().parents[1]
PRIOR = Path("docs/verification/m00/case-results.json")
STATUSES = {"Not Run", "Pass", "Fail", "Partial", "Blocked", "Not Applicable"}
OBSERVATION = {"status": "Not Run", "actual": "", "owner": "", "runUtc": "", "evidence": []}


def read(path):
    def unique(pairs):
        result = dict(pairs)
        require(len(result) == len(pairs), "Duplicate JSON key")
        return result
    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique)


def catalog(root):
    registry_path = root / candidate.REGISTRY
    registry = read(registry_path)
    rows = registry["cases"]
    cases = {r["id"]: r for r in rows}
    require(registry["version"] == 1 and registry["defaultStatus"] == "Not Run", "Registry identity")
    require(len(cases) == len(rows) == 399, "Missing or duplicate case")
    require(all(all(row.get(k) for k in ("steps", "expected", "owner", "method", "priority")) for row in rows),
            "Incomplete case")
    counts = Counter("Milestones" if re.fullmatch("M0[0-9]", r["scope"]) else r["scope"] for r in rows)
    require(counts == {"Milestones": 153, "Mixer": 213, "Transfer": 26, "Integrated": 7}, "Family counts")
    ids = "\n".join(sorted(cases)).encode()
    require(hashlib.sha256(ids).hexdigest() == "b235d00e99d493632183f3add8fcc4d095bd5df35a565f9e343c6551ba7f4984", "Version 1 case IDs changed")
    old = read(root / PRIOR)
    prior = {r["id"]: r["status"] for group in ("mixer", "transfer", "milestoneCases") for r in old[group]}
    linked = {}
    for r in rows:
        if "priorM00" in r:
            link = r["priorM00"]
            require(link["id"] == r["id"] and (registry_path.parent / link["record"]).resolve() == root / PRIOR,
                    f"{r['id']}: broken historical link")
            linked[r["id"]] = link["status"]
    require(linked == prior and len(prior) == 251 and list(prior.values()).count("Pass") == 65,
            "Historical receipt mapping changed")
    return cases


def utc(stamp):
    require(isinstance(stamp, str) and re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:\.\d{1,6})?(?:Z|\+00:00)", stamp),
            "Use an ISO 8601 UTC timestamp")
    return datetime.fromisoformat(stamp.replace("Z", "+00:00"))


def evidence(directory, relative, anchor):
    require(isinstance(relative, str), "Evidence path must be text")
    rel = Path(relative)
    require(rel.parts and rel.parts[0] == "evidence" and len(rel.parts) > 1 and ".." not in rel.parts
            and str(rel) == relative, "Evidence must be a file under this run's evidence directory")
    path = directory.resolve() / rel
    require(path == path.resolve() and path.is_file() and path.stat().st_size > 0, f"Missing or redirected evidence {rel}")
    return {"path": relative, "bytes": path.stat().st_size, "sha256": digest(path),
            "runId": anchor["runId"], "candidateSha256": anchor["candidate"]["manifestSha256"]}


def new_run(path, cases, identity):
    require(path.name == "run.json" and path.resolve() == path.absolute(), "Use RUN_DIRECTORY/run.json")
    require(not path.exists(), "Run already exists")
    anchor = {"version": 2, "runId": str(uuid.uuid4()), "createdUtc": candidate.now(), "candidate": identity}
    lock = path.with_name("run.identity.json")
    exclusive(lock, anchor)
    try:
        exclusive(path, {**anchor, "platform": "", "catalog": "", "results": {key: OBSERVATION for key in cases}})
    except Exception:
        lock.unlink()
        raise


def check_run(path, cases, identity):
    require(path.name == "run.json" and path.resolve() == path.absolute(), "Use the original local run")
    anchor_path = path.with_name("run.identity.json")
    require(anchor_path.resolve() == anchor_path.absolute(), "Redirected run identity")
    run, anchor = read(path), read(anchor_path)
    require(set(anchor) == {"version", "runId", "createdUtc", "candidate"} and anchor["version"] == 2
            and uuid.UUID(anchor["runId"]).version == 4 and anchor["candidate"] == identity, "Run identity mismatch")
    require(all(run[k] == v for k, v in anchor.items()) and set(run) == set(anchor) | {"results", "platform", "catalog"},
            "Run differs from retained identity")
    began = utc(run["createdUtc"])
    end = datetime.now(timezone.utc)
    require(began <= end and set(run["results"]) == set(cases), "Invalid run date or missing cases")
    require(all(isinstance(run[k], str) for k in ("platform", "catalog")), "Invalid host or catalog")
    counts = Counter()
    for key, row in run["results"].items():
        require(isinstance(row, dict) and set(row) == set(OBSERVATION), f"{key}: invalid observation")
        status = row["status"]
        require(status in STATUSES, f"{key}: unknown status")
        counts[status] += 1
        if status == "Not Run":
            require(row == OBSERVATION, f"{key}: unattempted case contains an observation")
            continue
        require(run["platform"].strip() and run["catalog"].strip(), f"{key}: identify host and QA catalog")
        require(all(isinstance(row[k], str) and row[k].strip() for k in ("actual", "owner"))
                and began <= utc(row["runUtc"]) <= end, f"{key}: missing observation or invalid UTC")
        attachments = row["evidence"]
        require(isinstance(attachments, list) and (attachments or status == "Blocked"),
                f"{key}: attach observations or scope decision")
        seen = set()
        for ref in attachments:
            require(isinstance(ref, dict) and ref == evidence(path.parent, ref["path"], anchor),
                    f"{key}: attachment identity changed")
            require(ref["path"] not in seen, f"{key}: duplicate attachment")
            seen.add(ref["path"])
    return counts


def main():
    p = argparse.ArgumentParser(description=__doc__)
    group = p.add_mutually_exclusive_group()
    group.add_argument("--new-run", type=Path)
    group.add_argument("--check-run", type=Path)
    p.add_argument("--checkout", type=Path, default=ROOT)
    p.add_argument("--commit")
    p.add_argument("--binary", type=Path)
    p.add_argument("--manifest-sha256", help="Trusted digest printed by the candidate freeze")
    p.add_argument("--evidence", help="With --check-run, print the attachment record for evidence/FILE")
    args = p.parse_args()
    try:
        root = args.checkout.resolve()
        cases = catalog(root)
        print("399 cases (153 milestone, 213 mixer, 26 transfer, 7 journeys); 251 historical links, 186 previously unfinished")
        run = args.new_run or args.check_run
        require(not args.evidence or args.check_run, "--evidence requires --check-run")
        if run:
            identity = candidate.verify(root, args.commit, args.binary or root / candidate.EXE)
            require(args.manifest_sha256 == identity["manifestSha256"], "Supply the trusted freeze manifest SHA-256")
            if args.new_run:
                new_run(run, cases, identity)
            print(dict(check_run(run, cases, identity)))
            if args.evidence:
                print(json.dumps(evidence(run.parent, args.evidence, read(run.with_name("run.identity.json"))), indent=2))
    except (ValueError, KeyError, TypeError, OSError, ReleaseError) as exc:
        p.exit(1, f"{exc}\n")


if __name__ == "__main__":
    main()
