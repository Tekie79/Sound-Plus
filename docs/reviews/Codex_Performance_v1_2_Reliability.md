# Codex prompt — Performance Mixer v1.2: reliable browsing and preview

Implement a focused reliability milestone for SoundPlus after Performance Mixer v1.1.

## Baseline, scope and preservation

The reviewed baseline is `97bf7c42f05fec73192193c0061ba174a7d96ab1`. Inspect current HEAD and working changes first. Preserve newer work and verify whether a finding has already been fixed. Do not reset to the review baseline.

Read the current `AGENTS.md`, `docs/PERFORMANCE_MIXER_V1_1.md`, `docs/PERFORMANCE_ARCHITECTURE.md`, the relevant verification record, and `docs/reviews/Review_97bf7c4_Performance_v1_1.md` if present. This prompt is self-contained if the review file has not been added.

Maintain the required live monochrome checklist in chat and `docs/`. Use an isolated QA catalog/design and generated fixtures. Preserve the latest working design, originals, existing rendered WAVs and unrelated edits. Close processes using an app bundle before rebuilding it.

The previous sparse-blend and slider-Escape source findings are corrected. Preserve the new law, per-mix legacy behavior, explicit undoable upgrade, manifest gates and cancellation latch. Preserve waveform source cards, visible Library/Browser previews, Lucide/theme, fixed 10-point corner fade handles, shared transport, Edit gestures and current render behavior.

The outcome is stable sound selection, responsive foreground waveforms, actionable audio-helper diagnostics, and realistic qualification. Keep the current feature architecture. Recorded takes, sampler pools, MIDI, plugin hosting, surround and AI picture spotting remain subsequent milestones.

## R1 — preserve selected Library asset during background changes

At the reviewed baseline, `LibraryView::requestPageForRow()` returns immediately for an already loaded `rowAsset(row)` (`LibraryView.cpp:711–712`). However, `selectedRowsChanged()` still calls that method to pin result membership (`848–850`), and `LibraryService::page()` is the view-invoked operation that sets `pinned=true` (`LibraryService.cpp:1428–1435`).

Reproduction to implement:

1. Publish the initial 20-row prefix of a query with more than 20 matches.
2. Select a loaded, visible result such as row 5 without scrolling or fetching another page.
3. Start its audition, then index enough earlier-sorting matches or apply a controlled semantic update to move the chosen ID out of the refreshed first 20.
4. The baseline may retain the previous row index and adopt a different asset, including selection-follow playback of that asset.

Separate explicit selection pinning from missing-page fetching. Selecting a valid row in the current published query must pin membership even when the row is loaded. Keep the guard preventing old displayed rows from pinning a newer pending query. Prefer a request-aware operation that cannot pin a different query through a race.

Retain page-serial-aware deduplication for actual missing-page requests. Preserve the shortened-prefix re-fetch fix. Do not solve pinning by repeatedly issuing unnecessary 512-row fetches on every selection refresh.

Restore selection by stable asset ID. Absence from the current loaded page must not silently retarget another asset at the same row index. Make a selected asset disappearing from the query or catalog an explicit, safe state. Keep existing stopped/playing/pending-playback ownership rules.

Add failing-before/passing-after tests through the actual view and service boundary. Verify current-query pin state, selected ID, range, audition identity and expected playback generation. Exercise indexing and semantic refresh; stopped, playing and pending playback; new queries with old rows still shown; explicit Refresh Results; paging/scrolling; selection-follow audition; and the corrected shorter-page invalidation scenario.

## R2 — supersede obsolete waveform decoding

`WaveformPool::demand()` replaces demands without cancelling in-flight work, while `tick()` submits a new source only when `pending` is empty (`WaveformPreview.cpp:74–145`). Selected audition now uses this shared pool. A long uncached row can keep the selected short file waiting after the old row is no longer demanded.

Reproduction to implement:

1. Start a controlled long-file background decode.
2. Remove that file from every surface demand.
3. Select a different uncached short file.
4. Verify the old job is superseded at a cooperative boundary and the selected waveform advances without waiting for the abandoned full decode.

Use the existing worker generation/cancellation checks before decoding and between blocks. Cancel or supersede work that is no longer demanded. Define bounded priority handling for a newly selected source behind lower-priority work, with enough fairness that useful background requests eventually finish. Do not create a worker per row or an unbounded queue. A blocked OS read has separate limits; keep UI responsive and report those limits honestly.

Reject obsolete completions and never reuse an image under a new binding. Avoid repeated cancellation churn for the same wanted asset. Preserve immutable shared data, derivative reuse, source-stamp checks, offline/reconnect behavior, and truthful all-channel/range/reverse semantics.

Add deterministic worker-boundary tests for abandoned work, selected-source priority, rapid reselection, still-demanded work, stale completions, eventual background completion and unchanged playback ownership. Prove the intended scheduling change with a failing-before case; a test that simply releases and waits for the old decoder is insufficient.

Also test simultaneous visible demands above the 48-source scheduling boundary. Keep retention bounded without silently starving useful visible sources forever. Implement a fair scheduling or explicit demand-cap policy if the test exposes a supported-layout failure. Do not equate total catalog size with simultaneously visible demand.

## Audio-helper restart — investigate the recorded failure

The v1.1 record contains one intermediate run reporting `Audio process restarted. Select a sound to audition again.` after 425 query acknowledgements. The final five short workloads passed, but the cause remains unexplained. Two other failed driver runs reached normal EOF; distinguish those from the helper restart.

Preserve raw failed evidence. The current `AudioIPC.cpp` reason strings are instrumentation, not a demonstrated fix. Investigate using the existing reason logging and add bounded diagnostics where necessary:

- Helper PID and process/playback/graph generations.
- Monotonic last-heartbeat and command/status timestamps.
- First health-failure reason, command/status sequence and pending state.
- Actual process exit/signal and whether the supervisor terminated it.
- Relevant bounded stderr/device/OS diagnostic context and memory/resource observations.

Distinguish a helper crash, watchdog timeout, IPC failure, explicit reconnect, ordinary EOF and benchmark-driver stall. Do not assign a cause to AI inference, waveform work or DSP without evidence. Do not mask the problem by merely increasing timeouts or repeatedly restarting the helper.

Reproduce with controlled concurrent playback, indexing, searching, scrolling, cold waveform loads and control movement. Use isolated fault injection to verify the observed failure paths and recovery reporting; intentional failure tests must be reported separately from ordinary workload results. Protect document state and output receipts across helper failure. Keep blocking work and logging outside the real-time callback.

This investigation is bounded: execute the deterministic cases, targeted diagnostic runs and the specified soak. If the intermittent cause remains unknown, retain an explicit unresolved issue with evidence and the next diagnostic trigger. Finish the independent code corrections and documentation; do not label the restart fixed or keep running unstructured repeats indefinitely.

## Realistic library and sustained-workload qualification

The previous 73-file, approximately 12-second workloads are development evidence. Qualify the new shared waveform/selection paths against a stated thousands-file corpus.

Use an isolated representative corpus of at least 10,000 genuine readable audio files, or a comparable snapshot of the user's available library without modifying its originals. If the available corpus or disk/time constraints prevent that scale, record the actual size and keep the 10,000-file gate open. Existing metadata-only large-search fixtures can remain separate tests; do not claim real waveform qualification from nonexistent source paths.

Include short effects, long recordings, mono/stereo/multichannel material, mixed sample rates, silence, sparse transients, long/duplicate names, multiple regions of one source, and offline/reconnect fixtures. Keep generated storage bounded and report the fixture recipe and size.

Measure cold and warm derivative conditions separately. State OS file-cache limits. Include text and semantic queries with reported model coverage, background indexing, row scrolling, actual loaded-row selection, waveform completion and active Designer control changes. Measure selected-waveform latency separately from background row work and query-to-paint. Do not use one painted row as evidence that the entire viewport is ready.

Record p50/p95/p99 for relevant latency, process memory and cache payload separately, decode/derivative-hit counts, cancellation/preemption counts, helper/graph/playback generations, underruns and errors. Compare only equivalent workloads, and identify any changed protocol.

Run a 30-minute active mixed-workload soak at the currently supported 48 kHz/512-frame configuration. Use an appropriate long-lived fixture or a clearly specified workload whose normal end is outside the interval. Avoid mistaking normal EOF for a crash or letting most of the soak become silent idle time. Keep expected transitions separate from unexpected restarts. If available, run 256-frame operation as a separate qualification with its own result; a failure there must not be concealed by the 512-frame result.

A successful ordinary soak requires no unexpected helper restart, selection retarget, unexplained audition takeover, stuck visible waveform, source mutation or output-receipt corruption. Record the actual underrun result and any limits; do not convert an incomplete run into a pass.

## Functional/native regression and human qualifications

Build on the supported Mac and run the relevant current CTest suites after the fixes. Include existing performance-law/version gates, all six slider-cancellation callbacks, source-card/range/reverse behavior, offline/relink, internal drops, selection-follow audition, snapshots/reopen, full/primary-layer rendering and print-back. Broaden testing only to cover these touched shared paths and the stated milestone gates.

Check the normal and supported minimum layouts with actual source cards and both list views. Preserve keyboard access, focus, explicit bypass, long-name handling and detailed Edit move/trim/slip/fade/scrub operations. Keep this a focused correction pass rather than another general visual redesign.

The old toolbar disappearance was not reproduced in v1.1; its physical-display/capture cause is unresolved. If it reproduces, capture a concrete sequence and fix the demonstrated cause. Do not add a repaint workaround solely to claim closure.

Complete code and automated/native work before requesting optional human checks. Keep physical held-pointer/Escape, listening and physical-display observation distinct from callback/PCM/screenshot tests. If no human result is received, state that exact limitation.

Keep Resolve receiving-app delivery as a separate qualification: actual original/full/selected file receipt, audio-track placement, save, SoundPlus exit, receiving-project close/reopen and online media. Reuse prepared files and fixtures if available. Do not infer acceptance from internal drops, Finder-only delivery or drag initiation. If the receiver is unavailable, keep the gate open without blocking unrelated corrections.

## Completion and next creative milestone

Deliver:

1. Corrected source with deterministic regression tests for R1 and R2.
2. Audio-helper diagnostic results and an honest fixed/unresolved disposition.
3. Realistic library and sustained-workload evidence with commands, corpus/configuration, raw logs, summaries and qualifications.
4. Updated milestone report, live checklist and reproducible verification record.
5. Native screenshots only where they show a relevant changed behavior or state.
6. Confirmation that the latest working design and existing durable outputs were preserved.
7. A concise actual-result summary and a coherent local commit, following repository instructions. Preserve unrelated changes and report remote publication only if it was authorized and actually performed.

After this bounded reliability milestone, prepare the next creative slice as recorded Perform takes: capture existing XY/macros on the shared playback timeline, retain an immutable starting recipe and applied control events, replay/render the movement, keep named takes and verify save/reopen. Keep takes distinct from static snapshots and delivered WAVs. Do not implement that feature inside this reliability task.
