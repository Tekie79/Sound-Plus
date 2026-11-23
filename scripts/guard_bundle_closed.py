#!/usr/bin/env python3
"""Prevent overwriting a running app/helper bundle; never kills a process."""
from pathlib import Path
import subprocess
import sys
bundle = Path(sys.argv[1] if len(sys.argv) > 1 else "build/SoundPlus_artefacts/Release/SoundPlus.app").resolve()
running = []
build_root = bundle.parents[2]
test_runner = str(build_root / "SoundPlusTests_artefacts")
for line in subprocess.check_output(["ps", "-ax", "-o", "pid=,command="], text=True).splitlines():
    fields = line.strip().split(None, 1)
    if len(fields) == 2 and (fields[1].startswith(str(bundle) + "/Contents/") or fields[1].startswith(test_runner) or (fields[1].split()[0].endswith("/ctest") and "--test-dir build" in fields[1])):
        running.append(line.strip())
if running:
    sys.exit("Close SoundPlus and its bundle helpers before rebuilding:\n" + "\n".join(running))
