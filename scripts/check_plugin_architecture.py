#!/usr/bin/env python3
"""Exercise actual arm64, Intel-only and universal owned VST3 discovery.

Requires separately compiled native/Intel copies of tests/PluginFixture.cpp.
Only the copied universal QA bundle is modified; neither input is installed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def binary(bundle):
    return bundle / "Contents/MacOS/SoundPlus QA Effect"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("worker", type=Path)
    parser.add_argument("native", type=Path)
    parser.add_argument("intel", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    records = []
    with tempfile.TemporaryDirectory(prefix="soundplus-architecture-") as directory:
        root = Path(directory)
        universal = root / "Universal.vst3"
        shutil.copytree(args.native, universal)
        subprocess.run(["lipo", "-create", str(binary(args.native)),
                        str(binary(args.intel)), "-output", str(binary(universal))], check=True)
        subprocess.run(["codesign", "--force", "--deep", "--sign", "-", str(universal)],
                       check=True, capture_output=True)
        for name, bundle, expected in [("native", args.native, "arm64"),
                                       ("intel", args.intel, "x86_64"),
                                       ("universal", universal, "arm64+x86_64")]:
            executable = binary(bundle)
            slices = subprocess.check_output(["lipo", "-archs", str(executable)], text=True).strip()
            reply = root / (name + "-reply.json")
            request = {"protocol": 1, "mode": "scan", "path": str(bundle.resolve()),
                       "reply": str(reply)}
            job = root / (name + "-request.json")
            job.write_text(json.dumps(request))
            started = time.monotonic()
            child = subprocess.run([str(args.worker.resolve()), str(job)], capture_output=True,
                                   text=True, timeout=15, check=True)
            response = json.loads(reply.read_text())
            plugin = response["plugins"][0]
            assert plugin["architecture"] == expected, (name, plugin)
            if name == "intel":
                assert not plugin["layouts"] and "Native arm64 required" in plugin["reason"]
            else:
                assert not plugin["reason"] and "5.1-side-v1" in plugin["layouts"]
            records.append({"case": name, "lipoArchitectures": slices,
                            "binarySha256": digest(executable), "response": response,
                            "elapsedMs": (time.monotonic() - started) * 1000,
                            "stderr": child.stderr, "passed": True})
    result = {"passed": True, "worker": str(args.worker.resolve()),
              "workerSha256": digest(args.worker),
              "fixtureSourceSha256": digest(Path("tests/PluginFixture.cpp")),
              "cases": records,
              "qualification": "Actual owned VST3 bundles; Intel rejected without bridging, "
                               "native and universal slices accepted. No vendor qualification."}
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"passed": True, "cases": [r["case"] for r in records]}))


if __name__ == "__main__":
    main()
