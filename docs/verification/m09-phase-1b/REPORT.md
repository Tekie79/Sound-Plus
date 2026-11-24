# M09 Phase 1B acceptance

Six of the seven prior partial cases closed on November 24, 2026. One remains Partial for specialist acoustic review. The independently bootstrapped development application passed all 64 Release tests. Continue controlled acceptance. External distribution remains on hold.

## Case decisions

| Case | Decision | Measurement |
|---|---|---|
| M00-03 · parallel delay | Pass | A 10 ms wet delay with 0.5 feedback and a lookahead limiter returned into the dry stereo path. At 44.1, 48 and 96 kHz the limiter declared 221, 240 and 480 frames respectively. Signed dry impulses, echoes and feedback after clip end matched every sample through frame 12,287. Delay bypass doubled the aligned dry signal. Muting the return isolated dry. Maximum error was zero across all 45 rate, block and mode combinations. [PCM](evidence/pcm.json) |
| M00-05 · hidden curves | Pass | Reopened five documents from successive states of one track, with Volume and pre-fader Send Level reads disabled or enabled independently. Both ramps returned to their base dB values at frame 8,192. Maximum errors were 1.10e−8 FS for producer PCM and 5.59e−8 FS for finalized WAV. The actual disclosure on the reopened two-curve document exposed one Volume control and collapsed to zero while both curves kept their read flags. [PCM](evidence/pcm.json) · [View commands](evidence/lane.log) |
| M09-01 · clean build | Pass | The pinned bootstrap fetched JUCE and ONNX into a fresh detached worktree and installed the pinned build tools. Both candidate inventories verified. Source, provenance, configured dependency digests and the executable machine-code section matched. The independent Release gate passed 64/64 in 511.07 seconds. The rebuilt app launched against an empty QA catalog. [Comparison](evidence/build-comparison.json) · [Release log](evidence/release.log) · [Launch](evidence/rebuilt-launch.json) |
| M03-13 · Library audition | Pass | The actual Library coarse and refined commands sent two distinct play generations for the same asset revision. A controlled IPC sink captured the frozen production decoder/ring consumer at unity gain. All 46,900 coarse and 313 refined stereo frames exactly matched original Ls/Rs. Both prepared drag WAVs retained the six source channels and 0x60f mask. The half-open intervals were [100,47000) and [37200,37513). This measures pre-device audio and drag preparation. [PCM and receipt references](evidence/pcm.json) · [Source and captured files](evidence/generated-pcm.zip) |
| M08-12 · acoustic rejection | Partial | The dataset labels clip-021 as sea waves. The fresh score for NATURAL DISASTER was 0.444326519966125. In the QA catalog, Reject survived quit/reopen, Undo restored the suggestion, and an explicit Apply survived another restart. The product owner listened, described a disaster sound and agreed with the category. That judgment and the dataset label differ. Specialist acoustic adjudication remains open. [Native observations](evidence/metadata-ui.txt) · [Source and listening record](evidence/metadata-review.json) |
| M01-02 · credentials | Pass | A unique macOS Keychain account saved, returned and erased the synthetic credential. Four positive encoding controls worked. All 22 scanned files were clear, including saved edit/undo/redo, collected portable project, exported WAV and the OS crash report. The crash occurred in a disposable loopback client after its own POST. PID and executable UUID matched the OS record. Only the sanitized diagnostic is archived. [Scan](evidence/redaction.json) · [Diagnostic](evidence/diagnostic.json) |
| M01-07 · interrupted SFX | Pass for approved synchronous criterion | The product owner approved criterion 1B-sync-1. The loopback receiver counted one generation POST, then the interrupted client reopened at Needs review with no resubmission. A separate live native request used the owner's saved credential and produced one verified stereo WAV, 21,168 frames at 44.1 kHz (0.48 s). The UI used Auto duration and one candidate. The owner approved up to $2. The local receipt contains no invoiced amount. Live interruption and provider-side billing reconciliation remain outside this observation. [Approval and recovery](evidence/recovery-policy.json) · [Live candidate](evidence/live-provider.json) |

## Identities and preservation

The report branch began at `ec0c4689923a6e50b12cd14f0be97d0a860b3880`. Frozen source was `3be7178ee0525791af3726dbb03b0fdaefec1130`, tree `4f701df30da740c426127255cacdd6594c89986c`, executable SHA-256 `ca92909592d92c7e7ac09a0e0b48553e3167c63af84e07bdd25797711876b97f`, manifest `c3c5a2f55bc2bf4174125845ff57185aa87d74a18c135951516701734634ecc0`. The independent executable was `6abdca749c1d9bae3a570236f02e7efc45134be41a85878653e156facbf8bdbc`. Of the file-backed Mach-O sections compared, only the C-string section differed. Its printable-string difference was a clock value, 10:20:16 versus 12:21:49. Whole-binary hashes therefore differ. Both builds used Apple clang 21.0.0 and SDK 26.5 on arm64 macOS 26.3.2. [Build evidence and exact command templates](evidence/build-comparison.json)

Run `b750c9eb-2afa-4557-b7a8-022ca186aadf` used an exclusively created QA directory under `~/SoundPlus-QA/phase1b.SN9DTO`. The earlier run, its 42 archived files, its separately retained anchor and its checklist were checked against the reviewed tip. The working design was independently observed at revision 4506 before and after. Of 488 protected files, 485 remained byte-identical. Three state files advanced in the running live-insert helper. All five catalog projections matched. User media, recording journals and the active Resolve project stayed outside the QA workflow. [Custody](evidence/custody.json) · [Preservation](evidence/preservation.json)

## Runner diagnosis

The arm64 multichannel test reserves 8,467,408 bytes after a 128-byte register save, exceeding the observed default soft stack of 8,372,224 bytes before fixture work begins. A Python attempt to raise the limit after interpreter startup was refused. Launching through zsh with `ulimit -s hard` yielded 67,092,480 bytes in the CTest child. Two executions of the same frozen test binary passed with 885,231 and 888,051 checks and zero reported native error. The runner also validated a fresh QA-owned `similar-test-output` before launching the similarity case. This evidence locates the original failure in test setup. [Disassembly](evidence/stack-frame.txt) · [CTest commands and output](evidence/stack-repeat.log)

## Remaining matrix

| Scope | Pass | Partial | Fail | Blocked | Not Run |
|---|---:|---:|---:|---:|---:|
| Fresh Phase 1B only | 6 | 1 | 0 | 0 | 392 |
| Combined P0 | 8 | 0 | 0 | 0 | 173 |
| Combined P1 | 15 | 1 | 0 | 0 | 198 |
| Combined P2 | 0 | 0 | 0 | 0 | 4 |
| Combined Phase 1 and 1B | 23 | 1 | 0 | 0 | 375 |

The combined rows retain the 17 previously verified passes. The original registry and its historical run remain independently readable. Synchronous recovery approval applies specifically to ElevenLabs SFX. Asynchronous providers still need their original job-ID oracle. Phase 2 can use a separately authorized spending scope and a dedicated throwaway Resolve project for receiver placement, drag and reopen. Physical hardware, specialist listening, distribution signing, notarization and release sign-off remain open. [Fresh ledger](run.json) · [Previous ledger](../m09-phase-1/run.json)

## Recheck

From the reporting worktree, the official validator reads the frozen candidate and verifies its attachments. The second command independently reads every evidence file, checks the identity bindings and replays the DSP and Library PCM oracles using the archive alone.

```sh
frozen="$HOME/SoundPlus-QA/m09-3be7178"
archive="$PWD/docs/verification/m09-phase-1b"
python3 -B "$frozen/scripts/m09_acceptance.py" --checkout "$frozen" \
  --commit 3be7178ee0525791af3726dbb03b0fdaefec1130 \
  --manifest-sha256 c3c5a2f55bc2bf4174125845ff57185aa87d74a18c135951516701734634ecc0 \
  --check-run "$archive/run.json"
python3 -B "$archive/evidence/recheck.py" "$archive"
```
