# Library performance milestone

Implemented and verified on 17 November 2026 against the repository's architecture and requirements specification and the existing Library/audition milestone. This completes the requested library-performance slice. Semantic retrieval, Designer, plugins and Resolve placement remain outside it; this is not the specification's complete M1 release gate.

## Behavior and architecture

All matching recordings are accessible through a virtual table. The first request publishes up to 20 records; scrolling loads one bounded 512-record window. A disk-backed sequence of asset UUIDs gives pages stable ordinals without retaining a long WAL read transaction. Selecting or paging holds that sequence while indexing continues. **Refresh Results** incorporates new matches and retains an eligible selected UUID; background metadata/availability updates retain the viewport. The playing asset is independent of the result list.

The old service already coalesced pending query text, but an expensive active query could delay its replacement. The new reader interrupts obsolete SQLite work, checks request generations before publication, and holds only one pending query and one pending viewport request. Cached root counters replace repeated catalog-wide COUNT/SUM joins. Native updates request a repaint directly; the previous 100 ms result polling interval no longer gates search.

Empty browsing sorts by filename and UUID. Exact filenames and asset UUIDs rank first within the selected source. Other keyword matches use stable catalog insertion order. Removing the global BM25 sort is a deliberate lexical-ranking tradeoff; full relevance/hybrid ranking is not claimed.

Four owned filesystem workers isolate root probing, directory walking and header reads from the serial database writer. The write queue holds at most eight batches of at most 64 records each (512 queued records, plus bounded worker/writer buffers). Metadata backlog is durable SQLite state, not an in-memory list of 100,000 jobs. Changed-directory event sets are bounded at 2,048 and collapse to recovery work on overflow. Source jobs coalesce; no threads are abandoned or created on each retry.

FSEvents starts before enumeration. Ordinary events reconcile affected directories after coalescing and 750 ms file settling. Directory fingerprints and checkpointed child-directory lists avoid file enumeration on an unchanged restart. Availability probes run every two seconds, fingerprint sweeps every 30 seconds, and full file reconciliation every five minutes. Unreliable/nonlocal watchers use a 30-second full fallback. Missed-event flags, remounts, detected wake gaps and **Rescan** request recovery. Interrupted discovery resumes at directory checkpoints and durable pending headers. Schema v1 migrates transactionally to v2 without replacing asset IDs.

**Pause Indexing** drains committed work and parks workers at safe I/O boundaries; search and audition continue. The status shows root, stage, header backlog, worker count and queued directories. A kernel-blocked filesystem call cannot be cancelled: pause acknowledgement and shutdown may wait for it. One blocked root leaves other worker slots available; four blocked calls can exhaust the pool. The controlled delay test does not qualify arbitrary hung SMB/NFS mounts.

## Workload and measurement

Both comparison runs used the same physical MacBook Pro, **Apple M5 Pro, 48 GiB, Mac17,8, macOS 26.3.2 (25D2150)**, internal APFS SSD (`disk3s5`, Apple Fabric, solid-state). Release arm64, Apple Clang 21.0.0, SDK 26.5, JUCE 8.0.6, CMake 3.31.6, macOS 12.0 deployment minimum. macOS 12 itself was not tested. No OS cache purge or thermal/power time series was performed. “Cold” means a fresh process opening an existing catalog; repeated query runs are warm, with uncontrolled OS caches.

The isolated fixture contains exactly **100,000 original files**, 70,428,810 logical bytes / 450,551,808 allocated bytes, **266.637 seconds** of valid audio: 90,000 WAV, 5,000 AIFF, 5,000 FLAC, including 11 malformed files retained as failures. Valid short clips are 128 frames at 48 kHz mono. Two roots each contain 10 banks × 50 buckets × 100 files (1,022 directories including roots); names include Unicode, repeated basenames in different folders and varied tokens. Independent files are written without hardlinks or clones. Construction took **6.266 seconds**, excluded from import timing.

A separate six-file WAV corpus contains **405 seconds / 62,694,264 bytes**, one 180-second and five 45-second clips, mono/stereo at 44.1/48/96 kHz. All six are fully decoded through the audition PCM stream; cached device starts use the 180-second file. The controlled slow-source comparison uses two 0.25-second mono 48 kHz WAV files, **48,208 bytes / 0.5 seconds total**, on the same SSD and machine. These local cached reads do not qualify slow external storage or semantic analysis throughput.

Each 200-query set contains 40 exact names, 40 prefixes/clears, 40 quoted phrases, 40 source-filtered queries and 40 zero-hit queries. Warm and auditioning workloads each repeat three times (600 samples); indexing has 200. The audition workload issues playback before entering its query loop, so its first requests can overlap playback startup; it does not record a per-query playing flag. Continuous playback during searches is also checked separately by callback-frame progress and the native controls test. The native endpoint is `LibraryView::paintOverChildren` for the matching request ID after its first result window or completed empty state is applied. Only the visible rows need painting. Debounce is separately recorded as **0 ms**. This includes dispatch, queue/database work, message delivery and native painting; it excludes compositor/display scanout. These are not database-only microbenchmarks.

Incremental timing is file-close to **UI-applied** searchable state, not its first paint (five additions). Discovery timing begins at the first eligible directory entry and ends at the first committed searchable batch. Filename throughput ends when the expected 100,000 names have committed. Memory is the main process's peak RSS, excluding the audio helper. Filesystem counters count explicit source-file `lstat`, directory fingerprint checks, entry inspections and enumeration separately; they are not a syscall trace of JUCE, SQLite or macOS.

## Before and after

| Native dispatch → first paint (ms) | Samples | Before p50 / p95 / p99 | After p50 / p95 / p99 |
| --- | ---: | ---: | ---: |
| Search during indexing | 200 each | 99.14 / 199.03 / 207.46 | 7.70 / 10.18 / 13.32 |
| Warm search (3 runs) | 600 each | 98.75 / 106.84 / 198.77 | 7.46 / 8.49 / 12.37 |
| Search during audition (3 runs) | 600 each | 99.01 / 107.09 / 199.06 | 7.39 / 8.45 / 8.76 |

| Other measure | Before | After | Proposed target / scope |
| --- | ---: | ---: | --- |
| Process launch → nonempty paint, n=1 | 397.15 ms | 190.36 ms | Fresh process; OS caches uncontrolled |
| First eligible → searchable batch | 0.357 ms | 0.744 ms | ≤ 2,000 ms: pass |
| Filename discovery | 19,905 files/s | 14,549 files/s | ≥ 500: pass |
| Filename stage elapsed | 5.024 s | 6.873 s | Same first-eligible boundary |
| All headers committed, from benchmark start | Not captured | 10.666 s | Includes overlapping discovery/header work |
| Incremental close → UI apply, n=5, p50 / p95 / p99 | 2208.00 / 2313.01 / 2313.01 ms | 878.62 / 1388.11 / 1388.11 ms | Searchable state; not painted endpoint |
| Cached audition callback, n=20, p50 / p95 / p99 | 42.73 / 47.01 / 48.58 ms | 36.82 / 43.10 / 43.32 ms | p95 ≤ 150: pass at 48 kHz / 512 frames |
| Main-process peak RSS, full workload | 146.78 MiB | 123.61 MiB | Audio helper excluded |
| Peak durable pending headers | 99,986 | 99,400 | SQLite backlog; not RAM queue size |
| Queued write batches | No separate queue; serial indexer | 8 observed maximum, 64 records/batch | One pending search in both versions; new active-query cancellation |
| Healthy root visible with one 5 s delayed root | 5017.80 ms | 128.81 ms | Service result, n=1 controlled case |
| 35 s reopen: source-file stats | 200,010 | 0 | Explicit source-file calls |
| 35 s reopen: entry inspections | 202,050 | 0 | Candidate enumerated 0 directories |
| 35 s reopen: directory visits/checks | 2,044 | 2,044 | Candidate fingerprint checks, plus 36 root jobs/probes |
| 35 s reopen: discovery commits / header jobs | 6,256 / 0 | 0 / 0 | Zero unchanged metadata jobs: pass |

Final warm-run p95 values: 8.84, 8.54, 8.41 ms. All 200 indexing samples were recorded while work remained active. Controls peaked at 135.80 MiB main-process RSS separately. The six longer clips decoded all 405 source seconds in 127.30 ms in the cached offline decode check; this is separate from live audition timing.

The baseline was captured before optimization from commit `d545ace40e6f49c50c728c2c7cebf18276266223` with timing instrumentation only. The preserved executable, instrumentation patch and source archive are under `build/benchmark`. The final candidate's executable SHA-256 and portable measurements are in [the verification record](verification/library-performance.json).

Warm search missed the proposed 50 ms p95 baseline target. Inspection showed 100 ms UI polling, repeated root aggregation and full ranking sorts on the interactive path. The measured replacement removes those costs and cancels obsolete work. Filename discovery throughput decreased with directory checkpoints, cached counters and serialized batch handoff; it still exceeds the 500 files/s target. The audition result qualifies the observed **48 kHz / 512-frame MacBook Pro Speakers** configuration, not the specification's proposed 256-frame configuration. Its endpoint is the first callback copying prepared PCM, not acoustic output latency.

No application FIFO starvation was observed in the recorded live runs. This counter observes a lack of prepared application PCM while playback expects more data. It cannot observe every hardware/driver underrun, physical DAC latency or audible defect. Full-file offline decoding is reported separately and is not an audio-device or model-performance measurement.

The 35-second unchanged reopen includes startup and one 30-second sweep. Zero new header jobs was already true in the baseline; the improvement is eliminating unchanged file enumeration, source-file stats and discovery transactions. Directory fingerprints and root probes still perform filesystem I/O. The full import and reopen retain 100,005 catalog records after five incremental additions; controls use a fresh 100,000-file import and one temporary selection fixture.

## Verification and limits

Both CTest suites passed: existing `library_and_audio` plus `library_performance_regression`. They cover interrupted import/WAL rollback, Unicode/formats, identity/rename/offline restoration, 4,096-match paging without duplicate UUIDs while importing, rapid 500-query replacement/source switch/clear, pause before and during enumeration, resume, scoped incremental work, unchanged reopen and Core Audio progress during paused searches.

The native benchmark also paints beyond row 1,000 during a paused import, replaces 500 rapid requests with the latest source filter, auditions/searches while paused, resumes to completion, and verifies selection/viewport retention when an alphabetically earlier recording arrives and results refresh. A controlled five-second root stall checks healthy-root progress separately at the service-result boundary.

The normal native picker and keyboard workflow was observed on this Mac: Add Folder imported the six longer files into an isolated catalog; Command-F entered a query; Down selected; Space started/stopped; a zero-hit search left the original audition advancing; Pause/Resume changed the visible state. Keyboard search, selection, playback and pause were repeated on the final measured binary. Automation needed the editor focused and the app raised before keys delivered reliably. A direct test catalog under Documents stalled in macOS `guarded_open`; using a temporary catalog resolved it. Scripted measurements launch the native executable directly with dedicated catalogs. Check privacy access for your own chosen folders through Add Folder.

Migration of a copied 100,005-record v1 catalog passed SQLite/FTS integrity and foreign-key checks with identical identity/revision/state digest. The disk-image test passed detach, offline retention, remount at a different path and unchanged UUID/revision. Physical USB/Thunderbolt, network shares, sleep/wake hardware cycling, VoiceOver usability, human listening quality, external audio interfaces, older macOS and the reference M2 hardware remain manual/unqualified checks. This is development verification, not human UAT or a signed/notarized release qualification.

| Requirement | Covered in this milestone | Remaining partial coverage |
| --- | --- | --- |
| LIB-01 | Multiple in-place roots, progress/availability, isolated slow-source work | User exclusion patterns; physical/shared-drive qualification |
| LIB-02 | Watch before enumeration, progressive filename batches, later headers, durable backlog | Visible-item analysis priority, waveforms, text/semantic stages |
| LIB-03 | Unicode filenames/paths; WAV/AIFF/FLAC technical headers; per-item failures | BWF/iXML description fields, UCS mapping, overrides |
| LIB-04 | Coalesced directory work, settling, directory checkpoints, recovery, immediate catalog reuse, unchanged-header avoidance | Persistent FSEvents cursor; full network/wake fault matrix |
| LIB-06 | Pause/resume, explicit Rescan/retry, root/stage/count/backlog display | AI root policy, audio-hour backlog and capture-aware scheduling |
| SRCH-01 / SRCH-02 | Exact-name/UUID promotion, prefix/phrase/source filters, FTS5, bounded cancellation, newest-request painting, complete paged access | Structured metadata/tag/UCS filters and concurrent semantic retrieval |
| PERF-01 | End-to-end paint timings, 3 × 200 warm/auditioning runs, indexing, empty states, RSS, discovery, reopen and incremental counters | Complete per-stage baseline elapsed times and peak disk I/O/storage telemetry; reference-hardware qualification |
| PERF-02 | Corpus bytes, duration and length distribution; separate long-file decode accounting | Semantic windows/s, source-hours/hour, overlap/model version and thermal qualification: deferred with embeddings |

LIB-05/DATA-01 identity behavior and the existing real-time callback separation remain intact. SRCH-05's full relevance fusion is not implemented; the ordering tradeoff above is explicit.

## Reproduce and inspect

Exact build, launch, benchmark commands and the five-minute own-library test are in [README](../README.md). Run one benchmark at a time with a visible native window and a fresh output directory. `prepare` reuses an existing generated fixture; run catalogs are isolated, and original user sounds are never modified. The generator and runner validate ownership markers before removing their own incremental fixture files.

Evidence retained in this workspace (build outputs are gitignored):

- `build/benchmark/baseline/summary.json`, `full.json`, `reopen-completed.json`, `cold-open.json` and corresponding logs; `Baseline.app`, `baseline-source.tar.gz`, `instrumentation.patch`.
- `build/benchmark/after-final/summary.json`, `full.json`, `reopen.json`, `controls.json`, `long-decode.json` and logs; `build/benchmark/after-final-run.log`.
- `build/benchmark/candidate-test.log`, `final-build.log`, `build/Testing/Temporary/LastTest.log`.
- `build/benchmark/baseline-slow/result.json`, `after-final-slow/result.json`, `migration/result.json`, `final-volume/volume-result.json` and their logs.

Raw per-request samples and per-run percentiles are retained. The initial fixture with detected filename collisions (`baseline-invalid-fixture`) and exploratory `after-1` run are excluded. The baseline reopen with one unfinished header was rerun after completion; only the completed catalog supplies the unchanged-reopen comparison. The final candidate rerun includes the selection fix and native selection assertions.
