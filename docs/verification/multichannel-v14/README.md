# v1.4 evidence index

See [the report](../../MULTICHANNEL_V1_4_REPORT.md) for closure, workload/operation limits, commands and interpretation. `artifact.json` binds the tested native binary/helpers/test binary to the final source commit. `manifest.json` records the byte size and SHA-256 of each evidence file except itself. Source paths in the logs are actual local QA paths; generated fixture media and durable receiving-project references remain in place.

- `final-ctest.log`: final complete 21-suite run; `final-build.log` plus `portable-legacy-build.log` cover the production rebuild and last test-only portability correction.
- `multichannel-results.json.gz`: generated layout/PCM/routing/take results, including native helper stream ownership and portable historical source fingerprints.
- `original-consumed-prefix.f32.gz`, `replay-consumed-prefix.f32.gz`, `native-pcm-prefix.json`: bounded little-endian interleaved float32 **pre-monitor** consumed-prefix evidence; hashes match. Ahead-produced frames outside the declared extent are excluded.
- `legacy-parity.json` and compressed historical documents: unchanged canonical identities and actual original WAV checksum through reopen and deliberate Live conversion. Tests regenerate historical sources in owned folders and verify original fingerprints before explicit same-byte test relinking.
- `pressure-release-{capture,replay}-*`: final-artifact raw observations, native logs, input plans, summaries and actual published documents. `measurements.json` recalculates nearest-rank percentiles from those observations. Corpus files are real independent files with intentional repeated PCM templates; their manifest is compressed separately. Six capture below-threshold peak observations are retained in the counts and qualified in the report.
- `pressure-release-output-audit.json`: independent actual RIFF/PCM/checksum audit of the two published outputs from one frozen take, with different Live edits. This is separate from original capture evidence and human listening.
- `device-{legacy,layout}.json.gz`: actual lifecycle handler/service tests; physical unplug is explicitly false. Audio-health fault injection logs represent intentional test failures/restarts, not unexplained runtime restarts.
- `migration-native-ax.txt`: native recovery from a copied schema-4 catalog. `working-session-*-audit.json` and `user-session-reopened-final-ax.txt`: final actual working-session preservation and recovery. No historical snapshot replaced it.
- PNGs: real native Library/Designer normal/minimum views, labelled 16-channel minimum meters and six-channel detail during final capture pressure. CUA normalizes screenshot dimensions; Retina scale 2 is recorded by the app/environment.
- `delivery-cases.json`, `resolve-api-import.json.gz`, `resolve-api-reopen.json`, receiver PNGs: actual source headers/peaks, API-imported media, 5.1 placement and save/source-app-exit/reopen persistence. Resolve references retained source paths. **These do not qualify native drag.** `native-workflow.soundplus.gz` and its audit retain the native take, explicit interpretation and five finalized output receipts. `native-drag-original.jsonl.gz` preserves the original CRLF log bytes; `native-drag.jsonl` normalizes line endings for review and proves initiation/end only; confirmed Resolve target acceptance remains unverified.
- `development-backpressure-*`: failed earlier pressure evidence retained for the diagnosed IPC correction. Historical unexplained restart/zero-peak observations are not retroactively diagnosed.

A simple archive-integrity check:

```sh
python3 - <<'PY'
from pathlib import Path
import json, hashlib
root = Path('docs/verification/multichannel-v14')
for entry in json.loads((root/'manifest.json').read_text())['files']:
    data = (root/entry['path']).read_bytes()
    assert len(data) == entry['bytes']
    assert hashlib.sha256(data).hexdigest() == entry['sha256']
print('All evidence checksums match')
PY
```
