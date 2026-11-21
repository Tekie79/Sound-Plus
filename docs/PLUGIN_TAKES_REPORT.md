# Plugin-aware recorded Perform takes v1

Request: attachment 0747e22a-0c25-410e-b5d3-a35f8f64fda0. Starting HEAD **a502a69**, exactly the reviewed commit; ordered implementation **d9f62b2**. No newer commits or unrelated changes were present. Stage A must pass and be committed before Stage B implementation.

## Stage A correction checkpoint

| Finding | Correction and regression |
|---|---|
| Valid v7 pooled stock designs rejected | Historical DSP tag is validated from historical processing semantics, independently of the current writer's format promotion. Canonical insert metadata and forged/incompatible tags cannot masquerade as v7. Genuine fixtures were saved/reloaded/rendered using unmodified core at **5b81430** in an isolated checkout; Track, Master and variation-only stock cases include a selected alternate, routing/fades/colour/locks/bypass and historical frozen takes. Model state, baseline static/take PCM, frozen hashes, recovery loading and save/reopen as v9 are tested. |
| Seeded variations changed after round trip | Draws follow canonical typed references, resolving stock payloads by stable ID. Reordered Gain/Filter/Delay, fixed seed, effect/parameter locks and macro-owned parameters compare by ID. A baseline-generated historical RNG oracle verifies unchanged canonical order retains old behavior. |
| Reorder destroyed keyboard focus | Ordered rows are reused by stable InsertRef, with indices/bounds refreshed in place. The focused row scrolls into its own viewport; refresh never grabs external focus. Actual peer-delivery tests exercise consecutive Down/Down/Up/Return, 16 rows, focused subcontrols and background refresh; production Designer commands exercise stopped/playing and Undo/Redo using peer dispatch rather than direct row calls. Native evidence follows. |

Baseline fixture generator/source/producer hashes and original emitted manifests/PCM are retained in [legacy-v7](../tests/fixtures/legacy-v7/README.md). The current writer was not used to invent these v7 fixtures. Relocation uses refreshed live source stamps and explicit identical-content take overrides; historical recipes/hashes are unchanged.

Targeted correction pass: **2/2 /20.80 s** after related source-pool/take/performance/view suites passed. The first new Return assertion assumed the controls were still collapsed on its second invocation. Test setup now closes them before the no-extra-click keyboard sequence and still requires Return to open the correct slot. Raw failed evidence is retained. Full gate and native results are recorded below once executed.

## Preserved boundaries and open requirements

The user's current **Workflow demonstration r3801**, unsaved Perform state (Blend .608/.760, Macro 1 .491, monitor .25), catalog, sources and durable outputs were protected before normal Quit. This is a new protection snapshot, not the older milestone backup. The pre-quit UI also reported an already-restarted audio helper; this observation is not attributed to the new implementation. The original catalog remains in place throughout isolated QA.

OldSkoolVerb native custom input remains unconfirmed until an accepted native gesture changes state/PCM. VST2 dependency/adapter and AUv3 runtime qualification remain open. Expanded Perform groups/additional pads and Resolve product integration remain separate milestones. Human listening, physical surround, numerical PCM, native execution and receiving-project persistence are reported separately.

Stage A required full gate: **32/32 /268.55 s**, Release arm64 / ASan OFF, actual vendor/AUv2/device branches enabled. No processing/UI source changed afterward. Native Track and Master stopped/playing **Down, Down, Up, Return** keep the same slot with no intermediate clicks. Native Undo/Redo retains focus; playing Track Return opens its same hosted editor. [Gate](verification/plugin-takes/stage-a/full-gate.log), [source/helper identity](verification/plugin-takes/stage-a/identity.json), [stopped focus](verification/plugin-takes/stage-a/track-stopped-focus.png), [playing Master](verification/plugin-takes/stage-a/master-playing-focus.png), [same-slot editor](verification/plugin-takes/stage-a/track-playing-editor.png).

Full sixteen-row reachability, viewport scrolling, focused subcontrols and no background focus stealing are actual peer-delivery automated checks. Native checks use three-slot owner chains. Edge-drag auto-scroll and insertion-line feedback are **not implemented**; drag reorder within the visible list and keyboard navigation through all rows remain usable. No broader redesign is included.

## Stage B implementation and closure

Final processing/UI implementation **938a219943fd4f81eb0ec552918b9d52546d9242** (after implementation checkpoint cc9d4ba and native-signature correction eb158ac). Release arm64, macOS 26.3.2 / Apple M5 Pro / JUCE 8.0.6, ASan OFF. The isolated QA app and all five executable/helper identities match the root build: [identity](verification/plugin-takes/stage-b/identity.json). No processing/UI source changes follow this revision; subsequent commits contain evidence/documentation/audit tooling.

| Requirement | Status / implementation / executed evidence |
|---|---|
| Versioned immutable recipe | Fixed: document **10 / plugin-takes-v1**, recipe **3**, processing **plugin-ordered-takes-v1**; hashed canonical typed order, all bypass flags, plugin descriptors/opaque state/stable parameters, sources/SHA, pool provenance, named bus, initial controls and history/tail policy. Lower-version relabels, unknown policies and checksum/provenance conflicts reject. Historical stock fixture hashes/PCM remain unchanged. [PerformTake](../src/PerformTake.cpp), [model gates](../src/DesignModel.cpp). |
| Coherent native barrier | Fixed: reserve and Arming precede capture; document/variation/operation and source/recipe/plugin-identity binding guard publication. New native acknowledgments enter the recipe before freeze. Controlled tests cover success exactly once, cancellation/stale callback, changed region and failed capture. Existing equal-value newer-intent tests remain in the final gate. [session](../src/DesignTakeSession.cpp), [live commands](../src/DesignSession.cpp). |
| Independent fixed-state capture/replay | Fixed: frozen processors cannot borrow Live endpoints, open native editors or accept hosted-control mutations. Service rejects fixed/live combinations and fixed control writes. Capture accepts existing typed Perform state; immutable replay accepts only its own timeline. Native editors are hidden and cannot reopen in capture/take context. [rack/client](../src/HostedRack.cpp), [service](../src/HostedRackService.h), [ordered graph](../src/OrderedRack.inc). |
| Applied-event clock / PDC | Fixed: version-three replay translates the post-priming source event clock to the same internal position using the frozen pipeline latency. A sustained real-device PCM test exposed the missing offset; the tolerance remained **1e-6**. Historical stock interpretation remains unchanged. [graph](../src/DesignAudio.cpp), [regressions](../tests/PluginTakeTests.inc). |
| Real native control delivery | Fixed: Record carries the session's retained structural signature while constructing independent processors. It does not execute retained-borrower retirement, which briefly acknowledged an empty owner ID and falsely settled session preparation. New actual-helper tests assert the intermediate owner/generation status and require nonempty captured events. Native final checkpoints contain the accepted control changes. [client](../src/AudioIPC.cpp), [producer](../src/AudioPlaybackService.cpp). |
| Eight layouts / order / PCM | Passed owned-fixture tests: Mono, Stereo, LCR, Quad, 5.0, 5.1 Side, 5.1 Rear, 7.1. Track stock→hosted→stock and Master hosted→stock→hosted use biases/gains that do not commute, reported latencies 7/11/5 frames, distinct role impulses and no monitor fold-down in export. Capture/replay/final WAV maximum error is below **1e-6**; split roles and actual normative WAV masks are checked. Side and Rear remain distinct. |
| Body / history / tails | Passed independent impulse oracle: Track creative delay17 + Master creative delay19, separate reported latency, consumed body2049, 192-frame tail, exactly2241 output frames; delayed end impulse appears at2084. Fresh frame-zero processing and a body-input gate preserve effect history and render the requested tail once. |
| Ready / Incomplete integrity | Fixed and tested: finalization verifies source content and plugin fingerprints off the callback and persists verification failure as recoverable Incomplete. Actual child crash, changed source during actual capture, missing plugin verification, failed capture, cancellation and context change cannot certify Ready. Existing overflow, durable-checkpoint, stale/device-first-fault and recovery regressions remain in the full gate. Processor failures now retain their diagnostic in the checkpoint. No plugin is silently omitted. |
| Output provenance / readiness | Fixed: immutable receipt includes take/recipe/event/source/plugin-owner/order/state/layout/rate/body/tail, authoritative raw float pass hash and finalized WAV hash. Normalization/splits derive from one raw pass. Context resolution checks frozen provenance and existing finalized paths. Save/reopen receipts and actual ordinary referenced-layer print-back pass. [render](../src/DesignAudio.cpp), [resolver](../src/DesignTakeSession.cpp). |
| Modern UI | Existing Lucide/theme, owner panels and source-before-effects waveform cards retained. Native Arming/Recording/Finalizing/Ready and read-only Take identity observed. Native minimum outer window **1040×680** (saved JUCE content bounds1040×648) inspected in Edit with Browser+Inspector and in Perform. Controls/cards remain usable; names elide deliberately, and Inspector scrolls. Actual focus-dispatch/keyboard tests remain in the full gate. This pass's native Perform motion uses accessible value controls; failed pointer/key attempts are not claimed as successful keyboard motion. |

### Native plugin workflow

Final isolated documents: `build/plugin-takes/native-b/final-workloads/{LV50A,AUDelay} take QA.soundplus`; isolated catalog `build/plugin-takes/native-b/final-data`. Original user data is never used for QA.

| Plugin / bus | Actual native result |
|---|---|
| LV50A **1.0.0**, VST3, Stereo48k | Passed Record→accepted macro/Blend values→Stop→durable Ready→audition→Keep/name→Save→Quit→fresh-process reopen→Render Take→referenced print-back. **2 events**, body679424 frames. Then Live native Trim changed **−6→−12 dB**, with a visible accepted slider gesture and captured normalized Trim .375→.25. The kept recipe stayed .375. Two independent finalized renders, before/after the Live edit, are byte-identical: SHA256 `20f7d6a137239a68893e6fea70af9b0fc8ee1bfb331f5ee1e5d275ed9fc6c314`, Stereo48k float32, **703424 frames** including .5s tail. |
| Apple AUDelay **1.6.0**, AUv2, Stereo48k | Passed the same Record/accepted controls/Stop/audition/Keep/name/Save/quit/reopen/render/print-back workflow. **3 events**, body659456 frames, final **683456 frames** including .5s tail; SHA256 `5c88b49f31f85326d1b1d82f0fc209a1dadcac6cd69dbb42d8283480920db518`. This native document uses Dry/Wet0; it qualifies state/ownership/control/workflow, not audible AU delay-tail matching. Wet delayed-tail correctness is exercised by the owned delayed Track+Master oracle; no universal vendor repeatability claim follows. |

[LV recording](verification/plugin-takes/native/lv-recording.png), [fresh recall](verification/plugin-takes/native/lv-fresh-recall.png), [accepted Live Trim](verification/plugin-takes/native/lv-live-trim-minus12.png), [AU recording](verification/plugin-takes/native/au-recording.png), [AU fresh recall](verification/plugin-takes/native/au-fresh-recall.png), [minimum Edit](verification/plugin-takes/native/minimum-edit.png), [minimum Perform](verification/plugin-takes/native/minimum-perform.png).

Two preliminary native attempts were **not passes**: the first visibly changed controls but saved zero events (structural-signature mismatch); the second exposed premature empty-owner generation acknowledgment. Both were investigated and corrected, with actual-helper regressions, before the final successful native run. Preliminary QA documents/data remain in `build/plugin-takes/native-b/data`; final evidence uses only `final-data/final-workloads`. An earlier new source-diagnostic assertion raced the Incomplete checkpoint write; it now waits for the persisted reason. A sparse impulse comparison alone missed event/PDC timing; sustained native pre-monitor PCM caught it. Test tolerances/watchdogs were not relaxed.

### Receiving application

**DaVinci Resolve Studio21.1.2.10**, throwaway project **SoundPlus Frozen Plugin Take QA 20261121**, timeline **Frozen Take Delivery**. Both finalized WAVs existed and were hashed before import. Resolve API import created actual media items; each was placed on its own Stereo audio track. Save succeeded; SoundPlus quit; the project closed/reopened; both timeline items remained **Online**, 48k/2ch/32bit, referring to the same finalized paths. Resolve references these source files; no copy was observed. Exact sample durations are in the WAV audit; Resolve's 24fps duration display rounds to351/341 timeline frames. [Receiver evidence](verification/plugin-takes/stage-b/resolve-delivery.json).

| Delivery | Status |
|---|---|
| Final VST3-take WAV import/place/save/quit-SoundPlus/reopen | Passed via Resolve connector, original source reference retained. |
| Final AU-take WAV import/place/save/quit-SoundPlus/reopen | Passed via Resolve connector, original source reference retained. |
| Native cross-application file drag | **Unverified in this run**. API import/placement is not native drag acceptance. No drag-start callback or Finder-only result is substituted. |
| Human listening / physical surround | **Not performed**. Native audition transport/meters, numerical PCM and file persistence are separate evidence. |

Finalized source paths:
- `build/plugin-takes/native-b/final-workloads/Finalized/VST3 motion qualified-take-r8-6ee69dd5.wav`
- `build/plugin-takes/native-b/final-workloads/Finalized/AU motion qualified-take-r5-ba08e3c0.wav`

Do not delete these receiving-project references. A durable project media copy can be made deliberately later; no source files were moved/overwritten by this workflow.

### Reproducible verification and remaining boundaries

The required full gate is run with actual vendor/AUv2/device branches enabled on unchanged implementation938a219:

```sh
SOUNDPLUS_VENDOR_QUALIFICATION="$PWD/build/plugin-takes/stage-b-vendors" \
SOUNDPLUS_LIVE_DEVICE_QUALIFICATION=1 SOUNDPLUS_QUALIFY_AU=1 \
caffeinate -i .tools/cmake/data/bin/ctest --test-dir build --output-on-failure
python3 scripts/audit_plugin_takes.py docs/verification/plugin-takes/stage-b
```

The retained [artifact archive](verification/plugin-takes/stage-b/artifacts.zip), [records](verification/plugin-takes/stage-b/artifact-records.json), [native summary](verification/plugin-takes/stage-b/native-summary.json) and [audit](verification/plugin-takes/stage-b/audit.json) support independent file/PCM/metadata checks. The audit does not create new native/listening/drag evidence. Final gate result and latest user-state restoration are appended below after completion.

Open boundaries remain: OldSkoolVerb2.14 custom input; VST2 dependency/adapter/qualification; AUv3 runtime qualification; Expanded Perform beyond four simultaneous assignments; physical surround; human listening; native receiver drag; Resolve product integration. Source-pool alternatives do not add simultaneous assignments. OldSkoolVerb's retained custom Dry drag and Dry Mute attempts did not change captured parameters, state hash or selected PCM; no accepted input is claimed. One precise remaining physical check: in its Live Stereo editor, play the steady generated source, drag **Dry** by a visibly confirmed6dB, then Save/render and require the displayed value, captured state/parameter and PCM level to change together. Record vendor/version and GUI scale. No speculative pointer/scale hack is added to hosting.

Final required gate: **33/33 passed /294.18s** with vendor/AUv2/device branches enabled; [full log](verification/plugin-takes/stage-b/full-gate.log). Focused final plugin-take checkpoint: **1/1 /27.88s** before the final full gate (the full gate reran it successfully in27.06s). Retained artifact audit passes on59 files, including explicit multichannel role masks. No new ASan run is claimed. Actual receiver Play/Stop controls were actuated after reopen and both waveform clips remained present; [receiving timeline](verification/plugin-takes/native/resolve-reopened-playing.png). This is transport/UI evidence, not human listening.

Latest user state restored by launching the root app against its original `build/gain-hover-native` catalog, **without copying an older backup**: **Workflow demonstration r3801**, Unsaved changes, Perform, Blend enabled .608/.760, Macro1 enabled .491, monitor .25, Browser visible, Original context and the existing durable r33 output. Native AX confirms the exact values and all four source cards. [Restored UI](verification/plugin-takes/native/restored-user.png), [state](verification/plugin-takes/stage-b/restored-user.txt), [pre-reopen protection audit](verification/plugin-takes/stage-b/restoration-before.json). Catalog counts remain1 root /43 assets. All235 non-core-recovery protected files retain their hashes; three current recovery/backup files changed during the initial normal Quit's v8→v9 serialization. Normalizing the legacy insert encoding proves the pre/post-Quit canonical model is identical at revision3801. The newer retained state was kept. Resolve's previous **SoundPlus Native Delivery QA 20261118** project and Fairlight page were also restored after saving the throwaway QA project.

Build after quitting the running bundle, then launch the preserved working catalog:

```sh
.tools/cmake/data/bin/cmake -S . -B build
.tools/cmake/data/bin/cmake --build build --target SoundPlus SoundPlusTests --parallel 8
build/SoundPlus_artefacts/Release/SoundPlus.app/Contents/MacOS/SoundPlus \
  --data-dir "$PWD/build/gain-hover-native"
```

For a five-minute owned-library smoke check, use a separate saved design: add two recordings, choose Perform, assign two blend corners, add a qualified Track/Master effect, Record, move Blend and an existing macro, Stop, audition, Keep/name, Save, quit and reopen. Confirm Ready/read-only identity and the named take, Render Take, then Add completed output as layer. Return to Live Design, change its effect state, reselect the kept take and render again; judge plugin-dependent repeatability explicitly. Keep source recordings and finalized WAVs in place. Native-knob motion during recording is deliberately unavailable; it is not an omitted automation stream.

The initial pre-quit screen displayed `Monitor · SOLO` alongside an already-restarted helper notice; the fresh helper displays `Monitor · Stereo downmix`. No specific previous solo-channel identity was available in the persisted preferences or that screen, so this transient status was not reconstructed speculatively. Persisted monitor volume, document controls and panel/window preferences were restored as recorded.
