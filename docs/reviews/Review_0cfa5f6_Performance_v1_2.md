# SoundPlus — Performance Mixer v1.2 review and next milestone

**Reviewed commit:** `0cfa5f6cc303af4abf058513430fe74fe1559134` — Fix Performance Mixer browsing and waveform scheduling reliability.  
**Comparison:** `97bf7c42f05fec73192193c0061ba174a7d96ab1`.  
**Scope:** All projects / all scenes / application development and sound design tools.  
**Recommendation:** Make two targeted P2 corrections, then implement **Performance Mixer v1.3 — Recorded Perform Takes v1**.

Latest GitHub HEAD was checked during this review and matched the reviewed commit. The new attachment, repository guidance, changed source, regression tests, committed report, compressed raw records, manifest and screenshots were reviewed. Application source was not changed. This Linux review host cannot execute the repository's macOS Apple Silicon application, so native results below are committed evidence, not a newly executed native qualification.

## Decision

The previous wrong-sound selection and abandoned-waveform-work findings are closed for their stated regressions. The evidence package is substantially stronger: it retains raw measurements, a complete corpus manifest, native screenshots and explicit limits on its claims. No new high-confidence audio-helper foundation blocker emerged from the reviewed changes.

Two narrower source-confirmed edge cases remain. Correct them with focused tests in a small commit, then proceed to the creative feature already outlined in `docs/PERFORMANCE_TAKES_NEXT.md`. Another open-ended reliability milestone would delay useful progress without resolving a different concrete risk.

## Findings

### R3 — P2: restoring selection can re-pin a fresh query before its AI ranking arrives

**Location:** `src/LibraryView.cpp`, especially lines 858–861 and 1284–1295; supporting code in `restoreSelection`, `requestSearch` and `LibraryService::search`.

**User-visible consequence:** Refresh Results can continue reporting updated results without adopting the requested semantic ordering. A new typed query that retains the same selected file in its lexical prefix can be affected too. The previously fixed wrong-audio retargeting remains fixed.

**Trigger:**

1. Search with local AI enabled and select a file that also appears in the fast filename/lexical prefix.
2. A later semantic update is correctly held while that selection is pinned.
3. Click Refresh Results. The service creates a new request and clears its pin, but the view retains `selectedId`.
4. Before delayed semantics complete, the new lexical prefix reaches the view. `restoreSelection` finds that ID, and the timer calls `selectedRowsChanged` after clearing `updating`.
5. That handler pins the new request. Semantic completion then sets `newResultsAvailable` again rather than adopting the refreshed ranking.

This depends on timing and the selected ID surviving in the lexical prefix. It does not affect every query: an absent selected ID, a sufficiently fast semantic result, or the empty initial prefix used by reference search can avoid it.

**Correction:** Separate explicit user selection from programmatic restoration. Restoring an existing ID should update selection/details and preserve independent audition ownership without silently freezing the new lexical prefix. A deliberate new row selection should still pin its request. Explicit Refresh must be able to adopt the new semantic ordering without switching the playing source.

**Missing regression:** Exercise the actual Refresh handler with AI enabled, controlled delayed semantic completion and the selected ID retained in the lexical prefix. Publish lexical results first, then semantics. Assert new ranking adoption, safe selection/audio identity, and subsequent deliberate selection pinning. The current Refresh test runs after an empty query has already cleared selection; its Library fixture disables AI. The soak's pinned semantic updates test holding updates, not this explicit Refresh adoption sequence.

**Evidence:** [restoration and callback](https://github.com/Tekie79/Sound-Plus/blob/0cfa5f6cc303af4abf058513430fe74fe1559134/src/LibraryView.cpp#L1267-L1295), [pinning](https://github.com/Tekie79/Sound-Plus/blob/0cfa5f6cc303af4abf058513430fe74fe1559134/src/LibraryView.cpp#L847-L865), [semantic adoption](https://github.com/Tekie79/Sound-Plus/blob/0cfa5f6cc303af4abf058513430fe74fe1559134/src/LibraryService.cpp#L1290-L1315), [current regression](https://github.com/Tekie79/Sound-Plus/blob/0cfa5f6cc303af4abf058513430fe74fe1559134/tests/ReliabilityTests.cpp#L118-L142).

### R4 — P2: sustained high waveform demand can starve source revalidation

**Location:** `src/WaveformPreview.cpp`, lines 141–175 and 191–200.

**User-visible consequence:** Under sustained demand well above the 48-source retention limit, a retained selected/card preview can keep an old Ready waveform after its source changes or goes offline. The pool also repeatedly reloads still-wanted entries that it has just evicted.

The scheduler collects missing sources and an overdue validation candidate, but it chooses validation only when the missing list is empty. With the existing stress setup of 80 row/layer sources plus selected/card demand, that list never empties. Hot selected/card entries are protected from eviction, so they do not become missing and can indefinitely miss their five-second revalidation opportunity.

**Scope matters:** normal Perform demand is bounded to one selected source, four cards and up to 40 rows: 45 total. Designer clears layer demand in Perform mode. The relevant product pressure is combined Edit row/layer demand, and the existing 80-source test is a scheduler stress setup. This is not evidence that every ordinary Perform card is stale. A new source key from catalog metadata, explicit retry, reduced demand or pool recreation can recover; the defect concerns unchanged retained entries under continued pressure.

**Correction:** Give overdue validation a nonstarvable service budget or deadline. Distinguish completed display demand from peak-cache residency, or otherwise stop an unchanged over-capacity demand set from causing an endless immediate eviction/reload cycle. Keep memory bounds, selected-source priority, generation rejection and eventual background service.

**Missing regression:** Keep the current high-demand test running after every source has completed once. Hold the demand set unchanged, move/change a reserved source, advance the injected clock beyond the validation interval, and assert bounded Offline/Changed detection and recovery after reconnect. Check that unchanged steady-state demand does not keep increasing decode/derivative-read counts without useful progress. Do not claim an unconditional wall-clock bound while the OS can block a read; define the scheduler service bound separately.

**Evidence:** [admission and retention](https://github.com/Tekie79/Sound-Plus/blob/0cfa5f6cc303af4abf058513430fe74fe1559134/src/WaveformPreview.cpp#L139-L202), [test stops after first service](https://github.com/Tekie79/Sound-Plus/blob/0cfa5f6cc303af4abf058513430fe74fe1559134/tests/ReliabilityTests.cpp#L307-L335).

## What the commit successfully fixes

| Area | Review disposition |
| --- | --- |
| Previous R1: selected file shifts during indexing/reordering | Closed. Explicit selection pins the matching request; page loading is separate; sequence adoption is serialized against a pin; ID restoration no longer falls back to an old row number. Selection and audition ownership are separate. |
| Previous R2: abandoned work delays a new waveform; later demands never get scheduled | Closed for those regressions. Abandoned jobs cancel at worker checkpoints; selected work can preempt ordinary lower-priority work; all bounded demand sets are considered; protected background admissions establish eventual service. |
| Helper-health lifecycle | Improved. Bounded event history, retained diagnostic values, distinct failure/supervisor classifications, bounded cleanup and deliberate playback recovery are implemented outside the audio callback. |
| Small source waveforms | Implemented. Library rows, compact Browser rows and Perform source cards use the shared preview system. This request should be retained as an existing capability, not reintroduced as a future milestone. |

The protected background job is an explicit fairness tradeoff: a newly selected source can wait for that job. A blocked OS read is not instantly cancellable. Neither limitation reopens the original abandoned-demand finding.

Sources: [v1.2 implementation and qualification report](https://github.com/Tekie79/Sound-Plus/blob/0cfa5f6cc303af4abf058513430fe74fe1559134/docs/PERFORMANCE_MIXER_V1_2.md), [waveform worker checkpoints](https://github.com/Tekie79/Sound-Plus/blob/0cfa5f6cc303af4abf058513430fe74fe1559134/src/Waveform.cpp#L153-L195), [helper lifecycle](https://github.com/Tekie79/Sound-Plus/blob/0cfa5f6cc303af4abf058513430fe74fe1559134/src/AudioIPC.cpp).

## Independently executed evidence checks

The machine-readable results are in `Review_0cfa5f6_Evidence_Audit.json` beside this report.

| Check executed on this review host | Result and interpretation |
| --- | --- |
| Referenced committed artifact SHA-256 values | All 11 available artifacts matched. Eight Mac-local binaries, documents or output files were unavailable here and were not rehashed. |
| Raw timing records | All 24 checked distributions reproduced using the benchmark's nearest-rank percentile rule: six distributions for each of four runs. |
| Viewport accounting | All 1,397 records accounted for visible/painted rows and Ready versus terminal unavailable states. |
| Recorded audio continuity | Helper PID, process/play/graph generations and adopted device rate/buffer remained stable across each run's start, periodic observations and final record; recorded underruns and helper event lists were zero. |
| Corpus manifest | 10,000 unique paths, 133 distinct whole-file hashes, 1,253,073,116 bytes, five sample rates and mono/stereo/four-channel files. Source WAVs are not in this checkout, so their bytes were not independently rehashed here. |
| Committed CTest log | All 17 pass names match the 17 suites registered in CMake. Logged duration: 59.81 seconds. These tests were not rerun here. |
| Screenshots | Five JPEGs decoded; normal Perform, minimum Perform and pinned Library screenshots were visually inspected. |

### Recomputed p95 timings

| Recorded run | Active duration | Queries / selected previews / viewports | Query to paint | Selected preview data adoption | Whole viewport painted |
| --- | ---: | ---: | ---: | ---: | ---: |
| Cold derivatives, 512 frames | 120 seconds | 80 / 80 / 80 | 13.00 ms | 49.31 ms | 284.56 ms |
| Warm derivatives, 512 frames | 120 seconds | 80 / 80 / 80 | 13.04 ms | 52.62 ms | 267.73 ms |
| Active soak, 512 frames | 1,800 seconds | 1,197 / 1,197 / 1,197 | 13.63 ms | 55.12 ms | 277.45 ms |
| Separate 256-frame run | 60 seconds | 40 / 40 / 40 | 13.26 ms | 55.55 ms | 293.38 ms |

The 30-minute run's edit-to-tagged-callback p95 is 70.09 ms over 44,997 observations. This boundary excludes physical output/DAC latency. Its 652,623 nonzero audio-status observations are repeated status checks, not that many independent audio segments or a listening assessment.

Interpret the corpus as file-count and source-I/O qualification with disclosed synthetic duplication. It does not establish search relevance across 10,000 distinct commercial recordings. Cold derivatives do not imply a cold OS filesystem cache. The separate 256-frame run qualifies 60 seconds, not 30 minutes. The 512-frame configuration was restored in the submitted evidence.

Evidence: [verification record](https://github.com/Tekie79/Sound-Plus/blob/0cfa5f6cc303af4abf058513430fe74fe1559134/docs/verification/performance-v12.json), [CTest log](https://github.com/Tekie79/Sound-Plus/blob/0cfa5f6cc303af4abf058513430fe74fe1559134/docs/verification/performance-v12-ctest.log), [benchmark percentile definition](https://github.com/Tekie79/Sound-Plus/blob/0cfa5f6cc303af4abf058513430fe74fe1559134/src/BenchmarkReliability.cpp#L20-L39).

## UI and workflow assessment

The Edit/Perform split remains useful: Edit handles precise timing, trims and fades; Perform presents the four sound cards, XY blend and four macros. The latest screenshots show a consistent dark theme, restrained accent, compact source metadata and small waveforms. Preserve that structure and the existing Lucide family.

The minimum-size screenshot makes the XY pad noticeably smaller. The takes UI must reflow without squeezing it further or hiding source identity. The unused area beneath the macro controls is a reasonable place to start evaluating a compact take strip/list. Avoid adding a full automation editor to this slice.

Many submitted preview fixtures look nearly uniform. That alone is not a rendering defect, but these pictures are weak evidence for waveform readability across real sound shapes. In the next native screenshots, include a transient/impact, speech or stepped envelope, quiet tail and long ambience. Show actual waveform data; never substitute decorative waveforms.

## Qualifications to carry forward

- The historical intermittent helper restart remains unexplained. New diagnostics and successful controlled fault tests improve the next investigation; they do not identify its original cause. Preserve the first captured lifecycle event if it recurs.
- The restart-notice test currently checks immediately after the event appears. When touching this path, wait for a different helper PID and a subsequent healthy idle heartbeat, then assert that the notice persists until deliberate playback. This is a test-strength improvement, not a confirmed notice bug.
- `helperRestarts` currently reflects bounded history length and includes explicit reconnect events; do not treat it as a lifetime count of unexpected crashes.
- The submitted working-design preservation and durable-output hashes were reviewed as evidence, not rechecked against the user's Mac. Preserve the latest on-disk session at implementation time; the submitted report names Rain glass r9054, but never restore that revision over a newer one.
- Physical Escape behavior, human listening and additional display conditions remain unverified where the report says so.
- Resolve delivery remains blocked at actual receiver drop. No accepted import, placement or save/exit/reopen sequence is established. Do not infer that Resolve rejected the WAV, and do not label drag initiation as successful delivery.

## Next milestone: Performance Mixer v1.3 — Recorded Perform Takes v1

Deliver one complete workflow: **Record → move XY/macros → Stop → Audition → Keep/name → Render Take → drag the durable WAV or print it back.**

Keep three objects distinct:

| Object | Meaning |
| --- | --- |
| Snapshot | A static editable sound recipe. |
| Take | An immutable starting recipe plus an ordered timeline of controls actually applied by the engine. |
| Rendered WAV | A durable audio file generated from a specific take, with a receipt tying it to that take and source provenance. |

The critical implementation requirement is the applied timeline. Logging mouse motion or IPC receipt timestamps is insufficient: commands coalesce, the producer runs ahead, generations may be replaced, and stale preparation can be discarded. Record control application at the audio-producing boundary, preserve frame order and smoothing semantics, and commit only the valid captured timeline. `lastCommandMs` and `lastCommandSequence` in the new health telemetry are receipt/bookkeeping fields, not sample-accurate automation adoption fields. [Command processing source](https://github.com/Tekie79/Sound-Plus/blob/0cfa5f6cc303af4abf058513430fe74fe1559134/src/AudioEngine.cpp#L178-L186).

Start recording from a clear design-frame-zero origin for this first slice. Freeze the source assignments, mappings, processing recipe and initial controls. Capture the existing XY pair, four macros and enable/bypass/reset effects. Replay and render through the same resolver and stock processing path. Completed takes must survive save/reopen and later changes to the live design.

Use the prepared [takes brief](https://github.com/Tekie79/Sound-Plus/blob/0cfa5f6cc303af4abf058513430fe74fe1559134/docs/PERFORMANCE_TAKES_NEXT.md) and the companion `Codex_Performance_v1_3_Recorded_Takes.md`. First land R3/R4 with focused regression tests, then implement the recording workflow. Mouse/keyboard stereo performance is the scope; later slices can add sampler triggers, MIDI, hosted plug-ins, surround and picture spotting.

**Acceptance:** a captured performance produces a replay and durable render consistent with its actual applied event timeline; editing the current design does not change that take; stop/cancel, overflow, helper loss and source failure produce explicit states; existing search, source previews, editing and durable outputs remain intact. Native and listening evidence must remain separate from numerical checks.
