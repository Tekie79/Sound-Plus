# Track Mixer and native workspaces

November 23, 2026 · Apple M5 Pro · macOS 26.3.2 (25D2150) · arm64

The work started at `8d1eadca33f63b117d33c4c0e004eaa9db63103e`. The frozen build is `4fa36ef4a49d3d13a97c58f7bf0933a1527e564d` on `ft-ui-redesign`. The SoundPlus executable SHA-256 is `844dadf85aa79444ccced8f2940ec309ea852d26ace5843ca08393ec5dbe815d`.

## Changes

- Edit now has a resizable dock with Audio, MIDI, Group, FX Return and Master mixer strips. Track selection, fader, pan, solo, mute, recording controls and insert launchers use the existing session model. Peak, RMS and clip telemetry comes from the producer. The Master stays pinned during horizontal scrolling. [Mixer](../../../src/TrackMixer.cpp) · [Meter taps](../../../src/MixerMeters.h)
- FX Returns have their own track kind. Pre/post sends are disabled on creation and have stable IDs, level and enable automation, explicit destination roles and cycle checks. Group and FX arrivals share the graph's latency compensation. Legacy documents without mixer routing retain their existing format. New mixer routing uses document version 20 and frozen recipe version 11. [Routing](../../../src/TrackModel.cpp) · [Audio graph](../../../src/DesignAudio.cpp)
- Deliver publishes an aligned pre-Master batch for selected Track, Group or FX Return owners, with a common origin and tail. Parent and descendant combinations in the same batch are rejected. A verified batch has a receipt and distinct output paths. Export history, file reveal and file drag use finalized outputs. [Deliver](../../../src/DeliveryWorkspace.h) · [Stem tests](../../../tests/MixerGraphTests.cpp)
- Library, Edit, Perform and Deliver share a native navigation shelf, theme and command search. The Library has collections, results and preview panes. Edit has Controls, Effects and Routing inspector tabs. Perform retains its existing waveform cards, pad, axes, macro positions and resizing rules. Source-card tooltips omit full paths and multi-line instructions. [Workspace shell](../../../src/Workstation.h) · [Perform](../../../src/PerformView.cpp) · [Source cards](../../../src/SourceCard.cpp)
- Find Similar uses the installed local model and keeps the existing scope and cutoff. When analysis is available yet the model is missing, the control opens model setup. It is also available in the compact Library. [Library](../../../src/LibraryView.cpp)

## Evidence

| Area | Observed result |
| --- | --- |
| PCM and routing | Pre/post send matrix measured at 44.1, 48 and 96 kHz with blocks of 1, 32, 61, 257 and 512. Stereo impulses verified dry/wet alignment, mute, solo, return isolation and exact Send Enable at sample 1200 within `2.4e-7`. Save/reopen retained send identity and PCM. |
| Batch export | Independent Group and FX Return impulses matched at samples 900 and 1500. Stereo 48 kHz PCM24 files shared 14,192 samples including 6,000 tail samples. Stems summed to the linear pre-Master reference. The full Master render included its own chain. Rejection and cancellation left the prior verified batch intact. The native Deliver batch was verified independently against a real Master print at 48 kHz. Peak sample error was `1.13e-7`. After the naming correction, the repeated batch contained byte-identical PCM. [Native PCM receipt](native-stem-pcm.json) |
| Native Library | A real local inference query returned six results above 0.55 for a generated tone. Scores were descending and the reference was excluded. The Library-to-Edit action added the selected source. |
| Native Edit and Perform | At 1040×680 and 1440×900 on the Retina panel, Browser, Inspector, Mixer and transport were visible. The Mixer fader set to −6 dB matched the timeline's base gain. Command-K opened Perform. Native pointer drags assigned two clips and moved X and Y together. Command-S saved the isolated project to disk. Dark and Light appearance were exercised. The saved design reopened with the same source assignments, pad coordinates and fader value. A keyboard-driven FX Return and −12 dB pre-fader send survived a subsequent reopen. |
| Resolve receiver | Resolve Studio 21.1.2.10 imported a Master plus Group and FX Return stems through its native Import Media dialog. The Resolve API placed the three on stereo tracks 2–4 at frame 0. After saving, closing SoundPlus, quitting Resolve and reopening the receiving project, all three were Online and retained their seven-frame duration, IDs, channel mapping and source paths. The receiver references the QA-staged WAVs. [Receiver receipt](resolve-persistence.json) |
| Installed plug-ins | The Release qualification included the installed OldSkoolVerb and LV50A VST3s, qualified AUv2 automation, and the owned hosted MIDI fixtures. |
| Physical and listening | Built-in Stereo output was available. Physical six-channel playback and external MIDI controller tests were blocked by available hardware. Human listening was not recorded. |
| Package readiness | The local arm64 development bundle launched on this Mac. Its deep bundle-signature verification failed. No Developer ID-signed, notarized candidate was available for clean installation testing. |

The Resolve scripting import timed out on the initial workspace paths. The successful retry used the native file dialog and byte-identical QA copies under `~/Music/SoundPlus/Mixer QA/20261123-d7998372`. Two native file drags started and ended, and the Resolve Media Pool stayed unchanged. Those attempts failed the drag qualification. The available evidence cannot distinguish receiver rejection from pointer targeting. Internal Library-to-Perform drag passed.

The receiving WAVs were 48 kHz stereo, 24-bit PCM, 14,192 sample frames (0.295667 s). Resolve reports the duration as seven frames on its 24 fps timeline.

| Imported file | SHA-256 |
| --- | --- |
| `Untitled design-r1-92580a3e.wav` | `2367cedc4fa33f96d3b377c62b5c2f922277dbe0f88ae394606ecaf581387ea8` |
| `01 Repeated name.wav` | `2946fe0afe78f03c9faa65b3e951a93d671cb780d22579b6fbf5ea1b2d95929f` |
| `02 Repeated name.wav` | `311d682caafabd3a2be2e7dd0e7eda996171502bb5e51ef7a5e539281d0ff091` |

## 24-strip producer check

The first live-edit run exposed bidirectional socket backpressure. The audio helper stopped receiving controls while sending meter telemetry, and a parameter write timed out. The fix lets the helper continue reading while a bounded telemetry frame drains. The socket regression verifies that a large status frame remains intact while a command travels in the other direction. [IPC](../../../src/AudioIPC.cpp) · [Regression](../../../tests/AudioHealthTests.cpp)

A 120.13-second 5.1 Side session used 20 Audio Tracks, two Group Buses and two FX Returns at 48 kHz/512 frames on the built-in Stereo device. Native EQ, Advanced Delay and Algorithmic Reverb were active. The producer acknowledged all 465 fader/send edits with zero underruns and zero helper restarts. Its p95 upper bound was 3.85 ms, maximum fill was 7.14 ms, and measured edit acknowledgement peaked at 74.43 ms. The sampled helper RSS peaked at 65.2 MiB. Monitoring was muted. [Stress receipt](mixer-stress-24.json)

## Release gate and working session

The final arm64 Release run passed **56 of 56** suites in **559.75 seconds**. The freeze check matched 393 source, resource and script hashes, 17 executable hashes and 25 bundle files before and after. The previous candidate passed after replacing obsolete test color constants with current theme tokens. The final build also includes the transport correction from the stress run. [Release log](release-gate.log) · [Release identity](release-identity.json)

The working design opened in Perform at revision **4506** with both assigned waveform cards and the previous macro and axis values. The post-relaunch audit matched the saved design payload, all 409 referenced media/output files and all 18 catalog tables. The new workspace preference persisted alongside the existing settings. The app was relaunched with local analysis enabled to make Find Similar available. [Preservation receipt](workspace-preservation.json)

## Workflow coverage

The supplied [case workbook](SoundPlus_Track_Mixer_Test_Results.xlsx) retains 213 case IDs and its original status formulas. It records 47 passes, 1 failed drag check, 5 blocked, 1 inapplicable and 159 not run. Partial evidence appears in the actual-result column where a case's full steps were outstanding.

| Workflow | Status and boundary |
| --- | --- |
| W01 · Stereo MixConsole | Partial. Native fader parity, mute, Undo and Save were exercised. The whole four-track native sequence was outstanding. |
| W02 · Group routing | Partial. The Group path and group/child batch rejection had PCM evidence. The complete interactive folder and group sequence was outstanding. |
| W03 · FX Return and sends | Partial. Independent send tap, mute, solo and topology checks passed. Native keyboard creation, configuration, reopen and export of one Return passed. The multiple-return native sequence remains open. |
| W04 · PDC and plug-in failure | Partial. The impulse/block matrix passed. The installed plug-in gate exercised recovery separately. |
| W05 · Automation | Partial. Hidden automation and existing single-lane regression passed. Mixer Volume Touch and Send Touch were outside the verified native sequence. |
| W06/W07 · Side/Rear | Partial. Existing layout workflows and the implicit-role-map rejection ran. Independent checks confirmed Side mask `0x60f`, Rear mask `0x3f`, and byte-identical per-role mono splits. The full same-layout FX-send and per-role receiving scenario remains open. [Surround PCM receipt](surround-output-pcm.json) |
| W08/W09 · Recording and MIDI | Partial. Existing input/MIDI regressions ran. Complete new Mixer recording and instrument-to-aux native sequences were outstanding. |
| W10 · Perform | Partial. Native source assignment and axes, existing overdub regression, and frozen recipe roundtrip ran. The full new run with pad stems and routed FX was outstanding. |
| W11 · Stems and receiving DAW | Partial. Atomic cancellation, native and automated PCM, Resolve import and receiving persistence passed. Native drag failed to produce an imported item. The induced disk-full scenario is open. |
| W12 · Historical projects | Automated. Existing historical compatibility and take tests ran. Direct no-send JSON comparison passed. |
| W13 · Failure recovery | Partial. Cycle, ambiguity, invalid destination, duplicate send and cancellation checks ran. The complete induced failure sequence was outstanding. |
| W14 · Stress | Partial. Existing timing/device regressions ran. The 24-strip, 120-second native-effect stress run passed at 48 kHz. The combined hosted/MIDI scenario, other physical sample rates and extended soak remain open. |
| W15 · Keyboard and accessibility | Partial. Compact native layout, actual fader AX editing and Command-K/Command-S ran. The full VoiceOver strip traversal was outstanding. |
| W16 · Timed usability | Not run. |

## Reproduce locally

```sh
python3 scripts/guard_bundle_closed.py
.tools/cmake/data/bin/cmake --build build --config Release --parallel 10
.tools/cmake/data/bin/ctest --test-dir build -C Release \
  -R 'mixer_graph|mixer_stems|designer_canvas|designer_view_commands|library_similar_integrity' \
  --output-on-failure
qa_catalog="$(mktemp -d /private/tmp/soundplus-workflow.XXXXXXXX)"
build/SoundPlus_artefacts/Release/SoundPlus.app/Contents/MacOS/SoundPlus \
  --data-dir "$qa_catalog"
```

Add a QA sound folder in Library and select a clip. Try Find Similar with the local model ready. Add the clip to Edit and open Mixer. Add an FX Return, create a disabled send on the source strip, then enable it and compare its Pre and Post levels while moving the track fader. Choose **Deliver → Stems → Master inputs** to export the aligned set. Save the design and reopen it to inspect the routing and output history.
