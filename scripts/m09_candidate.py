"""Build and qualify one clean, source-stamped macOS development candidate."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess

import release_macos as release

BUILD = Path("build/m09-release")
APP = BUILD / "SoundPlus_artefacts/Release/SoundPlus.app"
EXE = APP / "Contents/MacOS/SoundPlus"
REGISTRY = Path("docs/acceptance/m09-cases.json")


def require(ok, message):
    if not ok:
        raise ValueError(message)


def now():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def git(root, *args):
    return subprocess.check_output(["git", "-C", root, *args], text=True).strip()


def source_identity(root, commit):
    require(re.fullmatch("[0-9a-f]{40}", commit or "") and git(root, "rev-parse", "HEAD") == commit,
            "Commit differs from checked-out source")
    require(Path(git(root, "rev-parse", "--show-toplevel")).resolve() == root, "Use the checkout root")
    require(not git(root, "status", "--porcelain", "--untracked-files=normal"), "Candidate checkout is dirty")
    return {"commit": commit, "tree": git(root, "rev-parse", "HEAD^{tree}"),
            "registrySha256": release.digest(root / REGISTRY)}


def exclusive(path, value):
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")


def inventory(root, commit):
    app = root / APP
    require(app.resolve() == app, "Candidate path is redirected")
    view = release.inspect_bundle(app, release.product_version(), "1", pristine=True)
    marker = ("SOUNDPLUS-CANDIDATE:" + commit).encode() + b"\0"
    require(marker in (root / EXE).read_bytes(), "Executable was not built from the candidate commit")
    require(view["bundleFiles"]["Contents/Resources/resolve-transfer.py"] ==
            release.digest(root / "resources/resolve/transfer.py"), "Wrong Resolve bridge")
    view["fileModes"] = {name: (app / name).stat().st_mode & 0o777 for name in view["bundleFiles"]}
    return view


def dependencies(root):
    # JUCE includes our tracked native-scale patch. Record the actual configured dependency trees.
    tree = hashlib.sha256()
    for folder in ("JUCE-8.0.6", "onnxruntime-1.22.1"):
        base = root / "vendor" / folder
        require(base.is_dir(), f"Missing dependency {folder}")
        for path in sorted(base.rglob("*")):
            if path.is_file() and ".git" not in path.parts and path.name != ".DS_Store":
                tree.update(str(path.relative_to(root / "vendor")).encode() + b"\0")
                tree.update(bytes.fromhex(release.digest(path)))
    return tree.hexdigest()


def verify(root, commit, binary):
    root = root.resolve()
    identity = source_identity(root, commit)
    require(binary and binary.absolute() == root / EXE, "Use the frozen build's exact executable path")
    manifest = root / BUILD / "candidate.json"
    require(manifest.resolve() == manifest, "Redirected manifest")
    data = json.loads(manifest.read_text())
    require(data["version"] == 1 and data["source"] == identity and data["binaryPath"] == str(EXE),
            "Candidate manifest source or path mismatch")
    require(data["inventory"] == inventory(root, commit), "Candidate bundle changed")
    for filename in ("release.log", "build.log"):
        require(release.digest(root / BUILD / filename) == data["logs"][filename], f"{filename} changed")
    return {"commit": commit, "tree": identity["tree"], "binaryPath": str(EXE),
            "binarySha256": data["inventory"]["bundleFiles"]["Contents/MacOS/SoundPlus"],
            "manifestSha256": release.digest(manifest), "registrySha256": identity["registrySha256"]}


def freeze(root, commit, toolroot, jobs):
    require(root == Path(__file__).resolve().parents[1], "Run the freeze script from the isolated checkout")
    identity = source_identity(root, commit)
    build = root / BUILD
    build.mkdir(parents=True, exist_ok=False)
    cmake = toolroot / "cmake/data/bin/cmake"
    ctest = cmake.with_name("ctest")
    commands = [[cmake, "-S", root, "-B", build, "-G", "Ninja",
                 "-DCMAKE_MAKE_PROGRAM=" + str(toolroot / "bin/ninja"), "-DCMAKE_BUILD_TYPE=Release",
                 "-DBUILD_TESTING=ON", "-DSOUNDPLUS_BUILD_PLUGIN_ASAN_QA=ON", "-DCMAKE_OSX_ARCHITECTURES=arm64",
                 "-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0", "-DSOUNDPLUS_CANDIDATE_SHA=" + commit],
                [cmake, "--build", build, "--parallel", str(jobs)],
                [ctest, "--test-dir", build, "--output-on-failure"]]
    with (build / "build.log").open("xb") as output:
        for cmd in commands[:2]:
            subprocess.run(cmd, stdout=output, stderr=subprocess.STDOUT, cwd=root, check=True)
    before = inventory(root, commit)
    dependency_hash = dependencies(root)
    with (build / "release.log").open("xb") as output:
        subprocess.run(commands[2], stdout=output, stderr=subprocess.STDOUT, cwd=root, check=True)
    after = inventory(root, commit)
    require(after == before and dependencies(root) == dependency_hash and source_identity(root, commit) == identity,
            "Inputs or bundle changed during qualification")
    log = (build / "release.log").read_text()
    total = re.search(r"100% tests passed, 0 tests failed out of (\d+)", log)
    require(total and int(total[1]) > 0 and not re.search(r"\*\*\*(?:Skipped|Not Run)|\(Disabled\)", log),
            "Incomplete Release gate")
    signatures = {}
    for name, binary, _, _ in release.SIGNING_ORDER:
        result = release.command("/usr/bin/codesign", "-dvv", root / APP / name, check=False)
        signatures[binary] = [s for s in result.stderr.splitlines()
                              if s.startswith(("Signature=", "Authority=", "TeamIdentifier=", "CodeDirectory "))]
    data = {"version": 1, "productVersion": release.product_version(), "buildNumber": "1",
            "source": identity, "binaryPath": str(EXE), "build": release.provenance(),
            "configuredDependencyTreesSha256": dependency_hash, "inventory": before, "unchangedAfterCTest": True,
            "commands": [[str(a).replace(str(toolroot), "<tools>").replace(str(root), "<checkout>") for a in cmd]
                         for cmd in commands], "signatures": signatures, "qualificationUtc": now(),
            "gate": {"passed": int(total[1]), "failed": 0}, "distribution": "Development only",
            "logs": {f: release.digest(build / f) for f in ("build.log", "release.log")}}
    exclusive(build / "candidate.json", data)
    print(json.dumps(verify(root, commit, root / EXE), indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--tools", required=True, type=Path)
    parser.add_argument("--jobs", type=int, default=6)
    args = parser.parse_args()
    freeze(Path(__file__).resolve().parents[1], args.commit, args.tools.resolve(), args.jobs)
