"""Compare file-backed Mach-O sections from both clean Candidate B builds."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

def sections(path):
    binary = path.read_bytes()
    assert struct.unpack_from("<I", binary)[0] == 0xfeedfacf
    count = struct.unpack_from("<I", binary, 16)[0]
    offset, digests = 32, {}
    for _ in range(count):
        command, size = struct.unpack_from("<II", binary, offset)
        if command == 0x19:
            for index in range(struct.unpack_from("<I", binary, offset + 64)[0]):
                name, segment, _, length, position = struct.unpack_from(
                    "<16s16sQQI", binary, offset + 72 + 80 * index)
                if position and length:
                    key = tuple(p.rstrip(b"\0").decode() for p in (segment, name))
                    digests[",".join(key)] = hashlib.sha256(binary[position:position + length]).hexdigest()
        offset += size
    return digests

first, second = map(Path, sys.argv[1:])
a, b = sections(first), sections(second)
assert a.keys() == b.keys()
clock = [set(subprocess.check_output(["strings", "-a", p]).splitlines()) for p in (first, second)]
print(json.dumps({
    "attempt1Sha256": hashlib.sha256(first.read_bytes()).hexdigest(),
    "attempt2Sha256": hashlib.sha256(second.read_bytes()).hexdigest(),
    "matchingSections": [k for k in a if a[k] == b[k]],
    "differingSections": [k for k in a if a[k] != b[k]],
    "printableStringsUniqueToEachBuild": [
        [s.decode("ascii", "replace") for s in sorted(left - right) if len(s) > 4]
        for left, right in (clock, clock[::-1])
    ],
}, indent=2))
