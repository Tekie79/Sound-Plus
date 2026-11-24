"""Independently read the installed JUCE and ONNX trees against the freeze digest."""
import hashlib
import json
from pathlib import Path
import sys

checkout = Path(sys.argv[1]).resolve()
dependency_root = checkout / 'vendor'
manifest = json.loads((checkout / 'build/m09-release/candidate.json').read_text())
combined = hashlib.sha256()
for name in ('JUCE-8.0.6', 'onnxruntime-1.22.1'):
    count = 0
    for p in sorted((dependency_root/name).rglob('*')):
        if '.git' in p.parts or p.name == '.DS_Store' or not p.is_file():
            continue
        combined.update(p.relative_to(dependency_root).as_posix().encode() + b'\0')
        combined.update(hashlib.sha256(p.read_bytes()).digest())
        count += 1
    assert count > 0, name
    print(name, count)
print(combined.hexdigest())
assert combined.hexdigest() == manifest['configuredDependencyTreesSha256']
