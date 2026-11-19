# Performance Mixer v1.2 — reliability

Implemented and qualified on 2026-11-18 against starting HEAD `97bf7c42f05fec73192193c0061ba174a7d96ab1`; clean checkout. The complete user request is attachment `7cc347b8-f6a7-43fd-9afe-fbc2dc0765ed/Pasted text.txt`. Supplied review and companion implementation prompt are retained in `docs/reviews/`. Preserve the corrected v1.1 blend laws/version gates, cancellation latch, source cards, theme/Lucide system, fixed 10-point corner fades, Edit operations and shared transport/output architecture.

## Corrections

R1 separates request-aware explicit selection pinning from missing-page fetching. Loaded selections pin membership without fetching 512 rows. The service serializes membership-changing sequence adoption against a concurrent pin. Missing-page requests remain request-aware and deduplicated by published page serial. A rejected in-flight reorder also restores the previous annotation map, preserving selected ranges. The view restores stable IDs, keeps an explicitly identified cached selection outside a loaded page, and never resolves a missing ID to the old row index. New queries/explicit refresh clear absent UI selection while preserving independent audition; deliberate new selections can still follow active Library audition. Actual catalog deletion is explicit and stops only the deleted selection's audition. Pinned browse sequences remove deleted IDs without adding/reordering the remaining membership.

R2 cancels abandoned work cooperatively through the existing worker generation, including between decoder blocks. A newly selected uncached source can preempt a lower-priority job. Identical still-wanted demands do not churn. Scheduling considers all bounded surface demands instead of stopping at the first 48. Oldest-service fairness crosses surface priorities every fourth ordinary admission. After three foreground admissions a useful background job gets a protected admission, so rapid continuous selection cannot starve background work indefinitely. Abandonment still cancels that job. This is job-based fairness: a selected preview arriving during a protected background job may wait for that job's completion, and blocked OS I/O cannot be interrupted immediately. There is still one worker, 48 retained peak sources, 96 geometries and 40 demands per surface; up to eight selected/card sources receive retention reservations.

## Helper diagnostics

The five-second watchdog is unchanged. Bounded diagnostics retain eight lifecycle events, helper PID, process/playback/graph generations, monotonic heartbeat/command/status times and sequences, first health-failure reason, pending inbound bytes, exit code/signal, supervisor signal, last observed device/resource status and a 16,384-character stderr tail. Logging and cleanup are outside the audio callback. A stopped helper is resumed for termination, with bounded escalation/reaping. A restart notice survives subsequent healthy idle statuses until deliberate playback. No automatic replay of a possibly stale design is introduced.

Intentional SIGKILL, SIGSTOP/watchdog and explicit reconnect tests are separate from ordinary workload results. They verify classification/reporting and preservation of the live recipe, saved document and durable output. The original v1.1 restart cause remains unresolved unless new evidence identifies it; later passing runs alone cannot close it. Next trigger: preserve the first event's timestamps/sequence, stderr, exit/supervisor signal and last device/resource status before another reconnect.

## Qualification and closure

Machine: **MacBook Pro Mac17,8, Apple M5 Pro, 48 GiB, macOS 26.3.2 (25D2150)**. Native Retina 2× display and built-in MacBook Pro Speakers. Release arm64 application and bundled helpers; exact binary hashes, final test results and measurement definitions are in [the verification record](verification/performance-v12.json). Raw final run records and the complete corpus manifest are committed as gzip JSON beside it. Resolve was already running in the background; no claim of an otherwise idle/fresh OS. Existing power assertions were observed, with no power-setting changes.

| Finding / gate | Disposition | Actual evidence and source |
| --- | --- | --- |
| R1 loaded-row pinning | Fixed | `LibraryView.cpp` selectedRowsChanged/restoreSelection; `LibraryService.cpp` pinSelection/searchLoop; `Catalog.cpp` reconcileBrowse. Fail-before regression, actual view/service indexing, semantic update, range, pending/playing/stopped identity, new queries, explicit refresh, paging and deletion. Native earlier-file import retains audition. |
| R2 obsolete decode / >48 demand starvation | Fixed | `WaveformPreview.cpp` tick; `Waveform.cpp` run. Held block-boundary job cancels without manual release; selected priority, no-churn, stale completion, useful background progress, and all 80 simultaneous sources completed at least once with 48 retention. |
| Historical helper restart | **Cause unresolved; bounded investigation completed** | Bounded diagnostics/fault classification added in `AudioIPC.cpp`/`AudioEngine.cpp`, `tests/AudioHealthTests.cpp`. Final ordinary runs have zero events. This does not identify or close the historical intermittent cause. |
| 10,000 genuine-file gate | Passed, stated synthetic diversity | Exactly 10,000 real readable WAV paths; all headers/sizes/SHA-256 rechecked after runs. |
| 30-minute active 48k/512 soak | Passed | 1,800,005.565ms active; 1,197 queries/selections/whole viewports; 299 pinned semantic refreshes; continuous nonzero design playback; zero underruns, helper events, retargets or incomplete foreground jobs. |
| Separate 48k/256 | Passed 60s check | 60,003ms; 40 viewports; zero underruns/events. **Not a 30-minute 256 qualification.** 512 restored afterward. |
| Functional/native regressions | Passed within limits below | All 17 current suites; native snapshot/reopen, card alias/replacement/drop/Undo, full/primary render/print-back, normal/minimum layouts. |
| Physical Escape/listening/display | **Unverified** | Optional human request sent only after automated/native work; no result received. Callback cancellation, PCM and screenshots are separate evidence. |
| Resolve original/full/selection delivery | **Blocked** | Studio 21.1.2 reachable. No accepted native receiver drop; no audio-track placement or save/exit/reopen evidence. Current app-scoped drag automation cannot establish the physical pointer drop. Prior attempts are retained, with no inferred receiver rejection. |
| Recorded Perform takes | Prepared only | [Next-slice brief](PERFORMANCE_TAKES_NEXT.md); no recording code/control added. |

### Corpus and workload

`build/performance-v12/fixture-qualified` contains **10,000 WAVs /1,253,073,116 bytes /133 distinct whole-file hashes**.128 short generated PCM templates are intentionally copied to independent files, plus 24 independently generated long sources including one hour. Rates 8/22.05/44.1/48/96k; mono/stereo/4ch; silence, sparse impulses, envelopes; duplicate/long names. This is genuine source-I/O/file-count qualification with limited content diversity, not commercial-library relevance evidence. [Manifest](verification/performance-v12-corpus-manifest.json.gz) lists every original path/header/size/hash. All 10,000 still match after qualification. A separate 40-minute source drives 8 layers/26 stock inserts; ordinary EOF is beyond the soak. Its post-run hash is recorded; no pre-run hash was retained for that extra source.

The visible native workload searches text and semantic queries, selects loaded rows through the actual view handler, scrolls whole viewports, publishes pinned semantic refreshes, adds 16-file batches, disconnects/reconnects an owned source folder and continuously changes XY/macros during Designer playback. Library browsing cannot take over that Designer transport. Controlled tests/native Library checks separately cover Library audition selection-follow. The soak indexed 60 incoming batches, ending with 10,961 assets. Nonzero processed signal was observed in **652,623/652,623 checks** and the playback clock reached 1,800.0s; it was not a mostly silent idle test. Monitor gain.015 is excluded from pre-monitor evidence. No human listening result is inferred.

### Measured latency

Values are **p50 /p95 /p99 milliseconds**. Query timing ends at the native Library paint acknowledgement, with semantic completion required for semantic cases. Selected waveform timing ends at data adoption on the UI thread, **not first physical paint**. Whole-viewport timing waits for each actual visible row's current geometry and paint callback; offline/error terminals are counted separately. Exact-height half-open bounds exclude an off-screen row.

| Condition | Queries / selected previews / complete viewports | Query → paint | Selection → preview data | Whole viewport |
| --- | ---: | ---: | ---: | ---: |
| Cold derivatives, 512,120s |80 /80 /80 |5.93 /13.00 /25.59 |26.24 /49.31 /56.97 |236.90 /284.56 /310.99 |
| Warm derivatives, 512,120s |80 /80 /80 |6.10 /13.04 /24.89 |29.42 /52.62 /63.54 |226.93 /267.73 /311.97 |
| Active 512, 30min |1197 /1197 /1197 |6.28 /13.63 /15.33 |27.75 /55.12 /65.78 |233.51 /277.45 /306.49 |
| Separate 256, 60s |40 /40 /40 |5.64 /13.26 /14.16 |24.68 /55.55 /66.63 |236.33 /293.38 /308.58 |

The soak's 34 resident-at-click previews measured 11.92 /32.09 /33.71ms; 1,163 nonresident previews 28.42 /55.12 /65.78ms. Small resident cohorts in the 120s runs are not meaningful comparative distributions. Last-coalesced-edit → first tagged callback PCM measured 55.30 /70.09 /74.38ms over 44,997 adopted edits in the soak; this excludes OS dispatch, DAC/acoustics. Pool internal adoption samples are capped 512; the selected-preview distributions above retain all selections.

Cold means an empty derivative directory, **not a cold OS file cache**. Warm uses a fresh process with copied derivatives/catalog/source IDs and prunes only owned previous incoming roots before repeating indexing. Model coverage differs, so no statistical warm-speedup or relevance claim is made.

| Condition | Decode attempts / derivative adoptions | Cancel / priority preempt | App / audio / AI peak resident MB | Retained peak bytes | Model complete / pending / failed |
| --- | ---: | ---: | ---: | ---: | ---: |
| Cold 512 |874 /293 |175 /102 |169.89 /51.89 /460.90 |544552 |577 /9488 /0 |
| Warm 512 |47 /1048 |172 /96 |190.32 /51.94 /458.11 |547728 |1120 /8945 /0 |
| Soak 512 |7960 /8686 |2642 /1529 |234.44 /51.92 /468.81 |553016 |8468 /2493 /0 |
| Separate 256 |459 /129 |83 /49 |165.51 /51.90 /464.08 |552288 |302 /9731 /0 |

Decode attempts include cancelled work. Derivative adoptions validate source identity. All runs ended at 48 retained sources/96 geometries, with no pending query, selected preview, viewport or waveform worker job. Process memory includes more than cache payload and is reported separately. Model `clap-htsat-unfused-q8-c28f2883575e` remained error-free; analysis coverage was incomplete. The soak had 299 semantic/898 text queries. Every run kept its initial helper/process/play/graph identity during the active interval: cold/warm/soak graph 1, separate 256 graph 2 after intentional device configuration before active timing.512 was restored afterward. Zero underruns/restarts/health events in all four runs; single preparation in each 512 active workload. Long-term/external-device performance and other chips/scaling are not qualified.

### Helper failure investigation

Intentional SIGKILL produces socket-close/signal 9 with no supervisor signal. SIGSTOP produces the unchanged watchdog at about 5,008ms, then supervisorTERM 15 after resuming the process. Explicit reconnect has its own classification. Live/saved document recipes and a real 960,000-frame finalized WAV/receipt survive these paths; its SHA is `62a7d2a075bf302eec38d517bc9bc6636b92dc99108dbd5d7da1c8e63adb82f2`. These are deliberate fault tests, separate from the ordinary workload distributions. First-reason diagnostics remain bounded to 8 events / 16,384 stderr characters. No new callback logging or timeout extension was introduced.

The original v1.1 restart after 425 query acknowledgements remains in [the old record](verification/performance-v11.json), separately from its two normal-EOF driver failures. Next occurrence: retain the first health event's timestamps/sequences, pending IPC bytes, helper/process/play/graph identity, stderr, exit/supervisor signal and last device/resource status before reconnect. Passing the specified bounded investigation does not prove its cause fixed.

### Automated and native workflow checks

The exact final app passed **17/17 CTest suites in 59.81s**: new library stability, waveform scheduling, audio health plus waveform previews, performance state, view commands, icons, Library/audio, selection audition, performance/search-integrity, similarity, transport, canvas, session, workflow and fades. [Complete log](verification/performance-v12-ctest.log). This includes new/legacy blend/version gates, all six slider cancellation callback sequences, range/reverse/offline/relink, Edit move/trim/slip/fade/scrub, session/snapshot recovery and pre-monitor versus offline PCM maximum error 0.0.

Native QA used `build/performance-v12/native-catalog` and a copy at `build/performance-v12/native-short/workload.soundplus`, never the working document. Loaded z-native 05 stayed selected and playing while 30 earlier files arrived; explicit refresh offered 61 results without audio takeover; deliberate next selection followed playback; stopped selection stayed silent. Alias editing/replacement retained the alias; internal card drag swapped assignments; a Library drop into an empty card added one ordinary layer/assignment and one Undo removed both.

Saved **Reliability static** at revision 1943 reopened selected with 8 layers, Weight.800, othermacros.500, X.700, Y.300 and four Ready source cards. Full and primary selected **Body** outputs are finalizedPCM 24 stereo 48k:2,242,286 and 1,968,000 frames respectively, including 1s tail. Paths/receipt hashes are in the record under `nativeUi.outputs`, in `~/Music/SoundPlus/Performance v1.2 QA/`. Their content hashes match the prior unchanged recipe renders. Render Selection retains its documented **primary-layer**, resolved-then-isolated semantics; it is not a timerange or multi-layer export. Print-back added a ninth normal layer; singleUndo restored 8. Files/receipts remained intact after SoundPlus exit/reopen.

Normal 1360×860 and minimum 1040×648 logical content were inspected at native 2×. Browser+Inspector, track buttons, output controls, card focus and waveform/fade controls remain usable; labels truncate with full paths in tooltips. Focused source-card Return opens Edit; track mute/Inspector toggles synchronizeOn/Off. The attempted Space check after AX row selection did not establish keyboard focus, so native Space is **not** newly qualified by that attempt; the actual Play action and keyboard suites are separate evidence. No toolbar disappearance reproduced; no repaint workaround was added.

Screenshots show the changed state and preserved layout:

- [Library pinned while indexing](verification/performance-v12-library-pinned.jpg)
- [Perform normal](verification/performance-v12-perform.jpg) and [minimum with card focus](verification/performance-v12-perform-minimum.jpg)
- [Edit normal](verification/performance-v12-edit.jpg) and [minimum with Browser/Inspector](verification/performance-v12-edit-minimum.jpg)

Optional human observations were requested only after the automated/native work. No result was received: physical held-pointer Escape, acoustic listening and toolbar behavior on the physical display remain unverified. ResolveStudio 21.1.2 is reachable, but no actual imported native-drop item was established. Original/full/selected delivery each remains **Blocked**, with placement, save/SoundPlus-exit/project-reopen and copied-versus-referenced status **not established**. Existing three prepared files were rehashed unchanged; [the precise manual receiver workflow](BUTTON_SEMANTICS_AND_RESOLVE_DELIVERY.md) remains available. An API import, internal drop, Finder test or native-drag-start return does not close it.

### Preservation

The latest working **Rain glass design r 9054**, not the earlier r 8567, was backed up before changes and reopened at the end. Its protected `.soundplus` SHA remains `0f7f0f2fc79edd133c091a5a77853473dc82cbee04495492cf5531a828c6ef55`, with Kept “Rain glass - kept B”, legacy law, Blend off, X.607/Y.321, monitor .70 retained. No older snapshot was restored over it. All 10,000 corpus manifest hashes/sizes/headers, 7 prior durable QA outputs and 3 working-document output receipts match. New output hashes/headers match their finalized receipts. Temporary QA state/output is retained independently.

### Reproduce

Close processes using this bundle before rebuilding. Full configure/bootstrap instructions are in [README](../README.md). From the repository:

```sh
.tools/cmake/data/bin/cmake --build build --parallel 6
.tools/cmake/data/bin/ctest --test-dir build --output-on-failure
open "$PWD/build/SoundPlus_artefacts/Release/SoundPlus.app"
```

For the owned real-audio workload, install the pinned optional model first (`python3 scripts/install_model.py`), close the normal app, and run one benchmark at a time. These are the executed commands; output directories must be fresh, so use new names when repeating. Preparation writes about 1.5GB including templates/40-minute design source and may take several minutes.

```sh
python3 scripts/benchmark_reliability.py prepare --output build/performance-v12/fixture-qualified --files 10000
python3 scripts/benchmark_reliability.py run --fixture build/performance-v12/fixture-qualified --output build/performance-v12/cold-final --seconds 120
python3 scripts/benchmark_reliability.py run --fixture build/performance-v12/fixture-qualified --output build/performance-v12/warm-qualified --seconds 120 --warm-from build/performance-v12/cold-final
python3 scripts/benchmark_reliability.py run --fixture build/performance-v12/fixture-qualified --output build/performance-v12/soak-512 --seconds 1800 --buffer-frames 512
python3 scripts/benchmark_reliability.py run --fixture build/performance-v12/fixture-qualified --output build/performance-v12/buffer-256 --seconds 60 --buffer-frames 256
```

Leave the native app visible during measurement. The benchmark writes `native.json`,`native.log`,`summary.json` and its plan per run. It requires the installed local model and an active CoreAudio output. Device buffer selection is a real device setting; restore 512 after a 256 run by repeating a 512-frame benchmark with a fresh output directory. The report records actual adopted rate/buffer throughout. Committed gzip records can be read with Python `gzip.open(...,'rt')`/`json.load`; source assets themselves remain local.

Five-minute Library check: play a result, add a folder containing earlier-sorting matches, confirm the selected filename/range/audio stay fixed and RefreshResults appears; refresh, select a new result and confirm active-only audition-follow; stop and select again to confirm silence; quickly scroll/select while waveform loading and check foreground progress; reopen the same catalog and confirm the indexed library persists. Avoid changing the working design for this check.

## Next feature

[Recorded Perform takes](PERFORMANCE_TAKES_NEXT.md) is prepared as the next creative slice. It is not implemented in this reliability milestone.

## Development evidence retained

- The R1/R2 regression cases fail before correction and pass afterward.
- The first intentional helper crash found a new diagnostics event-lifetime bug; retaining the event var fixes it. Subsequent crash/watchdog/reconnect tests pass.
- An initial corpus generator reused duplicate paths (9,190 unique files); that native run was aborted before active qualification. The corrected generator asserts both unique manifest paths and filesystem count at 10,000.
- The first valid-corpus run hit a harness viewport-count error at an exact row-height boundary. Actual visible rows and the selected preview had data; an extra off-screen row was being awaited. The shared interval calculation and boundary regression correct that assertion.
- The fault test's first output-preservation assertion hashed its output directory. The corrected test uses the renderer's returned WAV path and first checks actual file existence, nonzero frame count/peak, size and receipt SHA-256. Only the corrected assertion is used as output-preservation evidence.

These failed/invalid attempts remain separate from final ordinary workload distributions. There is no inferred cause for the earlier v1.1 restart.
