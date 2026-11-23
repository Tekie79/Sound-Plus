#!/usr/bin/env python3
"""Record exact unchanged sources and runnable Release artifacts around the full gate."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
from datetime import datetime, timezone

phase = sys.argv[1]
if phase not in ('before', 'after'):
    raise SystemExit('usage: qualify_hosted_release.py before|after [evidence-directory]')
out = Path(sys.argv[2] if len(sys.argv) > 2 else 'docs/verification/hosted-automation/final/release')
out.mkdir(parents=True, exist_ok=True)
files = [Path('CMakeLists.txt')] + [p for name in ('src', 'tests', 'resources', 'scripts')
                                    for p in Path(name).rglob('*')
                                    if p.is_file() and '__pycache__' not in p.parts]
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
bundle = Path('build/SoundPlus_artefacts/Release/SoundPlus.app/Contents')
binaries = [bundle / 'MacOS/SoundPlus',
            Path('build/SoundPlusTests_artefacts/Release/SoundPlusTests'),
            Path('build/SoundPlusAdvancedSanity_artefacts/Release/SoundPlusAdvancedSanity'),
            Path('build/SoundPlusPublicationDebug_artefacts/Release/SoundPlusPublicationDebug'),
            Path('build/SoundPlusMidiSource_artefacts/Release/SoundPlusMidiSource')]
binaries += [bundle / 'Helpers' / name for name in ('SoundPlusAudio', 'SoundPlusAI', 'SoundPlusPlugin',
                'SoundPlusPluginEditor.app/Contents/MacOS/SoundPlusPluginEditor')]
for target in ('SoundPlusFixture', 'SoundPlusStereoFixture', 'SoundPlusMidiFixture'):
    for p in Path('build/' + target + '_artefacts/Release').rglob('*'):
        if p.is_file() and p.parent.name == 'MacOS':
            binaries.append(p)
for p in Path('build/SoundPlus QA Multi.vst3').rglob('*'):
    if p.is_file() and p.parent.name == 'MacOS':
        binaries.append(p)
identity = {'timestampUTC': datetime.now(timezone.utc).isoformat(),
            'sources': {str(p): sha(p) for p in sorted(files)},
            'binaries': {str(p): sha(p) for p in sorted(set(binaries))}}
if phase == 'before':
    command = lambda *args: subprocess.check_output(args, text=True).strip()
    identity.update(baseHEAD=command('git', 'rev-parse', 'HEAD'),
                    hardware=command('sysctl', '-n', 'machdep.cpu.brand_string'),
                    os=command('sw_vers'), sdk=command('xcrun', '--show-sdk-version'),
                    compiler=command('clang', '--version'),
                    architecture=command('uname', '-m'),
                    environment={k: os.getenv(k) for k in (
                        'SOUNDPLUS_VENDOR_QUALIFICATION', 'SOUNDPLUS_LIVE_DEVICE_QUALIFICATION',
                        'SOUNDPLUS_QUALIFY_AU', 'SOUNDPLUS_HOSTED_VENDOR_QUALIFICATION')})
else:
    before = json.loads((out / 'identity-before.json').read_text())
    identity['unchanged'] = all(identity[k] == before[k] for k in ('sources', 'binaries'))
    assert identity['unchanged'], 'Sources or runnable binaries changed during the final gate'
(out / ('identity-' + phase + '.json')).write_text(json.dumps(identity, indent=2) + '\n')
print(len(identity['sources']), 'source/resource/script hashes;', len(identity['binaries']),
      'binary hashes;', identity.get('unchanged', 'before'))
