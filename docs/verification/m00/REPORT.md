# M00 Mixer and Resolve qualification

**Release decision · No-Go for full M00 acceptance.** The tested candidate fixes two workflow defects. The Release gate, focused PCM checks and verified Resolve transfers passed. The fresh case records retain open native, device and operator coverage. The package sequences later milestones after M00.

## Corrections

- Fairlight interpreted a 44,100-sample request on its 48 kHz timeline clock. The original stopped at 22.05 frames of a 24 fps second. SoundPlus now reads Resolve’s `timelineSampleRate`, converts the duration with integer arithmetic, and preflights the converted interval before invoking the active-track insertion. The original WAV and its sample rate remain authoritative. An unavailable timeline clock produces a specific refusal with the numbered-track option available. [Bridge](../../../resources/resolve/transfer.py) · [Clock regression](../../../tests/test_resolve_transfer.py)
- Deleting a bus with incoming mixer sends now requires an explicit choice. The track menu and Delete shortcut share the confirmation. Cancel and Escape retained the bus and its send. Confirm removed them, and Undo restored the same track and send IDs, pre-fader mode and −12 dB level. The model rejects an unconfirmed call before mutation and still checks Group output routes. [Track model](../../../src/TrackModel.cpp) · [Native editor](../../../src/MultitrackEditor.cpp) · [Graph regression](../../../tests/MixerGraphTests.cpp)

## Signal and workflow checks

- The native Mixer and Edit timeline both displayed −6 dB. Undo returned both to 0 dB and Redo restored −6 dB. The reopened QA design retained the fader, the Return and the enabled pre-fader send. Each 48 kHz stereo render had 96,000 samples including the one-second tail. The independent raw WAV reader verified the stored stem hashes and compared every original sample against the dry gain, pre-fader return and Master sum. Maximum error was `6.30e-8` full scale. [PCM receipt](native-stem-pcm.json) · [Reproduction oracle](../../../scripts/audit_mixer_exports.py)
- Fresh mixer tests ran both tap positions at 44.1, 48 and 96 kHz with blocks of 1, 32, 61, 257 and 512 frames. They checked PDC, disabled sends, mute, solo, route cycles, hidden send automation, frozen mixer recipe identity, and same-name stems. A separate transfer PCM run verified exact 21,810-sample excerpts, named Side and Rear masks, and Edit/Perform scoped mix PCM. [Release log](release-gate.log) · [Transfer PCM log](transfer-pcm.log)
- The 24-strip, 120.2-second run acknowledged all 472 sequential parameter edits with zero reported underruns or helper restarts. Producer fill was at most 6.36 ms, with a 3.00 ms p95 upper bound. Maximum observed edit acknowledgement was 69.79 ms. The helper’s sampled RSS peaked at 65.4 MiB. The graph used native EQ, delay and reverb with muted monitoring on the built-in stereo device. This workload covers live transport and the mixer producer. The independent duplex regression covers a full telemetry socket and bounded command timeout. [Stress receipt](mixer-stress.json)
- A separate owned 24 MiB volume produced real ENOSPC and preserved the previous saved design byte for byte. The render cancellation and helper recovery checks ran through `mixer_stems` and `audio_health`. Mid-batch ENOSPC and a full sustained workload with indexing, microphone and hosted instruments remain separate exercises. [Storage check](enospc.log) · [Release log](release-gate.log)
- The installed-vendor automation opt-in ran separately on the frozen build. Six exact parameter-scheduling cases passed across OldSkoolVerb, LV50A and AUDelay. The three Python tests for the Fairlight clock passed separately. [Vendor log](hosted-vendor.log) · [Fairlight clock tests](fairlight-clock.log)

## Resolve receiver

Resolve Studio `21.1.2.10` verified actual local media IDs, channels, sample rates, online source paths and audio-only timeline intervals. The 24, 25, 29.97 DF and 30 fps projects used independently checked record frames. The occupied and locked-track cases left the existing item IDs and intervals intact. The clear A2 override reused the original imported media. The original 5.1 Side and Rear fixture files retained masks `0x60f` and `0x3f` with the same ordered six source indices.

| Native Fairlight send | Resolve clock | Target | Start frame | End frame |
| --- | ---: | --- | ---: | ---: |
| 44.1 kHz mono | 48 kHz | A2 | 87216 | 87240 |
| 44.1 kHz mono | 96 kHz | A1 | 96 | 120 |
| 48 kHz stereo | 48 kHz | A1 | 87408 | 87432 |
| 96 kHz stereo | 48 kHz | A1 | 87312 | 87336 |

The receiving projects retained seven scripted placements, four native quick-send placements and two user-operated physical drops after both QA applications quit and Resolve reopened. Each physical drop created its own Media Pool item. Resolve referenced the verified files in place and retained both audio tracks, exact frame intervals, channel mappings and source paths. The earlier automated gesture started a native drag and produced no Media Pool item. The three scoped range drag cases remain open. [Receiver records](receiver-qualification.json)

| Physical drag source | Result | Resolve track and interval |
| --- | --- | --- |
| Library `stereo_44100_impulses.wav`, 44.1 kHz, stereo, 1 second | Pass. Referenced original, online after reopen. | A1, `[87480, 87504)` |
| Deliver `02 FX Return.wav`, 48 kHz, stereo, 2 seconds | Pass. Referenced finalized stem, online after reopen. | A2, `[87576, 87624)` |

## Candidate and preserved workspace

- Package review `7268a31731ececde59a45363aca8b0231da1c016`. Working baseline `6d6a2381c96740ee17c7fbebd377248517a8e9a7`. Built source `650dd5adb2b0fcf9b32f877acdeb971893ddd560`.
- Apple M5 Pro, arm64, macOS 26.3.2, SDK 26.5 and JUCE 8.0.6. Full unchanged Release CTest finished with **56 passed, 0 failed** in **489.39 seconds**. The before/after manifest matched 406 source, resource and script files, 67 Release artifacts and 26 app bundle files. [Identities](release-identity.json) · [Release log](release-gate.log)
- SoundPlus stayed open on the working catalog while QA used a separate bundle and isolated catalog. The audit matched working revision 4506, all 433 media files, and all 197 retained state files. The 18 catalog tables matched except for 43 ordinary scan bookkeeping values in `assets.seen`. The original Resolve project, timeline and Fairlight page were restored. [Preservation audit](workspace-preservation.json)

## Guided workflows

| ID | Fresh coverage |
| --- | --- |
| W01 | Partial. Native fader/Undo/Redo parity and exact independent stereo PCM. The full four-track pan, mute and solo sequence remains open. |
| W02 | Partial. Atomic route-cycle rejection and safe incoming-send deletion tested. Full interactive folder/Group sequence remains open. |
| W03 | Partial. Native pre-fader Return, −12 dB send, and master/stems verified. Automated pre/post, mute/solo and PDC checks passed. The two-Return stateful run remains open. |
| W04 | Partial. Owned latent send, plug-in failure regression and installed automation exercised in their own suites. The full reordered routed-rack sequence remains open. |
| W05 | Partial. Hidden send curve and exact enable event verified. The combined native Touch workflow remains open. |
| W06 | Partial. Side mask, order, automated Square Surround and receiver mapping passed. Physical speaker output remains unavailable. |
| W07 | Partial. Rear mask, order, implicit-map rejection and receiver mapping passed. Full native bed/Return workflow and physical output remain open. |
| W08 | Partial. Automated capture lifecycle and finalization ran. No physical microphone capture was conducted. |
| W09 | Partial. Owned MIDI instrument and automation cases ran. The full native MIDI-to-Return workflow remains open. |
| W10 | Partial. Multi-pad replay and independent pad/mix transfer PCM ran. Full hands-on overdub and delivery remains open. |
| W11 | Partial. Native aligned stereo stems and master PCM, scripted placement and two physical drops survived receiver reopen. Delivery of the complete stem batch remains open. |
| W12 | Partial. Historical manifests and take replay regression suites ran. The complete manually opened project range remains open. |
| W13 | Partial. Actual ENOSPC, cancellation, stale sources, and helper recovery passed in separate tests. Full mid-render/Resolve timeout procedure remains open. |
| W14 | Partial. The 120-second, 24-strip run passed with every edit acknowledged. Combined active indexing and additional input/hosted workloads remain open. |
| W15 | Partial. Native mixer interaction, track-menu keyboard navigation, and 1040×680 layout with Browser and Inspector checked. Full VoiceOver traversal and non-Retina check remain open. |
| W16 | Blocked. A human timed idea-to-stems session was unavailable. |

The [case records](case-results.json) cover all 213 Mixer workbook IDs, 26 transfer IDs and 12 M00 IDs. The workbook’s **M00 Run** tab records fresh observations and assignment. Its **Cases** and **Start Here** tabs retain the historical statuses and counts. [Mixer workbook](../track-mixer/SoundPlus_Track_Mixer_Test_Results.xlsx)

| Case set | Pass | Partial | Blocked | Not Run |
| --- | ---: | ---: | ---: | ---: |
| Mixer workbook | 50 | 13 | 5 | 145 |
| Transfer | 8 | 15 | 3 | 0 |
| M00 package | 7 | 4 | 1 | 0 |

These are fresh results. The five Mixer blockers include physical surround/MIDI equipment and a distribution-signed candidate. Human listening was outside the measured PCM and physical drag checks.

## Reproduce the local checks

```sh
cmake_bin="$PWD/.tools/cmake/data/bin"
python3 scripts/guard_bundle_closed.py build/m00-release/SoundPlus_artefacts/Release/SoundPlus.app
"$cmake_bin/cmake" -S . -B build/m00-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_MAKE_PROGRAM="$PWD/.tools/bin/ninja" -DSOUNDPLUS_BUILD_PLUGIN_ASAN_QA=ON
"$cmake_bin/cmake" --build build/m00-release --parallel 10
"$cmake_bin/ctest" --test-dir build/m00-release -C Release --output-on-failure
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_resolve_transfer.py' -v
build/m00-release/SoundPlusTests_artefacts/Release/SoundPlusTests \
  --workspace-transfer "$PWD/build/m00-transfer-fixture"
m00_catalog="$(mktemp -d /tmp/soundplus-m00.XXXXXXXX)"
build/m00-release/SoundPlus_artefacts/Release/SoundPlus.app/Contents/MacOS/SoundPlus \
  --data-dir "$m00_catalog" --disable-ai
```
