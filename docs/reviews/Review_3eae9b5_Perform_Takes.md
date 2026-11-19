# SoundPlus v1.3 review — recorded Perform takes

**Reviewed HEAD:** `3eae9b569f1edb9bb94b345c9af5e4354ef7d64e`
**Prerequisite corrections:** `817c8105bc38a169a2811ecd8a508a84ab6ae830`
**Compared with:** `0cfa5f6cc303af4abf058513430fe74fe1559134`
**Episode / scene:** All projects / all scenes
**Department:** SoundPlus application and sound-design workflow
**Priority / next action:** Correct take integrity and output identity in v1.3.1, then advance to Source Pools.

## Verdict

The recording feature is substantially implemented, and the previous R3 semantic-refresh and R4 waveform-validation findings are closed for their original causes. Capture, immutable data, versioned persistence, applied-frame replay and permanent output receipts form a useful foundation. The compact interface and small waveform displays should be retained.

**The immediate next milestone should be v1.3.1 — Take Integrity and Output Correctness.** Five source-confirmed defects remain, including one high-priority path that changes a frozen take's sound during normal audition/Keep operations. Complete this bounded correction before extending performance sources.

This review inspected the complete attachment, current repository instructions, both commits, relevant implementation/tests, raw evidence and screenshots. The repository is macOS Apple Silicon only; this review ran on Linux. Native builds, CTests, hardware interaction and human listening were not newly executed. No application source, user media or remote repository was changed by this review.

## Findings

### R5 — P1: metadata edits can change immutable take audition

**Primary location:** `src/DesignSession.cpp:1038–1047`.

**Reproduction:** Record and keep Take A. In Live Design, change a layer gain substantially while retaining the layer structure. Select and audition A, then Keep/rename it while it plays. The metadata edit can send the current Live Design parameters into A's replay graph. Render-settings edits or adopting a completed render receipt can cause the same side effect. Even without a different live recipe, a sparse-event take can have its current performed state replaced with the starting live values.

**Cause:** `nameTake()` uses ordinary `edit()`. `changed()` schedules a preview update during active playback. Both `designStructure()` and `designParameters()` operate on the editable `activeMix()`, which does not select the frozen take. The audio service distinguishes capture from ordinary playback but does not protect take replay from the ordinary parameter path. The reader and graph apply those parameters. A later recorded event may restore frozen values; otherwise the altered audition can persist. A structural mismatch can instead fail playback.

The saved frozen recipe/event data is not rewritten by this path. The defect is the active replay graph being modified by unrelated live or metadata changes.

**Correction:** Classify metadata-only changes so they do not schedule audio updates, and explicitly isolate immutable take replay at the session, service and reader boundaries. Cover the pending-preview revision catch-up path at `DesignSession.cpp:894–899` as well. Monitor volume remains a separate permitted listening control.

**Required regression:** Compare uninterrupted take replay with replay during Keep/name, render-settings edits and asynchronous receipt completion, while the live recipe differs. Include sparse events, a live layer/rack mismatch and an edit during held preparation. PCM and take identity must remain unchanged; the metadata operation must not silently restart playback.

Sources: [preview dispatch](https://github.com/Tekie79/Sound-Plus/blob/3eae9b569f1edb9bb94b345c9af5e4354ef7d64e/src/DesignSession.cpp#L1038-L1047), [metadata edit](https://github.com/Tekie79/Sound-Plus/blob/3eae9b569f1edb9bb94b345c9af5e4354ef7d64e/src/DesignTakeSession.cpp#L156-L172), [parameter context](https://github.com/Tekie79/Sound-Plus/blob/3eae9b569f1edb9bb94b345c9af5e4354ef7d64e/src/DesignLive.cpp#L48-L75), [audio adoption](https://github.com/Tekie79/Sound-Plus/blob/3eae9b569f1edb9bb94b345c9af5e4354ef7d64e/src/AudioPlaybackService.cpp#L306-L316).

### R6 — P2: Ready output can belong to a different take

**Primary locations:** `src/DesignSession.cpp:298–304` and `1120–1128`.

**Reopen reproduction:** Render A. Select B, which has no output. Save with B selected and reopen. `restoreDerivedState()` restores `outputs.back()` unconditionally, so A's WAV becomes the ready drag/print output while B remains selected.

**Asynchronous reproduction:** Start rendering A, switch to B or Live Design, and let rendering finish. Completion assigns A's path directly to `lastOutput` without resolving the now-current context. The UI enables Drag/Print-back from that cached path.

The filename is visible, but a user can still deliver the wrong performance while the take selector shows a different one. The existing `selectContextOutput()` handles explicit selection correctly; reopen and completion bypass it.

**Correction:** Retain completed receipts in history, then resolve the ready output for the current context through one consistent path. Use it on reopen, render completion, selection, Undo/Redo and deletion. Require an existing finalized file and matching take identity/provenance before enabling delivery. Preserve existing static-output behavior.

**Required regression:** Two distinct takes with separate renders; a selected take with no render; Live Design; save/reopen; held render completion after context changes; Undo/Redo/deletion; missing output files. The old receipt remains available in history while the ready output is the current context's valid WAV or empty. Check the actual Print-back/drag file binding.

Sources: [restore](https://github.com/Tekie79/Sound-Plus/blob/3eae9b569f1edb9bb94b345c9af5e4354ef7d64e/src/DesignSession.cpp#L298-L305), [completion](https://github.com/Tekie79/Sound-Plus/blob/3eae9b569f1edb9bb94b345c9af5e4354ef7d64e/src/DesignSession.cpp#L1120-L1128), [existing context resolver](https://github.com/Tekie79/Sound-Plus/blob/3eae9b569f1edb9bb94b345c9af5e4354ef7d64e/src/DesignTakeSession.cpp#L124-L154), [delivery controls](https://github.com/Tekie79/Sound-Plus/blob/3eae9b569f1edb9bb94b345c9af5e4354ef7d64e/src/DesignerView.cpp#L1205-L1213).

### R7 — P2: final take-render validation checks Live Design sources

**Primary location:** `src/DesignAudio.cpp:784–788`.

The take reader correctly uses the frozen recipe and verified source relocation overrides. The final publication sweep, however, calls `sourceUnchanged()` on `d.activeMix()`: the Live Design or selected static variation.

**Reproduction:** Record a take, move its original source away, and relink the take to an identical file at a different path. The reader can produce valid PCM from the verified new location, but final validation checks the old Live Design path and rejects publication. An unrelated offline layer subsequently added to Live Design can also block a valid take render.

**Correction:** The producer, render metadata and final source guard must use the same resolved immutable input snapshot. Preserve final validation against actual take-source changes; remove the dependency on unrelated live sources.

**Required regression:** Actually remove/move the original, verify-relink the take, and render it. Include an unrelated missing Live Design source and a genuinely changed take source. The first two must not prevent a valid take render; actual take-source mutation must still prevent publication. The current relink test copies the source, leaves the original online and does not render afterward.

Sources: [final validation](https://github.com/Tekie79/Sound-Plus/blob/3eae9b569f1edb9bb94b345c9af5e4354ef7d64e/src/DesignAudio.cpp#L783-L788), [frozen recipe and relocation](https://github.com/Tekie79/Sound-Plus/blob/3eae9b569f1edb9bb94b345c9af5e4354ef7d64e/src/PerformTake.cpp#L271-L297), [current relink test](https://github.com/Tekie79/Sound-Plus/blob/3eae9b569f1edb9bb94b345c9af5e4354ef7d64e/tests/TakeTests.cpp#L603-L618).

### R8 — P2: a same-rate device interruption can escape capture failure handling

**Primary locations:** `src/AudioEngine.cpp:28–40` and `145–149`.

The callbacks retain only the latest device rate and a changed flag. Stop sets the rate to zero; restart overwrites it with the new rate. The timer interrupts capture only when the final rate differs from the playback rate. `audioDeviceError()` discards its error argument and merely sets the same flag.

**Trigger:** A device stops and restarts at 48 kHz before the timer consumes the flag. The timer sees 48 kHz versus 48 kHz and misses the interruption. An error callback without a rate change has the same problem. The capture can continue toward Ready despite the contract requiring a device failure to produce Incomplete.

**Correction:** Latch relevant lifecycle/error transitions independently of the final sample rate. Associate them with capture generation/baseline and handle failure off the callback. A later start callback must not erase an earlier failure. Preserve the first useful diagnostic and require deliberate recovery.

**Required regression:** Stop then same-rate restart before one timer iteration; error at unchanged rate; ordinary rate change; expected idle startup; and a fresh capture after deliberate recovery. Tests must exercise actual handler/service state transitions. Physical device removal remains a separate hardware qualification.

Sources: [device callbacks](https://github.com/Tekie79/Sound-Plus/blob/3eae9b569f1edb9bb94b345c9af5e4354ef7d64e/src/AudioEngine.cpp#L28-L40), [rate-only interruption condition](https://github.com/Tekie79/Sound-Plus/blob/3eae9b569f1edb9bb94b345c9af5e4354ef7d64e/src/AudioEngine.cpp#L134-L149).

### R9 — P2: recovered display summaries can exceed their memory bound

**Primary location:** `src/WaveformPreview.cpp:132–136`.

Evicted visible sources are correctly compacted to at most 2,048 peak pairs. After a display-only source becomes Offline/Changed and then recovers, the worker loads or decodes full peaks. The validation-adoption branch assigns those full peaks directly to `displays`, bypassing compaction. Later Ready validations preserve that oversized data.

**Reproduction:** Maintain over-capacity demand with a long non-hot source. Let it become display-only, move it offline, allow validation, restore the exact file, then validate again. A 20-second 48 kHz source already exceeds 2,048 full bins; long sources can reach 65,536. An entry can retain 512 KiB of peak pairs instead of 16 KiB. Source counts remain bounded, but the promised 160 summaries / 2.5 MiB payload bound is no longer enforced.

**Correction:** Centralize bounded summary construction and use it for every summary adoption, including recovery. Preserve all-channel extrema, availability transitions, geometry invalidation and promotion to full selected/card peaks.

**Required regression:** Long display-only Offline→Ready and Changed→Ready cases under unchanged demand, with per-summary and aggregate payload assertions. Current pressure fixtures are one-second aliases, and the disconnection case targets a hot full-resolution source, so they miss this path.

Sources: [recovery adoption](https://github.com/Tekie79/Sound-Plus/blob/3eae9b569f1edb9bb94b345c9af5e4354ef7d64e/src/WaveformPreview.cpp#L132-L138), [eviction compaction](https://github.com/Tekie79/Sound-Plus/blob/3eae9b569f1edb9bb94b345c9af5e4354ef7d64e/src/WaveformPreview.cpp#L219-L224), [worker recovery/full peaks](https://github.com/Tekie79/Sound-Plus/blob/3eae9b569f1edb9bb94b345c9af5e4354ef7d64e/src/Waveform.cpp#L127-L176).

## What the implementation gets right

- R3 now separates deliberate selection from restoration, with actual AI-enabled Refresh and typed-query tests using delayed semantics and retained selection.
- R4 gives overdue validation service even with missing work, preserves compact display data after eviction and tests settled demand rather than only first completion. R9 is a narrower new adoption-path defect.
- Applied controls are captured at source production after coalescing. Full typed state preserves atomic XY, Blend and four macro values/enables.
- Stop reconciles read-ahead with consumed output frames and a separate interpolation dependency. The rational duration does not grow to include speculative producer work.
- Fixed event storage, immutable recipe/event sharing, worker checkpoints, generation gates and final durable acknowledgement address the requested capture architecture.
- Manifest 4 / `stock-takes-v1`, source fingerprints, identical-content relink and explicit Incomplete recovery are implemented. Source relocation remains separate from frozen recipe hashes.
- The controlled audio tests compare original pre-monitor capture with replay, then offline output, rather than only comparing two replay paths.

Sources: [implementation contract](https://github.com/Tekie79/Sound-Plus/blob/3eae9b569f1edb9bb94b345c9af5e4354ef7d64e/docs/PERFORMANCE_TAKES_CONTRACT.md), [prerequisite corrections](https://github.com/Tekie79/Sound-Plus/blob/3eae9b569f1edb9bb94b345c9af5e4354ef7d64e/docs/PERFORMANCE_V1_3_CORRECTIONS.md), [take tests](https://github.com/Tekie79/Sound-Plus/blob/3eae9b569f1edb9bb94b345c9af5e4354ef7d64e/tests/TakeTests.cpp).

## Independently checked evidence

The executed audit is retained in `Review_3eae9b5_Evidence_Audit.json`.

| Check | Review result |
| --- | --- |
| Available committed artifact SHA-256 values | All 10 matched. Fifteen Mac-local referenced files were unavailable here. |
| Raw versus embedded recording summary | Exact agreement; all three headline percentile distributions recomputed. |
| Viewports | 80 records consistently account for visible/painted rows and terminal unavailable states. |
| Audio status observations | 15 start/progress/stop/final observations have stable helper/process/play/graph/rate/buffer/preparation fields, zero recorded underruns and no recorded helper lifecycle events. |
| Saved take data | Two saved documents, 4,510 events total; contiguous sequence, ordered integer frames, normalized controls, matching generation and events inside the exclusive body boundary. |
| Receipts | Take IDs, recipe/event hashes, consumed/body frames and source fingerprints agree with saved takes; output frame counts agree with exact body duration plus one stated tail. |
| Corpus | Retained manifest reconciles 10,000 unique paths, 133 whole-file hashes and 1,253,073,116 bytes. It remains a synthetic file-count/I/O corpus. |
| CTest log | 18 passing names match current CMake registration; recorded duration 72.55 seconds. Not rerun here. |
| Screenshots | All three decode and were visually inspected. |

### Recorded final-artifact workload

The submitted 48 kHz / 512-frame capture contains **5,760,000 consumed frames = 120.000 seconds**, **4,504 applied events**, and peak checkpoint backlog **30 / 8,192**. The saved native pointer/keyboard take contains six events and 244,736 consumed frames. Its receipt adds 19,200 frames for a 0.4-second tail, producing 263,936 output frames. The sustained take adds 48,000 tail frames, producing 5,808,000 output frames.

| Recomputed timing | p50 | p95 | p99 |
| --- | ---: | ---: | ---: |
| Query to paint | 5.53 ms | 9.83 ms | 15.53 ms |
| Selected waveform data adoption | 29.37 ms | 50.94 ms | 64.70 ms |
| Whole viewport paint | 233.86 ms | 276.23 ms | 332.24 ms |

Recorded process peak RSS: app 194,936,832 bytes; audio helper 77,135,872; AI helper 462,471,168. These are process peaks, not callback-allocation measurements or a whole-system guarantee. The 43,701 nonzero observations out of 43,708 are repeated status checks. Seven zero-peak observations remain undiagnosed; neither audible dropouts nor a listening pass can be inferred from them.

Five controlled rate configurations report capture/replay maximum error 0.0: 44.1→44.1, 48→48, 96→96, 48→44.1 and 44.1→48 kHz. Each rate case consumes 1,536 output frames and records one applied event. These are short boundary/conversion tests; the sustained native hardware case is 48 kHz / 512 only. No new long mixed-rate or 256-frame hardware qualification is implied.

Compressed artifact hashes were recomputed. Canonical JUCE envelope/recipe/event hashes were checked for cross-record consistency, not independently recomputed from reconstructed JSON. Original sources and rendered WAVs were unavailable here, so their reported hashes/PCM comparisons remain committed Mac evidence. Sources: [verification record](https://github.com/Tekie79/Sound-Plus/blob/3eae9b569f1edb9bb94b345c9af5e4354ef7d64e/docs/verification/performance-v13.json), [CTest log](https://github.com/Tekie79/Sound-Plus/blob/3eae9b569f1edb9bb94b345c9af5e4354ef7d64e/docs/verification/performance-v13-ctest.log).

## UI assessment

The small waveforms are present and now demonstrate distinct impact, stepped-texture, quiet-tail and ambience shapes. The four-card / XY / four-macro layout is clearer than the original oversized unannotated pad. Reusing the existing tool row for take controls preserves the minimum layout. Keep the current theme and Lucide system.

Take replay is appropriately read-only in the visible controls, although R5 shows that this policy is not yet enforced throughout the audio command path. Source-card tooltips should describe the read-only context instead of advertising an Edit action that is blocked. This is a small wording correction; a UI redesign is unnecessary.

## Next milestone and subsequent creative feature

### Immediate: v1.3.1 — Take Integrity and Output Correctness

Close R5–R9, add the exact missing interaction regressions, run the existing native regression gates and repeat the relevant user workflow with two distinctly named takes. Preserve the current capture limit, document compatibility, source provenance and presentation. The companion `Codex_Performance_v1_3_1_Take_Integrity.md` contains the implementation instructions.

Completion means: metadata cannot alter/restart immutable audition; delivery always resolves the selected context; verified take relocation renders independently of Live Design; relevant device failures cannot produce Ready; every retained display summary obeys its bound. Retain all completed receipts and existing user WAVs.

### After closure: v1.4 — Source Pools and Repeatable Variations

A useful first Source Pools slice would let a layer hold alternate recordings, show the currently resolved source's small waveform, and choose an alternative before generating or recording a variation. Provide deliberate next/previous and deterministic seeded selection, respect variation locks, and freeze the resolved source identity/range/fingerprint into each take. Keep the standard workflow usable with one source.

This creates practical variations of impacts, movement or ambience using the existing designer. Live sample triggering or source changes during recording require explicit event types, voice/history rules and a compatibility gate; they should not be smuggled into the present normalized-control-only take format. Plug-in hosting remains a separate larger milestone.

## Carried qualifications

The historical helper restart remains cause unresolved. Human listening, physical held-pointer Escape and other displays remain unverified where disclosed. Resolve still lacks an established actual receiver drop, track placement and save/exit/reopen persistence sequence. Preserve these distinctions in the next report.

The submitted protection record identifies Rain glass r9299. Implementation must protect the latest actual working session at that time, not restore this historical revision over newer edits. For series work, use early sound experiments to answer story/timing questions and leave final sound, VFX and color polish until picture lock.
