# M09 acceptance · Phase 1

**Continue controlled acceptance. Hold external distribution.** On November 24, 2026, the Apple Silicon candidate completed a first sample spanning all six requested areas. Of 399 registered cases, **24 were attempted, 17 passed, 7 were partial, 0 failed, 0 were blocked and 375 remain Not Run**. No P0/P1 application defect was reproduced. The seven partial cases still have outstanding acceptance criteria. Codex operated the local checks. A release owner has yet to sign off. [Case-by-case observations](run.json)

## Candidate and custody

| Item | Identity |
|---|---|
| Review/report base | `cef66e4690a2809cae18de3b016d5dd6a3ef7ab1` |
| Tested source | `3be7178ee0525791af3726dbb03b0fdaefec1130`, clean detached worktree |
| Git tree | `4f701df30da740c426127255cacdd6594c89986c` |
| App executable SHA-256 | `ca92909592d92c7e7ac09a0e0b48553e3167c63af84e07bdd25797711876b97f` |
| Freeze manifest SHA-256 | `c3c5a2f55bc2bf4174125845ff57185aa87d74a18c135951516701734634ecc0` |
| Fresh run | `272a0d51-e6d0-413d-9968-2d5850e631a6` |
| QA host | Apple M5 Pro, arm64, macOS 26.3.2 (25D2150), SDK 26.5, Apple clang 21.0.0 |
| Device shown in Library | MacBook Pro Speakers |
| Full regression gate at freeze | 64/64 in 529.52 s. This session rechecked that transcript's digest and ran targeted checks afresh. |

The candidate verifier checked the embedded marker, bundle file inventory and modes, registry digest and both qualification transcripts. A separate read of 4,222 configured JUCE files and 12 ONNX Runtime files reproduced the combined tree SHA-256 `03ba048d409822584c0db0668b22d39ba53be51911c651fed98ae78fe38079ef`. The frozen test executable and separate QA driver used the original build artifacts. Only the external measurement driver was compiled. [Environment](evidence/environment.json) · [Frozen manifest](../m09-prep/candidate.json)

The local run and original fixtures are under `/Users/harvest/SoundPlus-QA/phase1.QgKS16`. A read-only copy of its anchor is separately held at `/Users/harvest/SoundPlus-QA/anchor-272a0d51-e6d0-413d-9968-2d5850e631a6.json`. The original 399-Not-Run receipt and its 251 historical links retain their original identities. This archive contains the new run, its anchor, original observations, and generated WAVs. [Run identity](run.identity.json)

## What was exercised

| Session | Observation | Registered results |
|---|---|---|
| A · Compatibility | Three version-7 designs with Track, Master and variation inserts survived migration, save and reopen. Six historical project/take PCM references were checked within 1e-7 FS. The take-integrity test reported zero native PCM error. | M01-15 Pass. M09-01 Partial. |
| B · Mixer | Unity error 0. At −6 dB the largest error was 2.27242e-8 FS. Both 5.1 layouts retained all six roles through Group and Master. RIFF masks were 0x60f (Side) and 0x3f (Rear). Largest role sample error was 1.19210e-7 FS. Duplicate-name Group/Return stems had unique owners, equal origin, 14,192 frames and matching SHA-256 receipts. Pre/post, solo, return mute and PDC were checked at three rates and five block sizes. | MX-FAD-002, MX-SND-006, MX-SPA-001, MX-SPA-002, M00-06 Pass. M00-03 Partial. |
| C · Library | Native Find Similar returned clip-041 (0.891) and clip-013 (0.889) for clip-003 at a 0.55 threshold. Native Reject survived process restart, Undo restored the proposal, and Apply saved the RAIN overlay across another restart. Independent source-manifest checks matched for all 43 files. In the view harness the exact refined bookmark/transfer interval was [37200,37513). | M03-06, M03-07 Pass. M03-13, M08-12 Partial. |
| D · Edit and Perform | Created Audio and MIDI tracks in the QA app. The Audio disclosure expanded to Volume. Switched from Pad A to B and observed the four assignments. Graph/session fixtures exercised multiple hidden curves, Latch/Write/Punch, +3 dB trim, seeded grain partitions, spectral freeze, typed source triggers and frozen replay. Largest source-overlap error across 44.1/48/96 kHz was 8.79e-8 FS. | M04-02, M05-03, M05-07, M06-04 Pass. M00-05 Partial. |
| E · Portable projects | Explicitly approved generated media and a two-tap IR, collected the project, moved the source directory offline, reopened and rendered its frozen take. All 8,192 stereo frames matched the analytical convolution exactly, and both complete WAV files had the same digest. A subsequent changed IR was refused before final publication. The separate same-filename fixture preserved both distinct assets. | M07-02, M07-04, M07-07 Pass. |
| F · Providers | Exercised no-key and declined consent, an isolated Keychain round trip, three approved loopback SFX requests and exact published PCM (132,300 scalar samples). Killed the client after submission. The synchronous SFX job reopened for review. The asynchronous transform resumed its existing ID with one POST. All provider traffic used loopback. | M01-03, M02-06 Pass. M01-02, M01-07 Partial. |

The independent decoder compared every frame and channel, including expected silence and the stems' 6,000-frame tails. The external driver linked the frozen core archive and existing JUCE objects. Its generated source pulses and IR are included for independent inspection. [Decoded measurements](evidence/pcm.json) · [Generated WAVs, stems and portable files](evidence/generated-wavs.zip) · [QA driver](evidence/portable.cpp)

Acoustic review yielded 27 suggestions and 16 abstentions. The fixture labeled sea waves (clip-021) received NATURAL DISASTER at 0.4443. Glass breaking (clip-005) received no suggestion. The near-silent synthetic rear stimulus had cosine 0.9791 against digital silence. These are observations of the classifier and encoder. No human listened or rated the material in this run. [Scores and fixture identities](evidence/quality.json) · [Encoder measurements](evidence/encoder.json) · [Native UI observations](evidence/ui.txt) · [Perform capture](evidence/perform.png)

## Open criteria and triage

| Partial case | Remaining evidence |
|---|---|
| M09-01 | Independent clean bootstrap/build reproducibility. The existing frozen app was verified and launched. |
| M00-03 | Repeat the parallel-path PCM measurement with the specified delay on the FX Return. The exercised return used a lookahead limiter. |
| M00-05 | Toggle each hidden curve independently in the same document and compare live playback and exported PCM. This session compared both enabled against both cleared and checked the Volume-first UI. |
| M03-13 | Capture coarse and refined audition PCM directly and compare sample boundaries and channel interpretation. |
| M08-12 | Exercise the native rejection flow on the known-wrong source and collect the expert listening judgment. The observed native rejection used the rain recording. Missing-model and offline checks ran in the core harness. |
| M01-02 | Inspect exported projects, undo and an OS crash artifact for the dedicated test credential. The isolated Keychain and job-artifact checks passed. |
| M01-07 | Verify job-ID reconciliation with a provider that exposes a durable generation status. The synchronous SFX mock recovered to review with one submission. |

Two test-launch issues were retained. The multichannel harness hit the macOS stack guard at function entry with an 8,176 KiB soft limit. Its first retry requested a limit above the shell hard maximum and did not start. With the child process limit set to the existing hard maximum (65,520 KiB), the same test binary completed twice, reporting 882,411 and 885,231 checks respectively with zero native PCM error. The other launch used an incorrect output directory basename. The harness refused it before its first check and passed when given a fresh QA-owned `similar-test-output`. Neither incident required a candidate change. [Crash triage](evidence/stack-triage.json) · [Multichannel confirmation](evidence/multichannel-confirm.log) · [Similarity rerun](evidence/similar-retry.log)

The working SoundPlus process stayed on its original bundle at revision 4506 with 43 assets and catalog schema 7. Five catalog projections matched. Among 488 protected files, 485 were byte-identical. Three state files belonging to the running live-insert helper advanced. No files were added or removed in the audited set. QA used a separate catalog and process, and no receiving project was opened or modified. [Preservation audit](evidence/preservation.json)

## Recheck the retained evidence

Run from the reporting worktree. The validator reads the frozen checkout, then verifies the archived run and every case attachment. The PCM replay uses only the generated files in the archive.

```sh
frozen=/Users/harvest/SoundPlus-QA/m09-3be7178
archive="$PWD/docs/verification/m09-phase-1"
python3 -B "$archive/evidence/verify_dependencies.py" "$frozen"
python3 -B "$frozen/scripts/m09_acceptance.py" --checkout "$frozen" \
  --commit 3be7178ee0525791af3726dbb03b0fdaefec1130 \
  --manifest-sha256 c3c5a2f55bc2bf4174125845ff57185aa87d74a18c135951516701734634ecc0 \
  --check-run "$archive/run.json"
replay=$(mktemp -d)
unzip -q "$archive/evidence/generated-wavs.zip" -d "$replay"
python3 -B "$archive/evidence/verify_pcm.py" "$replay" > "$replay/observed.json"
cmp "$replay/observed.json" "$archive/evidence/pcm.json"
```

## Remaining acceptance

1. Continue local outstanding cases in small sessions. Phase 2 adds approved paid-provider tests and throwaway Resolve projects for both scripted placement and operator file drag. Verify receiving track, frame, media path and online state after save, quit and reopen.
2. Phase 3 covers real MIDI, microphone, calibrated surround and DAC routing, human listening and endurance. Capture device identities, physical channel observations and measured timing.
3. Phase 4 covers authorized Developer ID signing and notarization, clean install, upgrade/rollback, corresponding-source and license review, and the release owner's written beta decision.
