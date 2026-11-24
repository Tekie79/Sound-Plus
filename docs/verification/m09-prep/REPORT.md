# M09 candidate preparation

**Development gate passed. Distribution remains No-Go.** The frozen macOS arm64 application is SoundPlus 0.1.0, build 1. The isolated candidate checkout is at `3be7178ee0525791af3726dbb03b0fdaefec1130`. Later reporting commits retain this candidate's identity.

## Frozen build and checks

| Observation | Result |
|---|---|
| Full unfiltered Release CTest | 64/64 passed in 529.52 sec, including ASan |
| Candidate application SHA-256 | `ca92909592d92c7e7ac09a0e0b48553e3167c63af84e07bdd25797711876b97f` |
| Freeze manifest SHA-256 | `c3c5a2f55bc2bf4174125845ff57185aa87d74a18c135951516701734634ecc0` |
| Source tree | `4f701df30da740c426127255cacdd6594c89986c` |
| Source inputs and bundle | 449 tracked source/resource/test inputs and 26 bundled files. File hashes and modes matched before and after CTest |
| Toolchain | Apple clang version 21.0.0 (clang-2100.1.1.101), SDK 26.5, macOS 26.3.2, arm64 |
| Distribution credentials | Development build. Developer ID distribution, notarization, quarantined installation, upgrade and rollback await authorization and testing |
| Integrity checks | Eight focused harness checks plus nine existing packaging checks passed. The harness suite ran again inside the full gate |
| Acoustic increment | Five focused Release suites. All 43 score lists, bounds and abstentions reproduced on the frozen test executable |
| M09 receipt | Run `fbd8a3e5-e147-4fed-a583-f983d5b112db`. 399 Not Run, including 181 P0, 214 P1 and four P2. The historical 65 passes and 186 unfinished observations are preserved in their M00 record |

The freeze manifest includes the actual linked dependencies, full bundle inventory, nested signatures, dependency-tree digest, commands and transcript hashes. Decompress the build transcript to verify its original SHA-256. Keep the recorded manifest digest and initial run identity outside the editable working receipt. The owner must review the evidence behind each status. [Candidate manifest](candidate.json) · [Source inputs](source-inputs.json) · [Build transcript](build.log.gz) · [Release transcript](release.log) · [Opening receipt](run.json) · [Retained identity](run.identity.json)

## User-state audit

The existing app stayed open on its original bundle. Its active document remained at revision 4506, with 43 Library assets on schema 7. Of 488 protected files, 485 had identical hashes. 3 files changed inside the running live-insert helper, listed individually in the audit. All five catalog table projections matched. Verification used copied QA catalogs, separate app bundles and temporary fixtures. [Preservation record](preservation.json)

## First acceptance session

Use the frozen checkout and the receipt identity above. Start with **MX-FAD-001** in a new QA catalog and design. Create a mono −15 dBFS tone, set Track and Master to unity, render, and compare decoded samples under the registry's expected routing. For **M00-01**, change the mixer fader to −6 dB, inspect the Edit value, check rendered PCM and exercise Undo. Retain fixture and WAV hashes, channel layout, sample rate, exact frames, numerical tolerance, observed values, observer and timestamp. Enter results only after the case observations exist. Continue through P0/P1 data and signal cases, including M08-01 and the M08-12 acoustic review supplement.

Subsequent operator sessions cover authorized paid providers and billing, listening and physical MIDI/microphone/5.1, real Resolve placement and Save/Quit/Reopen, followed by distribution and owner approval. This phase performed no billable provider calls and made no changes to receiving projects. [Acceptance procedures](../../acceptance/M09_PROGRAM.md) · [Acoustic report](../m08-1/REPORT.md)

## Reopen the frozen build on this Mac

From the SoundPlus repository root, this opens the isolated compact QA profile with its indexed evaluation sounds.

```sh
rc="/Users/harvest/SoundPlus-QA/m09-3be7178"
open -n "$rc/build/m09-release/SoundPlus_artefacts/Release/SoundPlus.app" --args \
  --data-dir "$PWD/build/m08-1-qa/candidate-compact" --model-dir "$PWD/build/semantic/model-pack"
python3 scripts/m09_acceptance.py --checkout "$rc" \
  --commit 3be7178ee0525791af3726dbb03b0fdaefec1130 \
  --manifest-sha256 c3c5a2f55bc2bf4174125845ff57185aa87d74a18c135951516701734634ecc0 \
  --check-run "$PWD/docs/verification/m09-prep/run.json"
```

The native validation re-read the receipt and refused a second initialization, the previous development executable, and the pre-candidate source revision. [CLI results](validation.json)
