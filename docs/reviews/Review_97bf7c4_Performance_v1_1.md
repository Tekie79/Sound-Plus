# SoundPlus Performance Mixer v1.1 — review and next milestone

## Scope and verdict

| Field | Scope |
|---|---|
| Episode | All projects; application development |
| Scene | All |
| Department | SoundPlus engineering, sound-design workflow, UI and QA |
| Priority | Two new P2 corrections plus audio reliability and realistic library qualification |
| Next action | Performance Mixer v1.2 — reliable browsing and preview; then recorded Perform takes |

Reviewed commit: [`97bf7c42f05fec73192193c0061ba174a7d96ab1`](https://github.com/Tekie79/Sound-Plus/commit/97bf7c42f05fec73192193c0061ba174a7d96ab1), “Deliver Performance Mixer v1.1 with continuous blending and source previews,” compared with `e6ca7484830d6bfc8d3c95a22b7b8b6e889d1e6f`. GitHub HEAD was checked for this review. This report is pinned to that revision.

**Verdict: the waveform/usability feature is substantially delivered, and the two earlier source findings are corrected. Request a focused reliability follow-up for two new P2 issues.** The source review did not identify a reason to replace the shared audio engine, the new source cards, or the modern icon system. It does not establish production readiness across all libraries or devices.

This was a read-only application-source review in a detached checkout, plus evidence checks and an independently executed numerical reproduction. The host is Linux, while the build explicitly targets macOS Apple Silicon. The Mac application, native CTests, physical pointer interactions and human listening were not executed here. Review documents are the only local additions; no application changes or remote publication were made.

## Closed previous findings and delivered behavior

### Sparse blending — corrected for the new law

The new law uses coordinates on the segment between two occupied corners or barycentric coordinates on the triangle formed by three. Points outside the occupied convex hull project onto it. The nonnegative amplitudes are then power-normalized. Zero/one-slot behavior remains appropriate, and four-slot behavior stays bilinear. See [`PerformanceState.cpp:311–395`](https://github.com/Tekie79/Sound-Plus/blob/97bf7c42f05fec73192193c0061ba174a7d96ab1/src/PerformanceState.cpp#L311-L395).

An independent Python reproduction of the implemented formula checked all 16 occupancy masks, bounds, normalization and neighboring positions. For the user's three-slot arrangement, `(1,.999)` now gives approximately `(0,.707814,.706399)`, the empty corner `(1,1)` gives `(0,.707107,.707107)`, and `(.999,1)` mirrors the first result. Those are amplitude multipliers, not measured output loudness. This was numerical verification, not execution of the native C++ tests.

Compatibility is explicit: legacy performance data retains law 1, while new performance state uses law 2 and manifest 3 / `stock-perform-v2`. Untouched legacy performance state retains manifest 2 / `stock-perform-v1`; neutral old designs remain manifest 1. The update action changes only the active mix through the undoable session path. Consequently, an old design can still exhibit its previous sparse-corner behavior until the user explicitly updates it. That is preserved compatibility, not proof that the new correction failed. Sources: [state serialization](https://github.com/Tekie79/Sound-Plus/blob/97bf7c42f05fec73192193c0061ba174a7d96ab1/src/PerformanceState.cpp#L28-L124), [document gates](https://github.com/Tekie79/Sound-Plus/blob/97bf7c42f05fec73192193c0061ba174a7d96ab1/src/DesignModel.cpp#L115-L141), and [update action](https://github.com/Tekie79/Sound-Plus/blob/97bf7c42f05fec73192193c0061ba174a7d96ab1/src/PerformView.cpp#L124-L133).

### Slider Escape — corrected in the control path

Cancellation now stays latched until release, and rejected post-cancel values restore the displayed controls. A fresh gesture can proceed. The tests cover all six configured slider callback sequences, continued movement, release, fresh drag, undo/redo and document/variation changes. The physical simultaneous held-pointer/Escape check remains separately unverified. Sources: [`PerformView.cpp:374–436`](https://github.com/Tekie79/Sound-Plus/blob/97bf7c42f05fec73192193c0061ba174a7d96ab1/src/PerformView.cpp#L374-L436) and [`PerformanceStateTests.cpp:114–184`](https://github.com/Tekie79/Sound-Plus/blob/97bf7c42f05fec73192193c0061ba174a7d96ab1/tests/PerformanceStateTests.cpp#L114-L184).

### Waveforms and interface — delivered

The inspected implementation and native captures show:

- Genuine 26-point source previews in occupied Perform cards, including source ranges, reverse, duration and state handling.
- Small waveforms in visible full-Library and compact-Browser rows.
- Shared immutable source peaks and bounded source/geometry caches.
- All-channel extrema with every intersecting peak bin included in compact reduction; the display is truthfully described as a source overview.
- Stable source-card identities, alias handling, move/swap assignment and a route to the assigned layer in Edit.
- Visible X/Y labels, compact macro rows, clearer Blend states and contextual advanced mapping.
- Passive source playheads that use the existing timeline/source mapping without starting another transport.

Sources: [SourceCard](https://github.com/Tekie79/Sound-Plus/blob/97bf7c42f05fec73192193c0061ba174a7d96ab1/src/SourceCard.cpp), [compact waveform/pool implementation](https://github.com/Tekie79/Sound-Plus/blob/97bf7c42f05fec73192193c0061ba174a7d96ab1/src/WaveformPreview.cpp), and [native screenshots and report](https://github.com/Tekie79/Sound-Plus/blob/97bf7c42f05fec73192193c0061ba174a7d96ab1/docs/PERFORMANCE_MIXER_V1_1.md).

## New finding R1 — P2: loaded-row selection no longer pins Library results

**Primary location:** [`LibraryView.cpp:703–721`](https://github.com/Tekie79/Sound-Plus/blob/97bf7c42f05fec73192193c0061ba174a7d96ab1/src/LibraryView.cpp#L703-L721), specifically the new early return when `rowAsset(row)` is already available.

The optimization avoids fetching loaded rows, but the same method is still called by selection code to freeze the result sequence. [`selectedRowsChanged():848–850`](https://github.com/Tekie79/Sound-Plus/blob/97bf7c42f05fec73192193c0061ba174a7d96ab1/src/LibraryView.cpp#L848-L850) explicitly documents that intent. The skipped `LibraryService::page()` call is what sets `pinned = true` ([service implementation](https://github.com/Tekie79/Sound-Plus/blob/97bf7c42f05fec73192193c0061ba174a7d96ab1/src/LibraryService.cpp#L1428-L1435)).

### Reproduction scenario derived from source

1. Open a query with more than 20 matches and wait for its initial 20-row prefix.
2. Select an already loaded visible result, such as row 5, and start auditioning it. Do not scroll or trigger a missing-page fetch.
3. Concurrent indexing adds enough earlier-sorting matches to push the chosen asset out of the first 20, or a semantic update moves it out of that prefix.
4. Because selection did not pin membership, the service republishes a reordered 20-row prefix.
5. When the selected ID is absent, the view keeps the previous row index and then resolves that index to another asset. Selection-follow audition can start the other sound.

The fallback and audio continuation are visible at [selection restoration](https://github.com/Tekie79/Sound-Plus/blob/97bf7c42f05fec73192193c0061ba174a7d96ab1/src/LibraryView.cpp#L1253-L1271) and [selection-follow callback](https://github.com/Tekie79/Sound-Plus/blob/97bf7c42f05fec73192193c0061ba174a7d96ab1/src/LibraryView.cpp#L834-L895). The visible-waveform demand loop only reads available rows and does not pin them.

**Impact:** background work can change the chosen recording and possibly its audition without a deliberate user selection. The sequence is source-confirmed; it was not reproduced in a native Mac UI during this review.

**Correction:** separate explicit selection pinning from missing-page fetching. Pin the current published query when the user selects a loaded result. Preserve the newer protections against old-query rows pinning a pending query and preserve page-serial-aware fetch deduplication. Selection restoration must remain ID-based; an absent ID must not silently become another row's asset.

**Regression test:** use the actual view to select a loaded result, verify service pin state, inject indexing/semantic changes, and verify selected ID, range, audition identity and expected playback generation remain stable. Cover stopped, playing and pending-playback states. The current new paging test covers stale queries and shortened pages but not this loaded-row selection path ([test](https://github.com/Tekie79/Sound-Plus/blob/97bf7c42f05fec73192193c0061ba174a7d96ab1/tests/WaveformPreviewTests.cpp#L59-L84)).

## New finding R2 — P2: obsolete background decoding delays the selected waveform

**Primary location:** [`WaveformPreview.cpp:74–145`](https://github.com/Tekie79/Sound-Plus/blob/97bf7c42f05fec73192193c0061ba174a7d96ab1/src/WaveformPreview.cpp#L74-L145).

`demand()` can remove a source from every surface, but it does not cancel its in-flight decode. `tick()` submits another source only after `pending` is empty. There is no pool call to the decoder's cancellation mechanism. Selected-source priority therefore affects the next job, without superseding obsolete current work.

### Reproduction scenario derived from source

1. A visible row starts decoding a long, uncached recording.
2. Scroll it away until no surface demands it.
3. Select a different uncached short recording.
4. Its detailed waveform cannot be requested until the abandoned recording finishes.

**Impact:** waveform availability can lag badly behind selection on long files or slower storage. This finding does not establish an audio playback interruption, UI-thread block, or a measured delay. Previously, selected audition directly requested the cache and advanced its generation; the shared background scheduler changes that behavior.

**Correction:** cooperatively cancel or supersede no-longer-demanded work; consider bounded priority preemption for the selected source while preventing starvation of useful background work. Keep a bounded scheduler. The existing worker already checks its generation before decoding and between blocks ([worker](https://github.com/Tekie79/Sound-Plus/blob/97bf7c42f05fec73192193c0061ba174a7d96ab1/src/Waveform.cpp#L151-L188)). A blocked OS read still has its own interruption limits; do not promise instantaneous cancellation of all storage operations.

**Regression test:** hold a low-priority decode at a controlled cooperative boundary, remove its demand, request a selected short source, and verify obsolete work is cancelled and the selected source advances. Check rapid reselection, stale results, eventual background completion and unchanged playback ownership. The existing late-result test waits for the old job to finish; it verifies binding safety, not priority responsiveness ([test](https://github.com/Tekie79/Sound-Plus/blob/97bf7c42f05fec73192193c0061ba174a7d96ab1/tests/WaveformPreviewTests.cpp#L283-L325)).

## Reliability and evidence qualifications

### What was checked independently

- Exact GitHub/local revision and application diff.
- The corrected blend formula through an independent numerical reproduction.
- All nine committed screenshot SHA-256 values; every hash matched and every image decoded.
- The 14 passing suite names and 50.98-second total in the committed CTest log; the suite set matches CMake.
- Normal/minimum Perform, Library, range/reverse and source-state captures.

### What remains recorded native evidence

The report records 14 CTest suites, 3,634 waveform assertions, 19,930 performance-state assertions, static pre-monitor/offline PCM error 0.0, native source-card workflows, saved reopening and durable render receipts. The assertion counts include fixture writes and are not independent user-scenario counts. Mac-local binaries, raw benchmark files and generated WAVs were unavailable in this checkout; their hashes could not be independently recalculated here.

The five final short runs record 3,000 search acknowledgements, stable helper/graph/playback generations and zero underruns, on a 73-file development catalog. This supports those runs. It does not qualify thousands of source files, a 30-minute session, 256-frame buffers, M2 hardware or other interfaces. A moved-viewport waveform acknowledgement means at least one waveform painted, not that all visible waveforms completed.

### Audio-helper restart — observed, cause unresolved

The evidence record retains one intermediate failure with `Audio process restarted. Select a sound to audition again.` after 425 query acknowledgements. Two other intermediate runs reached normal audio EOF while the driver waited; those should not be conflated with the restart. The added `AudioIPC.cpp` lines record existing failure reasons; they are observability, not a demonstrated restart fix. The five later passing runs do not identify its cause. Do not attribute it to the waveform worker, semantic inference or DSP without further evidence.

### Other open qualifications

- Physical held-pointer/Escape and human listening have no received human result.
- The previous toolbar disappearance was not reproduced in the newer native captures; the capture-versus-display cause remains unresolved.
- Physical SoundPlus-to-Resolve import, placement and save/exit/reopen persistence remain open.
- The scheduler considers at most 48 unique demands in priority order. More simultaneous visible demands can starve later ones. This is a source-visible scale boundary, not a reproduced failure at the supplied normal/minimum layouts; test fairness under cache pressure.

Sources: [verification record](https://github.com/Tekie79/Sound-Plus/blob/97bf7c42f05fec73192193c0061ba174a7d96ab1/docs/verification/performance-v11.json), [CTest log](https://github.com/Tekie79/Sound-Plus/blob/97bf7c42f05fec73192193c0061ba174a7d96ab1/docs/verification/performance-v11-ctest.log), and [IPC instrumentation](https://github.com/Tekie79/Sound-Plus/blob/97bf7c42f05fec73192193c0061ba174a7d96ab1/src/AudioIPC.cpp#L223-L297).

## Recommended next milestone

### Now: Performance Mixer v1.2 — reliable browsing and preview

Keep the current interface and close a bounded reliability slice:

1. Fix R1 loaded-row selection pinning and R2 obsolete waveform work.
2. Add deterministic regressions at the actual view/worker boundaries.
3. Investigate the helper restart with bounded diagnostics, controlled fault injection and a real mixed-workload soak; preserve an unresolved status if no cause is established.
4. Qualify thousands-file behavior with a stated representative corpus, cold/warm derivatives and real source files; test long-source interruption and simultaneous surfaces under cache pressure.
5. Run a 30-minute active playback/browse/index/control workload at the supported 512-frame setting, with a source duration or explicit test design that avoids mistaking normal EOF for a crash. Treat a 256-frame run as a separately reported configuration.
6. Verify selection/audition stability, snapshot/reopen, full/primary renders and working-design preservation after the fixes. Keep native, automated and human results distinct.

The next prompt is `Codex_Performance_v1_2_Reliability.md`. It contains implementation, validation and stop-condition details so this does not expand into an indefinite redesign.

### Then: the next creative feature is recorded Perform takes

The original architecture puts takes, sampler/pools, MIDI and variations in M2, with installed effects in M3. The next useful creative slice after this reliability pass is recording XY/macro movement:

- Record → perform → stop → audition → keep/render.
- Store an immutable starting recipe, blend-law identity and time-stamped applied control events as a take distinct from a static snapshot.
- Replay/render the captured movement through the shared engine, with explicit timeline and effect-history semantics.
- Recall and render a take after save/reopen even if the current editable recipe has changed.
- Begin with existing XY/macros and mouse/keyboard control; sampler triggers, MIDI and hosted effects can follow in separate slices.

This makes the performed movement reusable and deliverable, which is the next clear step beyond the static configurations already implemented. The [architecture and requirements](https://github.com/Tekie79/Sound-Plus/blob/97bf7c42f05fec73192193c0061ba174a7d96ab1/docs/Sound_Design_Workstation_Architecture_and_Requirements.docx) distinguish recipes, performed takes and delivered audio; the next feature should preserve that distinction.
