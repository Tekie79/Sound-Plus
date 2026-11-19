# Codex implementation prompt — v1.3.1 Take Integrity and Output Correctness

Implement this bounded corrective milestone in `Tekie79/Sound-Plus`. Complete the fixes, meaningful regressions, native verification where available, documentation and local commits. Preserve the current interface and recording architecture.

## 1. Read the current state and protect the user

Read `AGENTS.md`, `docs/reviews/Review_3eae9b5_Perform_Takes.md`, `docs/PERFORMANCE_MIXER_V1_3.md`, `docs/PERFORMANCE_TAKES_CONTRACT.md`, and the relevant implementation/tests completely.

Reviewed HEAD: `3eae9b569f1edb9bb94b345c9af5e4354ef7d64e`. Prerequisite R3/R4 fixes are in `817c8105bc38a169a2811ecd8a508a84ab6ae830`. If HEAD is newer, inspect the additional changes and adapt. Retain unrelated work and keep the already corrected R3/R4 behavior.

Maintain a concise monochrome Tasks in progress checklist in chat and `docs/`. Protect the latest actual working session, settings, recovery files, source media and durable outputs before rebuilding. The previous report's Rain glass r9299 is historical; do not replace newer edits with it. Close processes running from the app bundle before rebuilding. Use separate QA catalogs/documents and generated fixtures.

Retain JUCE/C++, shared session/audio/resolver/DSP, Lucide/theme, small source waveforms, Edit/Perform, direct timeline gestures, fixed 10-point corner fades and selection-follow audition. Keep allocation, parsing, graph preparation, serialization, file/database I/O and process operations outside the real-time callback.

Keep the current 120-second, 8192-event and 16-take limits and manifest-4 compatibility unless a necessary correction requires a documented migration. This milestone fixes R5–R9; it does not add Source Pools, MIDI, plug-in hosting, surround or a new UI framework.

## 2. R5 — isolate immutable take audition from Live Design updates

At the reviewed baseline, Keep/name and other allowed metadata edits use ordinary `edit()`/`changed()`, which schedules a live preview update. The timer sends `services.parameters(document, ...)`; `designStructure` and `designParameters` use Live Design `activeMix()`. The service treats take replay as ordinary non-capture playback and the reader applies those parameters.

The result can change the sound of an immutable take during Audition → Keep, or fail playback when the live layer structure differs. A render-settings change or asynchronous output-receipt completion can do the same. The pending-preview revision catch-up at `DesignSession.cpp:894–899` is another entry point.

Make metadata versus audio-affecting changes explicit. Keep/name, receipt adoption and output settings must not alter, restart or replace active take audition. Add an explicit playback-context identity through the session/service/reader boundary so live design parameters cannot enter immutable replay. Do not rely only on disabled UI controls or preventing Rename while playback runs. Preserve valid live editing, capture control updates, Stop and independent monitor gain.

Tests must use a sparse-event take with a sustained changed state. Materially alter Live Design gain/effects, audition the take, then Keep/name, change render settings and complete a held render. Compare pre-monitor PCM against uninterrupted immutable replay using a stated tolerance. Assert stable take/generation identity and no unexpected graph preparation. Include a live layer/rack mismatch, metadata during held preparation, and a stale queued live update. Persisted recipe/event hashes must stay unchanged.

## 3. R6 — resolve Ready output from the selected context everywhere

`restoreDerivedState()` currently selects `outputs.back()` after reopen. Render completion also sets `lastOutput` directly. Both bypass the existing context resolver.

Reproduce: render A, select unrendered B, save/reopen; B can expose A's WAV. Also start A's render, switch to B/Live before completion, and A's WAV becomes ready under the other context.

Use one output-resolution policy after restore, render completion, context changes, Undo/Redo and deletion. Retain every completed receipt in history. Resolve the current context to an existing finalized output with matching take identity and frozen provenance, or no ready output. Preserve existing static-output semantics. Verify the actual file bound to Drag and Print-back; nonempty cached state alone is insufficient.

A completed background render must not silently change context. Its status can identify the completed take while the ready-delivery control continues to reflect the current context. Missing/deleted files must disable or clearly invalidate delivery. Keep permanent WAVs intact when deleting or undoing a take.

Tests need A/B with distinct files, B without a render, Live Design, save/reopen in each context, held asynchronous completion after a switch, Undo/Redo/deletion and an externally missing output. Assert both receipt retention and correct ready-file/Print-back binding.

## 4. R7 — validate the same resolved take sources that produced the render

The take reader resolves the frozen recipe and verified relocation overrides. The final publication sweep in `DesignAudio.cpp:784–788` checks the original document's `activeMix()`, which is Live Design/static variation.

Correct this so preparation, PCM production, receipt provenance and final source checks use the same effective immutable input snapshot. Keep detection of actual take-source changes during rendering and atomic publication. A missing unrelated live layer or an obsolete live path must not block a valid take render.

Test the real failure sequence: record a take, move/remove its original source, relink that take to identical bytes at a new path, and render successfully. Add a missing unrelated Live Design layer and repeat. Then mutate an actual take source during a controlled render and confirm safe failure with no published partial output. Verify source IDs/revisions, relocation provenance, receipt hashes and existing outputs remain correct.

The existing copy-only relink test is not sufficient: remove the original path and exercise final rendering/publication.

## 5. R8 — latch device interruption/error independently of final sample rate

The device callbacks currently retain a rate and changed flag. Stop writes zero, but a same-rate restart can overwrite it before the timer runs. `audioDeviceError()` discards its error and leaves the rate unchanged. The timer therefore misses relevant interruptions when final rate equals playback rate.

Introduce a callback-safe lifecycle/error signal or generation latch with a capture baseline. A later start callback must not erase a relevant earlier failure. Process the transition off the callback, mark the active capture Incomplete, preserve its recoverable prefix and first useful diagnostic, release ownership safely and require deliberate recovery. Do not automatically replay stale capture intent.

Handle expected idle initialization and a new deliberate capture without inheriting a stale failure. Define how Arming, Recording and finalization treat lifecycle events. Retain the existing consumed-frame and durable-checkpoint rules.

Exercise actual handlers/service transitions for Stop→Start at the same rate before one timer iteration, device error at unchanged rate, ordinary rate change, idle initialization, interrupted Arming and a fresh capture after recovery. Assert that affected capture cannot later become Ready through a late checkpoint. Callback work remains bounded and free of allocation/I/O. Physical device unplug/replug is a separate native qualification; report honestly if unavailable.

## 6. R9 — enforce the waveform-summary bound on recovery

The eviction path compacts display summaries to 2048 peak pairs, but validation recovery assigns the worker's full result directly into `displays` at `WaveformPreview.cpp:135`. An Offline/Changed display-only source can recover with up to 65536 pairs and retain that oversized summary.

Centralize bounded summary construction/adoption and use it for every write to `displays`. Keep the 48 full-source, 96 geometry and 160 display-summary limits, including the 2048-pair per-summary / 2.5 MiB peak-payload bound. Preserve conservative all-channel extrema, geometry invalidation, availability state, selected/card promotion and settled-demand behavior.

Add long-source tests where a non-hot source is genuinely display-only under unchanged over-capacity demand. Exercise Offline→Ready and Changed→Ready, then assert the individual and aggregate summary bounds and stable work counts after validation settles. A one-second source or a hot source retained at full resolution does not exercise this defect. Preserve R4's nonstarvable validation and the distinction between scheduler bounds and blocked OS I/O.

## 7. Focused UI behavior and verification

Keep the present four waveform cards, XY pad, macros and compact take toolbar. Retain normal/minimum layout usability and read-only playback feedback. Correct take-card tooltips that advertise an unavailable Edit action; show concise take-context wording. Do not add another toolbar row or redesign icons.

Use two distinctly named generated-fixture takes for native verification:

1. Record/Stop/Keep A; create B; save/reopen.
2. Change Live Design; audition A while renaming/keeping and changing output settings. Verify continuous correct playback separately from visual state.
3. Render A while selecting B/Live. Verify the completion notification, selected context and actual ready WAV.
4. Reopen B without a matching render; no A output may be offered as B's.
5. Render after verified relocation with the original path unavailable.
6. Print-back the matching output, inspect the ordinary layer, Undo, and confirm take/receipt/WAV preservation.
7. Exercise relevant device transitions where possible and inspect long-source waveform recovery and minimum/normal layouts.

Add deterministic regression coverage for each finding before claiming closure. Run the current 18-suite Release gate plus new relevant tests on supported macOS. Preserve original capture/replay/offline comparisons and rate-boundary tests. Use focused sustained concurrent capture/replay checks to resolve risks introduced by the changed paths; do not repeat unrelated benchmarks without a concrete reason.

Report commands, final binary hashes, configuration, test counts, PCM tolerances/differences, take/output IDs, event/frame counts, memory bounds and blocked checks. A test attempt or mock-only assertion is not a native/PCM pass. Keep the seven previously disclosed zero-peak observations accurately qualified; do not infer either audible dropouts or a listening pass.

Human listening, physical held-pointer Escape and additional displays remain separately qualified. The historical helper restart remains cause unresolved unless new evidence identifies it. Resolve delivery still requires actual receiver import/drop, placement and save/exit/reopen persistence; use no substitute as a receiving-app pass. Complete all available engineering work before reporting any remaining manual check.

## 8. Finish and prepare the next creative slice

Update the take contract and a v1.3.1 correction/evidence report with one disposition per R5–R9. Preserve the distinction between fixed code, automated regressions, native UI, PCM comparisons, hardware checks and listening. Reopen the latest user session and audit protected files.

Commit the completed corrections in coherent local commits and report the identifiers. The final response should state what changed, why, actual verification and precise remaining limitations.

After R5–R9 are closed, prepare a concise `docs/SOURCE_POOLS_NEXT.md` brief for the next feature. Proposed first slice: per-layer alternate recordings, explicit selected-source waveform, next/previous and deterministic seeded choice before generation/Record, variation locks, stable source/range/fingerprint provenance and frozen resolved choices in takes. A one-source layer remains the simple default.

Live sample triggering or source changes during capture need their own event types, voice/history semantics and compatibility gate. Document that boundary; do not add unrecorded source switching to the current XY/macro event format. Source Pools implementation belongs to the next milestone after this corrective work is verified.
