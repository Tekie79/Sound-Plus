# Multitrack Edit foundation and single-lane automation v1

Started from **ed9893a**, compared with reviewed **bcb6c14b7dbef5d59dc4dbd178319e7b2a9f98b8** and plugin-take implementation **938a219943fd4f81eb0ec552918b9d52546d9242**. The newer compact, resizable Perform layout is retained. This is an Edit increment; existing Perform and Library workflows remain separate.

## Delivered

| Request | Result / implementation |
| --- | --- |
| A — Track/Clip/media identity | Independent persistent Track IDs and clip/media references; empty Audio tracks, multiple clips per owner, names/colour/base volume/mute/solo/inserts, cross-track move/copy, split/trim/slip/gain/custom and preset fades. [Model and commands](../src/TrackModel.cpp), [native editor](../src/MultitrackEditor.cpp). Overlap is an explicit independent sum. |
| A — organizational and audio ownership | Nested Folder parents with collapse, reparent/reorder and unwrap-delete. Independent Group output routing, cycle/missing-owner rejection, protected deletion of referenced buses, folder mute/solo without overwriting local flags. Distinct Clip source, Track, Group and Master processing. MIDI has a typed dedicated Instrument slot contract, with no unsupported creation/hosting control. |
| B — compatibility | Manifest **11 / multitrack-automation-v1**; lazy materialization at the first Add track operation gives each legacy clip its own compatible Audio Track. Old source-stage effects/routing/Perform IDs stay in place; they are never merged into one shared processor. Loading old documents does not rewrite historical takes. [Serialization/validation](../src/DesignModel.cpp), [signal path](../src/DesignAudio.cpp). |
| B — frozen new performances | Recipe **4 / track-automation-takes-v1** freezes complete tracks/clips/automation and independent fixed plugin state. Existing recipes1–3/hashes/events remain unchanged. Track automation and source-stage Perform controls compose at their explicit stages. [Take contract](../src/PerformTake.cpp). |
| C — one reusable lane | One collapsed-by-default lane for each processable owner, including Master; Volume initially selected. Searchable Track Controls/Inserts selector; independent shown target, Read, Touch arm and stored curves; stable owner/instance/parameter binding; unresolved targets retained. External registry entries remain non-writable. [Editor](../src/AutomationEditor.cpp), [registry/evaluator](../src/TrackModel.cpp). |
| D — editing and playback | Point/range selection, add/move/delete, real-unit numeric editing, copy/paste, range offsets, Linear/Hold, snap/bypass and one Undo per gesture. Bounded native Touch uses the actual owned producer clock, captured target and100ms return. Hidden/collapsed curves continue. Shared live/offline evaluator with per-sample Volume and deterministic native control segments/PDC. Prepared snapshots and all JSON/state/I/O stay outside the device callback. [Live controls](../src/DesignLive.cpp), [ordered processing](../src/OrderedRack.inc). |
| E — preservation and UI | Separate Track/Master inserts remain; an explicit Clip source-insert section makes legacy processors editable after migration. Lucide/theme, fixed subdued10-point fade corners, original fades, pools, variations and newer Perform splitter are retained. Minimum-size layout and native gestures were exercised; evidence below. |
| F — follow-ons | Audio Record, MIDI scheduling/instruments, PadA–D shared-clock overdubbing, external parameter scheduling and the agreed remaining product roadmap are explicitly separated in the [contract/roadmap](MULTITRACK_AUTOMATION_CONTRACT.md). No inert controls claim these features. |

The [contract](MULTITRACK_AUTOMATION_CONTRACT.md) documents activation/migration, signal order, overlap, project-time follow policy, target identity, limits, gestures, Touch, control accuracy, seeks/tails, selection-render semantics and follow-on increments. A new multitrack action is undoable; it does not change the user's legacy Perform session merely by reopening it.

## Verification evidence

Supported hardware: Apple M5 Pro / arm64, macOS26.3.2, JUCE8.0.6, Release build. Quality-engineering checks use owned generated PCM, separate catalogs/documents and controlled worker/session services.

- **Focused multitrack tests:**188 checks including empty-insert persistence, selection rebase, spatial routing and clip-only highlight assertions, covering empty tracks, IDs, safe hierarchy/output graphs, independent copied source processors, split seams, hidden simultaneous Volume/filter curves, stable duplicate-effect targets, unresolved deletion, project control snapshots, rate conversion/accurate seeks/fresh loop restarts, eight named roles and split-member persistence. Actual editor commands and pointer-target changes, copy/numeric/range/Undo and controlled Touch capture/return use the real view/session boundary. [Source](../tests/MultitrackTests.cpp).
- **Historical compatibility:** generated schema envelopes1–10 compare PCM before/after loading; these are synthetic representative encodings, not claimed to be ten historical application binaries. Existing genuine v7 fixtures/seeded-order regressions and historical stock/plugin take fixtures are separately exercised by the required gate. The retained59-artifact audit was rerun from the reviewed evidence before implementation; its result does not substitute for a new native run.
- **Latency oracle:** two Audio Tracks with reported latencies7/19 frames route into Group11 and Master13. Output equals the independent1.5× source oracle within2e-6. An off-grid Group filter curve after a hosted prefix gives the same PCM across257/61-frame partitions. New recipe4 plugin ownership/hash round-trip is included. [Source](../tests/OrderedInsertTests.inc).
- **Actual device capture:** the existing plugin take suite now covers both historical capture and a kept recipe4 containing hidden Track automation, alongside actual capture-state failure/recovery checks. Pre-monitor device-consumed capture equals frozen replay within1e-6. The prior focused run passed4/4 suites in95.05s, including plugin_takes35.51s and ordered_inserts58.13s. [Extension](../tests/PluginTakeTests.inc), [focused log](verification/multitrack/focused.log).
- **Native minimum and desktop Edit:** actual1040×680 outer window with Browser and Inspector visible, plus expanded desktop. Fixed a selector/Touch-slider overlap and UTF-8 separator defect found during inspection. Toolbar, owner controls, output, focus/numeric dialog and timeline remain usable; Inspector and track rows scroll. Track selection hides clip-only controls and immediately exposes its insert owner; clip selection outlines only that clip. [Minimum Track context](verification/multitrack/minimum-track.jpg), [native observation record](verification/multitrack/native-observations.json). [Minimum](verification/multitrack/minimum.jpg), [desktop](verification/multitrack/desktop.jpg), [native observations](verification/multitrack/native-observations.json).
- **Native editing:** menu-created empty Audio Track, double-click/keyboard rename, stock insert while empty, generated Library sound drag-in, second clip on that same Track, native cross-track clip drag and keyboard Undo, folder collapse/expand, target search, double-click keyframe insertion, Command-A/Return numeric editing and one-step Undo. Native receiving-app file delivery was not part of these internal canvas checks.
- **Native Touch:** the successful pass wrote frames322560 and323584 and returned to the captured underlying value at328384, exactly4800 frames/100ms after release at48kHz. One native Command-Z removed the entire pass while preserving the four original points. The short first attempt ended before writing; it is not counted as a pass. [Independent saved-model comparison](verification/multitrack/native-touch.json). Monitoring was muted; this is UI/model/device evidence, not human listening.

The first full gate passed33/34 but exposed a new producer-throughput problem in recipe4: only2048 frames were consumed while the test sent550ms of control gestures, leaving no durable events. Its [failed log](verification/multitrack/initial-gate-failure.log) is retained. The initial global32-frame scheduler multiplied hosted IPC calls. Native effect segmentation now runs inside the ordered native processors; hosted calls retain blocks up to512 frames. Pan/spatial coefficient updates are local and smoothed. The post-correction focused device run consumed30208 frames, saved9 events, reported0 underruns and matched replay with0.0 maximum PCM error; historical recipe3 consumed29696 frames /10 events /0 underruns. A consumed-clock floor now guards the regression. [Measurement](verification/multitrack/throughput-measurement.json), [six captured source/checkpoint/float artifacts with SHA-256](verification/multitrack/focused-capture-artifacts.zip). The same focused run passed3/3 in56.16s. Spatial routing also gained explicit L/R/LFE and partition checks; unsupported mono-to-mono Pan is not advertised.

The final unchanged full-gate result, exact source/build identities and in-place user restoration audit are appended after execution. No new ASan qualification is claimed.

## Build, launch and five-minute smoke check

Close the running bundle normally before rebuilding:

```sh
.tools/cmake/data/bin/cmake -S . -B build
.tools/cmake/data/bin/cmake --build build --target SoundPlus SoundPlusTests --parallel 8
build/SoundPlus_artefacts/Release/SoundPlus.app/Contents/MacOS/SoundPlus \
  --data-dir "$PWD/build/gain-hover-native"
```

Use a separate saved design for this check:

1. In Edit, hover the toolbar to find **Add track**, create an Audio Track and rename it by double-click/Return. Add a stock insert in Track inserts before adding any media.
2. Drag two Library sounds onto the same empty Track. Move one to another Audio Track, Undo/Redo, trim or adjust its fixed fade corner. Option-drag copies; Shift-Option-body slips; Command-E splits at the playhead. The originals remain in place.
3. Expand the Track's disclosure. Volume appears in its single lane. Double-click two points; select/drag, Shift-select or rectangle-select, Return for exact time/value, and Undo. Search the selector for a Filter parameter and draw a second curve. Choose Pan or collapse the lane; both saved Read curves continue playing.
4. With playback actually running, arm Touch on a supported target and drag its lane slider. Stop; one Undo removes the completed pass. The documented100ms return is part of the pass.
5. Make a Folder and a Group using Add track. Track options set Folder membership separately from Audio output. Save, quit, reopen, and confirm the clips, empty tracks, hierarchy, inserts and both automation curves. Render Design and compare by listening yourself. If testing named surround, check the chosen export roles and use qualified monitoring independently.

## Remaining qualifications

Audio-input recording, MIDI events/native or external instruments, multi-pad Perform and general external-plugin parameter automation are next increments. External registry entries are not scheduled automation. Live seek retains the existing approximate250ms history; Accurate audition is the deliberate full-history path. Native controls schedule in32-project-frame segments plus exact keyframe/prefix boundaries and existing DSP smoothing; only owner Volume is claimed per-sample. Project-time curves stay with their owner when clips move.

OldSkoolVerb custom input, human listening, physical surround output and new native cross-application delivery remain open. Existing Resolve-referenced finalized WAV paths are preserved. Internal canvas drag success is not a Resolve receiving-project qualification, and retained historical receiving evidence is not relabeled as a new multitrack delivery test.

## Final closure

**Final unchanged implementation:34/34 suites passed in303.33s**, with vendor, AU and real-device branches enabled. [Final log](verification/multitrack/full-gate.log), [exact source/app/test/helper SHA-256 identities](verification/multitrack/identity.json). The suite includes188 new multitrack checks and84 actual view-command checks. The preceding audio/scheduler gate also passed34/34 in307.20s; the subsequent UI-only clip-highlight/context correction was verified natively and in focused checks, then covered by the final unchanged gate. Source and binary hashes were rechecked after all tests and native verification. No new sanitizer claim is made.

```sh
SOUNDPLUS_VENDOR_QUALIFICATION="$PWD/build/multitrack/vendors" \
SOUNDPLUS_LIVE_DEVICE_QUALIFICATION=1 SOUNDPLUS_QUALIFY_AU=1 \
caffeinate -i .tools/cmake/data/bin/ctest --test-dir build --output-on-failure
```

The final minimum screenshots were taken after dragging the native boundary below its limit: **1040×680 outer**, saved JUCE content bounds1040×648. Browser and Inspector remain visible. Expanded automation, transport and keyboard interaction stay usable, with scrolling for additional tracks and Inspector content. Track selection and clip-only selection have distinct visuals; empty/Track selection exposes Track/Master inserts without disabled clip-only groups.

The latest original session is running again against its **in-place `build/gain-hover-native` catalog**, with no older backup copied over it: **Workflow demonstration r3801**, Unsaved changes, Perform/Browser visible, Original context, Blend enabled .608/.760, Macro1 enabled .491, monitor .25, Library scroll1802 and the existing durable r33 output. The before/after document payloads compare equal, all37 protected media hashes match, and the catalog remains1 root /43 assets. [Audit](verification/multitrack/restoration.json), [full AX](verification/multitrack/restored-user.txt), [restored interface](verification/multitrack/restored-user.jpg).

Both finalized WAVs referenced by the existing Resolve QA project remain at their original paths and match the retained archive SHA-256 values. [Path/hash proof](verification/multitrack/resolve-path-preservation.json). This preservation check does not claim a new receiving-app import or native drag qualification. The retained59-artifact audit remains passing; listening, physical surround and the named open qualifications above remain distinct.
