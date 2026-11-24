# SoundPlus workspace transfer qualification

**Release decision · No-Go for full cross-application qualification.** The native Send workflow, verified copy pipeline, and drag preparation passed the checks below. Five drag cases require physical mouse delivery into Resolve and subsequent receiver persistence. Six other cases have partial coverage.

## Workflow

- Use the **Resolve** or **Copy** icon in Library Preview, Edit, or Perform. The icon runs the saved action directly. In Drag WAV mode, click to prepare the file and drag the same icon when **Ready to drag** appears. The adjacent gear opens the scope and destination options. Choose **Save a copy** and select its destination in the macOS folder picker. Deliver → **Transfer receipts** opens the receipt directory.
- Library uses original source samples. The marked range is half-open, with IN included and OUT excluded. A full-file copy retains all original bytes. With Copy off, the full scope uses the verified source path and a range uses a finalized WAV in the catalog’s transfer-media folder.
- Edit track and Group exports tap the selected owner after its processing and PDC, before downstream Group and Master processing. Full track stems retain the design’s padded timeline and configured tail. Selected clip isolates the clip, retains its source region, fades and inserts, and passes through the owning track. Project ranges warm up from frame zero and stop at the exclusive end. Full and ranged mixes include Group and Master processing. Render sample rate and layout use the existing export settings.
- Perform selected pad isolates its owned sources, retains its motion and routing, and includes the common Master. Shared MIDI is excluded from an isolated pad and included in the combined mix. With nonlinear processing, summing isolated pads can differ from the combined output. Frozen transfers resolve the recorded recipe and include its recipe and event hashes in the receipt.
- For quick sends, select an audio track in Resolve’s Fairlight page. Resolve inserts audio on its active track at its playhead. Its scripting API exposes the insert command but does not expose the active track number, so SoundPlus checks the interval and layout on every unlocked candidate. If another track blocks this check, use the gear’s **Track override** and send from there. An explicit track also supports the Resolve Edit page. Collision checking rounds duration up for the protected interval. Post-placement validation uses Resolve’s actual subframe end, allowing up to one record frame of duration difference. Retrying a known pre-placement failure reuses the verified file and keeps prior attempt data.

### Quick-action follow-up

- On Resolve Studio 21.1.2.10 at 24 fps, the native Library Resolve icon placed audio onto A2 at frame 384, then onto A1 at frame 480 after the active track changed. Both items, media IDs, frame intervals and online paths survived Save, Quit and Reopen.
- An occupied A1 range failed before placement. The IDs and bounds of all existing timeline items on both tracks were identical afterward.
- Clicking the Copy and Resolve icons kept the workspace visible. A gesture on the prepared Copy icon recorded actual macOS drag start and end. Cross-application drop acceptance remains open.
- Six focused Release tests and the independent workspace-transfer PCM suite passed. The updated Release executable SHA-256 is `e9adca2b895ee8cfccd4865e715f2d18ed0c31f133fc636d5c3bb0610feaa5de`.
- Working revision r4506 and 429 audited media files matched after reopening the updated build. Catalog tables and working state matched the protected snapshot. The original Resolve project and timeline were restored.

## Original milestone build and verification identity

| Item | Verified value |
| --- | --- |
| Reviewed baseline | `5a64aaafdb655947826dd4d4d9ec6d8f996eb406` |
| Implementation | `cb20ed9841399b53966993714586865fb0b11228` |
| Host | Apple Silicon arm64, macOS 26.3.2 (25D2150) |
| Native Release app SHA-256 | `68801ad60e7d489edf2f70825a60488cd6c800a5aed8e69eb1519ff0bb664e34` |
| Bundled Resolve bridge SHA-256 | `87021afcc7dd7e8c9dca1d34e4629740419583fd119379717bc78632ced8c786` |
| Receiver | DaVinci Resolve Studio 21.1.2.10, local scripting |
| Final unchanged Release gate | **56 passed, 0 failed**, 489.58 seconds |
| Focused transfer suite | Passed. Independent 16-bit PCM oracle matched every requested sample and channel in 21,810-frame WAV excerpts. Direct RIFF readback verified masks `0x60f` and `0x3f`. Independent mix oracle passed all tested scopes within `1e-6` full scale. |
| User-state audit | Reopened r4506. Latest design payload, all 425 referenced media files and all 18 audited catalog tables matched. Audit retained 197 state files with no exceptions. Original Resolve project and timeline IDs restored. |

```sh
python3 scripts/guard_bundle_closed.py
.tools/cmake/data/bin/cmake --build build --target SoundPlus SoundPlusTests --config Release --parallel 10
.tools/cmake/data/bin/ctest --test-dir build -C Release --output-on-failure
build/SoundPlusTests_artefacts/Release/SoundPlusTests --workspace-transfer "$PWD/build/workspace-transfer/final-pcm"
open build/SoundPlus_artefacts/Release/SoundPlus.app
```

## Acceptance matrix

| ID | Status | Execution evidence |
| --- | --- | --- |
| TX-LIB-01 | Passed | Native exact range `[377,22187)`. Copied PCM matched the original sample slice byte for byte. Resolve placed at frame 24 and reopened online. |
| TX-LIB-02 | Passed | Native 48,000-frame stereo copy and original had identical bytes and SHA-256. Resolve referenced the copy at frame 0. |
| TX-LIB-03 | Blocked | Verified copied range and Ready to drag. Physical delivery into Resolve remains open. |
| TX-LIB-04 | Blocked | Copy-off native preparation returned the exact original path. Physical receiver acceptance remains open. |
| TX-ED-01 | Passed | Native selected track at frame 120. Receipt retained the occupied-track failure and same-path retry. Independent PCM matched the owner tap. Existing mixer tests exercised PDC and automation. |
| TX-ED-02 | Passed | Native track range `[41,1600)`, 1,559 samples, frame 192 after retry. Independent PCM matched the crop. Existing graph tests cover processing history. |
| TX-ED-03 | Blocked | Native mix-range WAV at 1,559 samples. Independent PCM matched. Physical drag remains open. |
| TX-ED-04 | Partial | Native selected clip created a separate verified copy and the independent PCM matched the isolated owner. This fixture had zero fades and an empty source rack. |
| TX-ED-05 | Passed | Native entire mix at frame 624. Independent oracle matched the Track, Group and Master gains applied once. |
| TX-PER-01 | Partial | Native Pads A–D at frames 264, 336, 408 and 480 with distinct isolated PCM. Existing multi-pad suite exercised independent motion. The transfer fixture used stationary pads. |
| TX-PER-02 | Blocked | Native Pad D range `[41,1600)`, 1,559 samples. Independent PCM matched. Physical drag and moved-control range QA remain open. |
| TX-PER-03 | Partial | Native all-pad mix at frame 552 matched the independent sum in the linear fixture. Shared MIDI in a combined transfer still needs a native fixture. |
| TX-PER-04 | Partial | Recorded and placed a complete take at frame 696 with recipe and events SHA-256. Missing-take preparation rejected. Explicit incomplete-take UI case remains open. |
| TX-GLO-01 | Passed | Folder selected with the macOS picker. Shared action and folder used across all three workspaces. Reopened QA preferences retained action, copy policy, bookmark and per-workspace scopes. |
| TX-GLO-02 | Passed | Native Copy-off full source used the original path. Focused tests verified a Copy-off excerpt in durable storage and preserved its SHA-256. |
| TX-GLO-03 | Passed | Duplicate names published unique paths with identical bytes. Native read-only and full mounted HFS+ fixture rejected copying with no WAV published. The image was detached. |
| TX-GLO-04 | Partial | Removed the selected QA source temporarily and confirmed no transfer was published. Focused tests rejected stale source identity. Existing advanced-effect tests cover IR integrity. A native changed-IR transfer remains open. |
| TX-RES-01 | Passed | Native SoundPlus sent while Resolve was closed and again with an active project without a timeline. Both reported failure with a verified drag fallback. |
| TX-RES-02 | Passed | Native Resolve record frames independently matched 24 fps at 24, 25 fps at 90132, 29.97 DF at 109754 and 30 fps at 108157. DF example used `01:01:02;04` with a nonzero timeline origin. |
| TX-RES-03 | Passed | Production bridge refused a locked track and an occupied interval in isolated projects. Native occupied-track retry succeeded at a clear interval with the same WAV. |
| TX-RES-04 | Passed | Production bridge independently observed Mono, Stereo, 5.1 Side and 5.1 Rear imports, compatible receiver tracks, source channel indices, sample rate and online paths. The Side and Rear files retain distinct role masks. Physical speaker routing and listening were outside this check. |
| TX-RES-05 | Passed | Saved the primary QA project, quit both applications, and reopened Resolve. All 11 items retained timeline item and media IDs, positions, durations and online source paths. Resolve referenced the authoritative paths. |
| TX-RES-06 | Blocked | Programmatic pointer drag produced no native-drag trace. Physical cross-app gesture, Resolve import, placement and reopen remain open. |
| TX-REG-01 | Passed | Existing ordered corrections, plugin takes, take integrity and multichannel suites passed on the final build. The transfer creates separate receipts and adds no design or take schema fields. |
| TX-REG-02 | Passed | Native Edit and Perform captures, unchanged canvas and pad implementations, and passing automation, surround and multi-pad suites. |
| TX-REG-03 | Partial | Focused cancellation, native receiver failure and retries passed. No transfer code enters the audio device callback. Native cancellation during sustained rendering or competing seek and helper timeout still need dedicated execution. |

**Matrix totals · 15 Passed, 6 Partial, 5 Blocked.** Human listening was not conducted. Numeric PCM evidence is independent of audition and physical speaker routing.

## Local execution records and native captures

The QA catalog, source fixtures, copied WAVs and receipts remain local. The full paths, SHA-256 values, item IDs and record frames are in the JSON evidence.

- [Native receipt verification](../build/workspace-transfer/native-receipts-verified.json)
- [Resolve saved records](../build/workspace-transfer/receiver/saved.json) and [reopened records](../build/workspace-transfer/receiver/reopened.json)
- [Resolve mapping, rates and safety checks](../build/workspace-transfer/receiver/rates.json)
- [Final Release log](../build/workspace-transfer/release-ctest.log) and [focused PCM log](../build/workspace-transfer/final-pcm.log)
- [User-state audit after reopening](../build/workspace-transfer/after-reopen.json)
- [Library full source](../build/workspace-transfer/screenshots/library-full-source.jpg), [Edit track retry](../build/workspace-transfer/screenshots/edit-track.jpg), [Perform Pad A](../build/workspace-transfer/screenshots/perform-pad-a.jpg), [Resolve timeline](../build/workspace-transfer/screenshots/resolve-before-quit.jpg), and [Deliver receipts after restart](../build/workspace-transfer/screenshots/deliver-reopened.jpg)

For the physical pass, load the isolated SoundPlus catalog listed in `build/workspace-transfer/native-qa-path.txt` and open the isolated **SoundPlus Workspace Transfer QA 20261123** project in Resolve Studio. Prepare the Library full source, Library marked range, Edit mix range and Perform pad range. Physically drag each **Drag prepared audio** handle into the Media Pool and place its imported item on a compatible audio track. Compare the imported path, channel mapping and duration with its receipt. Save, quit both apps and reopen Resolve to verify that each item remains online at the confirmed position.
