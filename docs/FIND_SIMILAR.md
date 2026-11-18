# Find Similar and semantic retrieval scaling

Implemented from clean commit `8043b22ea396713241aad0e76ef1c85586637571` on Apple Silicon macOS. The previous [local AI-search report](LOCAL_AI_SEARCH.md), [library-performance report](LIBRARY_PERFORMANCE.md), their JSON verification records and the architecture specification supplied the working baseline. This milestone implements the Library's SRCH-04 workflow and replaces repeated vector loading on interactive searches with a persistent, rebuildable **exact** retrieval cache. No approximate index or new model dependency was introduced.

## Using the application

Select an indexed recording, mark its useful region in the lower waveform, then click **Find Similar**. The upper reference panel pins its filename and exact range independently of candidate selection. Its play icon auditions the reference; the lower transport auditions candidates. Right-click the reference waveform or use ⌘E for exact IN/OUT edits, or drag its waveform then click its **Find Similar** button. **Include source recording** allows other moments from the reference take; the default excludes that recording before ranking.

The audio-file icon opens a native file picker; dropping one supported local audio file also creates a reference. An external reference is **temporary** and does not create catalog assets. Its separate folder-plus action explicitly adds the **containing folder**, including other audio files, using the existing in-place Library workflow. It does not copy the reference or silently import neighboring files. Individual-file-only catalog roots are not introduced. Reference selections are session state; completed exact-range embeddings persist across launches.

Transport, previous/next, folder, reference-file, pause/resume, retry, download, reconnect and waveform zoom/fit actions use native vector icons. **Find Similar** retains a label for discoverability. Every icon retains a button name, tooltip and keyboard focus indication. Command-F searches; Down moves to results; Space respects text-entry focus. The global stop button stops the current audition; previous/next selects and auditions an alternative. A late search never retargets pinned playback.

The playback strip and its primary Play button are centered beneath the waveform. Quiet icon tools and a larger circular Play button reduce visual clutter. The [ button precedes Play Whole; ] follows Play Match. Inline time inputs are hidden in both audition panels. **I** marks IN at the playhead; **O** marks exclusive OUT, in either letter case. **[ / ]** buttons and draggable bracket handles provide pointer alternatives. The stopped playhead remains visible. Shortcuts respect text editors and target the reference when its waveform has focus. Marking IN beyond the previous OUT restores the recording end; marking OUT before IN restores the beginning. Endpoint marks retain at least one sample. Exact numeric editing is available in a right-click / ⌘E popup; Return applies and Escape cancels. A newly selected recording clears the preceding waveform while its own peaks load.

A stopped waveform click now parks the playhead without starting audio. Play/Space uses that parked position; explicit Play Whole/Play Match start their respective ranges. An active seek restarts decoding at the requested original sample while preserving playback. Clicking an inactive waveform does not retarget the currently playing reference or candidate. Stop intent is visible immediately to the UI, closing the Stop-then-seek race before the helper acknowledges the stop. Range edits and source changes discard obsolete parked positions. Routine Playing/Stopped filename text is hidden; playback errors remain visible. Icon focus uses colour without an outline, and Rescan / Retry Analysis / Reconnect Audio use folder-scan / waveform-retry / plug glyphs.

## Reference encoding and channel policy

The installed paired CLAP checkpoint, ONNX Runtime 1.22.1 CPU/MLAS, 512 float32 dimensions, cosine metric, tokenizer and native preprocessing are unchanged from the preceding milestone. The active model is `clap-htsat-unfused-q8-c28f2883575e`; library preprocessing remains `soundplus-clap48k-slaney-repeatpad-sinc32-v1`. Runtime/license/checkpoint provenance is in [DEPENDENCIES](DEPENDENCIES.md) and `resources/clap-model.json`. The native audio/AI private protocol is now version 3, including audition-channel routing and analysis RMS.

A reference key includes stable asset ID/revision (or temporary file identity/location), file size/mtime/inode, original source sample rate/count, exact half-open original sample interval, model, preprocessing, channel-analysis policy and reference pooling version. Confirmed indexed relinks resolve through the catalog's current location; a location change alone does not invalidate the indexed reference vector. External paths are canonicalized off the UI thread. Source stamps are verified around header reading, each inference window, encoding completion and retrieval publication. This remains fast identity validation, not cryptographic source-content hashing.

Selections up to ten seconds encode their actual selected samples, using the existing repeat/pad preprocessing. Longer selections use overlapping ten-second windows at five-second stride; when more than twelve windows would be needed, twelve starts are spaced evenly from the selected beginning to the final ten-second window. Every sampled interval remains inside the original selection. Normalized audio vectors from nonsilent windows are averaged and normalized again; ranking by the pooled vector is proportional to mean window cosine similarity. This favors the overall selected sound. Narrow a mixed long take to the desired event for a more specific reference.

Reference pooling is `mean-nonsilent-max12-even-v2`. Windows below RMS 1e-6 are skipped; an entirely silent/cancelled mix produces an actionable state. This reference-only policy is in its cache key. It changes neither library window preprocessing nor existing library vectors. Progress reports attempted/completed windows, and reports skipped silent windows. A single semantic worker holds the newest pending reference/query; cancellation is checked before/after inference and before publication. Rapid edits cannot accumulate an unbounded queue. A changed, missing, empty, unreadable or unsupported reference leaves the rest of the Library usable.

The reference cache lives at `<catalog directory>/references`, is written atomically off the UI thread, and retains at most 512 entries. Reuse requires the full key and a finite normalized vector. Editing the selection computes its new representation; it never substitutes a whole-file vector for an edited region.

**Analysis continues to use the arithmetic average of all source channels.** The old audition API default was the first stereo pair. The normal UI now defaults to **Analysis Mix (all channels)**, and offers original stereo pairs. Changing this selector restarts the pinned audition at its current source position with the obsolete FIFO discarded. The selector changes monitoring, not analysis or its cache policy. The shared `PlaybackRegion` API retains its compatible first-pair default and explicitly accepts `auditionPair=-1` for the analysis mix, or 0–31 for source pairs.

Generated four-channel tests place a 317 Hz signal only on channel three: first-pair audition is silent, second-pair audition contains it, and Analysis Mix reproduces the encoder's one-quarter amplitude in both outputs. Opposing first-pair polarity cancels in the mean; its original pair remains auditionable even when that reference cannot be encoded usefully. Mixed sound/silence long references skip silent windows successfully. Native helper peak/generation checks verify both routing modes. Waveforms show the peak envelope across original channels, so they can remain visible when a mono average cancels. These signal tests do not establish semantic relevance or acoustic listening quality.

## Retrieval, persistence and resource controls

The archived exact scan missed the 300 ms warm target at 500,000 regions. Its measured cost was dominated by SQLite vector loading/filtering rather than dot products. The replacement keeps full-precision vectors in a memory-mapped flat file, with per-asset metadata blocks. Hard source filters and source exclusion apply before scoring/grouping. vDSP computes exact dot products; no quantization, approximate graph or recall tradeoff is introduced.

Recording-level ranking also fixes a dense-window failure: one recording can no longer consume a global 256-window budget and hide alternatives. Recordings rank by their best eligible window, with stable asset-ID ties. Up to 32 strongest windows per recording supply up to five nonoverlapping representative moments. Very dense takes may have other moments outside that bounded candidate list. The uncached SQL exact path remains available and uses the same recording/moment ranking policy for correctness comparisons. The preserved before-benchmark executable retains the earlier global-window grouping, so the comparison is the delivered baseline versus the completed implementation, not a cache-only microbenchmark.

SQLite is authoritative. Schema v4 adds a catalog identity and an 8,192-event bounded change log maintained by the existing serialized writer's triggers. Newly committed/replaced vectors, removals and affected asset states invalidate cached blocks. Queries overlay current vectors for changed assets, suppressing their old blocks. The delta is bounded to 32,768 regions; log gaps, oversized deltas or unavailable caches use the authoritative exact path. Offline entries remain eligible, matching the existing Library behavior. There is no separate availability-exclusion filter in the current UI.

One owned utility-priority builder creates immutable replacement generations from a consistent read snapshot. Vector data and metadata/source associations have SHA-256 checksums, namespace/catalog/sequence guards and structural validation. Data and the current manifest publish atomically only after validation; the prior usable generation plus current delta remains searchable while construction runs. Reopen verifies and maps a valid existing generation. Corruption or interruption rebuilds from stored embeddings without reanalyzing original audio. The cache format is version 2.

Background rebuilds coalesce until 1.5 seconds of quiet or 4,096 changes, avoiding a full rewrite on each audio window. Pause Indexing cancels construction at a safe boundary, rolls back its read snapshot, removes staging output and preserves the published generation. Resume rebuilds as needed; foreground queries and audition continue. Pause acknowledgement includes index work. Uninterruptible kernel I/O can still delay a boundary or shutdown. No worker is abandoned.

The cache supports up to one million regions; larger inputs use exact SQL with an explanatory state. Each mapped region stores 2,064 bytes (512 float32 values plus original start/end); metadata is separate. Two completed data generations are retained, with a third temporary file possible during rebuilding. Reference caches and query queues are bounded separately. Selected-waveform cache eviction, a configurable global disk budget and reference-M2 qualification remain future work.

## Workloads and results

Physical **MacBook Pro Mac17,8, M5 Pro, 48 GiB, macOS 26.3.2 (25D2150)**, internal APFS SSD; Release arm64, Clang 21.0.0 / SDK 26.5, JUCE 8.0.6. Audio checks use MacBook Pro Speakers at 48 kHz / 512 frames. OS caches and ordinary desktop activity were not controlled. “Cold” means a fresh process, not purged disk caches. Native submit-to-paint includes encoding, retrieval, fusion, message delivery and first painted matching page/completed empty state, excluding display scanout. Submission uses zero debounce; ordinary typing adds the existing 200 ms pause.

The capacity fixtures use the real SQLite tables and normal application retrieval/paint path at 10k / 100k / 500k regions. Seed 9417 generates 64 Gaussian 512-dimensional centers and 4,096 vectors with 0.15 Gaussian perturbations, normalized to float32. Vectors repeat cyclically; the largest fixture contains about 122 copies per distinct vector. Each asset has 50 windows. Sources are 98% broad, 1% narrow, 1% offline, plus an empty source. Physical 0.25-second WAV placeholders have deliberately synthetic catalog lengths/ranges; candidate files are not used for audio or relevance judgments. A separate 180-second deterministic PCM signal exercises active audition. Capacity is not audio-inference throughput or sound-quality evidence.

Each scale runs 200 natural-language variants three times warm and three times during active audition. Narrow and empty source filters are included. Cold construction/opening is separated from warm distributions. The 500k update workload adds 120 real five-second effects through the normal folder/background-analysis path while queries and audition run. It reports actual pending/playback flags and whether newly committed recordings reached painted results.

<!-- SCALE_RESULTS -->
All times below are milliseconds. Each warm or audition distribution contains **600 requests (200 × three runs)**. Verified milestone application SHA-256: `afefa0a856ec4a3498221177cd8658b05478c4c51b688c3ebf647fe136f60078`. The 10k/500k, keyword and semantic-regression runs use the preserved measured engine build `a6b323acccc36831eed9aa4cc8539b3834a2c2358fdd44aed91293fd6b2050de`; the 100k and reference runs below were repeated on the final marker-layout build. The helpers are byte-identical. Raw summaries identify each artifact.

| Regions | Before warm p50 / p95 / p99 | After warm p50 / p95 / p99 | After audition p95 | Main peak RSS (MiB) | Mapped vector data (MiB) |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 10,000 | 24.12 / 27.12 / 32.52 | 15.77 / 19.58 / 23.12 | 19.38 | 153.1 | 19.7 |
| 100,000 | 99.31 / 165.44 / 174.30 | 23.82 / 25.23 / 28.48 | 25.21 | 331.2 | 196.8 |
| 500,000 | 596.99 / 823.46 / 848.38 | 40.75 / 49.13 / 57.53 | 118.72 | 1118.8 | 984.2 |

| 500k component, warm p95 | Before | After |
| --- | ---: | ---: |
| fetchFilterMs | 761.351 | 0.165 |
| vectorScoreMs | 12.180 | 29.051 |
| rankGroupMs | 33.475 | 1.164 |
| fusionMs | 1.748 | 1.762 |

Scoring includes maintaining each recording's strongest-window candidates; its changed cost is not an isolated dot-product speed comparison. Component percentiles do not sum to the end-to-end percentile. Repeated SQLite BLOB materialization dominated the baseline.

| Warm source subset | 10k p95 | 100k p95 | 500k p95 |
| --- | ---: | ---: | ---: |
| capacity-narrow | 14.58 (n=90) | 16.70 (n=90) | 23.93 (n=90) |
| capacity-empty | 13.11 (n=30) | 12.89 (n=30) | 12.82 (n=30) |

The 500k live-update/audition workload measured **127.21 / 158.89 / 190.25 ms** (p50/p95/p99; n=200). All 200/200 samples had pending analysis and active playback; 142/200 painted a newly committed recording. 30 windows completed that session; remaining work is resumable. FIFO starvation was 0; this is not a driver-level underrun or listening measurement.

The 500k measured run began without a derived cache: validated generation construction took **16014.22 ms**, separate from warm queries, and model loading took 2536.01 ms. Valid-cache opens took 293.93 / 2069.37 ms at 10k/100k. The separate 500k recovery test measured checked reopen at 8241.28 ms. These are n=1 fresh-process observations, with uncontrolled OS disk caches.

The initial 500k SQLite fixture is **2,205,605,888 bytes**, including its vector BLOB pages; raw vector payload is 1,024,000,000 bytes. One mapped generation is **1,032,000,000 bytes**; two completed generations use about 2.064 GB plus metadata, with a third temporary file possible. Main-process peak RSS was **1,173,110,784 bytes** (1.093 GiB); AI-helper peak was separately 457,867,264 bytes. These are not synchronized combined peaks. Actual final generation/database file inventories are in the portable JSON.

All three measured warm p95 values meet the 300 ms target on this Mac. The smaller fixtures reopen unchanged caches; 500k uses a fresh catalog with the same deterministic vectors/identities/distribution before real updates. Baseline and candidate OS cache state/desktop conditions were not controlled. **All 90 exact/cached comparisons passed**, covering ten query vectors × three eligible subsets × three scales, full returned recording order, moments and cosine scores within 1e-6. There is no approximate search or approximate-recall tradeoff.
<!-- END_SCALE_RESULTS -->

The uncached exact reference and cached backend are compared on identical vectors, current revisions, model/preprocessing namespace, source filters and grouped ranking units. Because both are exhaustive, the approximate-recall gate is not applicable. Measured agreement is reported with its actual case count rather than inferred from the implementation.

<!-- REFERENCE_RESULTS -->
The ordinary native control path produced **200 uncached and 200 cached short-reference requests**, plus twenty annotated audio-reference cases. The uncached workload changes its selection interval on each request; cached requests repeat the identical final interval. All uncached requests encoded; all cached requests reused their exact-key representation. These timings use the real 43-file/74-region set, separately from capacity.

| Reference workload | Encode/cache p50 / p95 / p99 | Retrieval p50 / p95 / p99 | Submit-to-paint p50 / p95 / p99 |
| --- | ---: | ---: | ---: |
| example-uncached | 42.59 / 44.19 / 47.35 | 0.42 / 0.44 / 0.49 | 49.13 / 50.14 / 57.19 |
| example-cached | 0.23 / 0.35 / 0.37 | 0.48 / 0.57 / 0.59 | 8.03 / 8.16 / 8.63 |

One 60-second reference processed eleven windows with progress: **492.00 ms encoding**, **0.427 ms retrieval** (n=1). CUA separately observed 6/11 progress. Indexed/external references, source exclusion, pinned independence, reference audition, range edits, newest-of-100 rapid requests, temporary short/long references and clear-reference all passed.

All **20/20** annotated reference cases returned a relevant alternative at rank one; long-region overlap was found in 6/6 cases with annotated long-region expectations. Actual top-ten recordings, source-sample ranges, expected alternatives and provenance are retained in the portable record. These are annotation proxies, with no human judgments or held-out generalization claim.
<!-- END_REFERENCE_RESULTS -->

The relevance set is the preceding attributed ESC-50 subset: 43 anonymous files, 382.5 source seconds and 74 regions, including real-event composites at 20, 25 and 40 seconds. Labels remain outside indexed media. Twenty audio-reference cases exclude their source recording and check alternative recordings/regions against event annotations. Compilation CC BY-NC 3.0; original filenames, hashes, Freesound/source links and derivation details remain in its manifest. Unknown CLAP training overlap means this is workflow evidence, not held-out generalization. Generated signal/capacity fixtures are never counted as relevance evidence.

The existing ten-pair listening review remains separate. No numeric human ratings have been received. The local HTTP review is available at `http://127.0.0.1:8766/listener-review.html` while the development server runs; automatic browser opening was denied, so it was handed off for manual opening. Native PCM/transport checks and nonzero waveforms do not establish that a human heard the sound. The 200 held-out queries / two listeners release gate remains unmet.

## Verification and remaining scope

The starting three CTest suites passed. New deterministic integration checks cover original-sample reference windows, multichannel/opposing-polarity routing through the native helper, exact/cached ordering and moments, source exclusion, dense-window alternatives, live replacement/revision invalidation, offline visibility, bounded-log gap recovery, cancellation, binary and metadata corruption, valid-cache reuse and background-index pause. Real-model checks cover indexed/edited/external/short/multi-window references, cache-key changes, rapid replacement, source changes during analysis, missing/empty/silent/policy failures and restart reuse.

Native controls exercise the indexed reference action, independent candidate audition, pinned reference audition, numeric edits, rapid generations, external-file drop handler, progress, clear-reference and painted results. Separate CUA operation observed the ordinary file picker, a 20–25-second marked region inside `clip-041.wav`, alternatives `clip-014.wav` / `clip-029.wav`, include-source behavior, reference edits, a temporary 60-second external file with visible 6/11 progress, query changes during reference playback, pause/resume, and the explicit containing-folder addition (43 → 89 assets). Icons retained accessible names. OS-level Finder file dragging and human VoiceOver use were not qualified.

<!-- REGRESSION_RESULTS -->
**PASS: four CTest suites** on the final build, including marker boundary semantics and native multichannel transport. The real-model reference workflow passed source changes, revision resolution, empty/missing/silent failures, stale generations, exact cache keys and restart reuse. The existing semantic workflow and preprocessing/tokenizer parity check passed; maximum log-mel difference was 0.000328 dB.

| 100k keyword workload, p95 ms | Previous delivered milestone | Measured engine build |
| --- | ---: | ---: |
| indexing | 9.42 | 10.17 |
| warm | 8.56 | 8.80 |
| auditioning | 8.58 | 8.85 |

On the preserved measured engine build, the 35-second keyword reopen performed 0 header reads and 0 directory enumerations. Pause/resume, paging past row 1000, latest source filter and stable selected ID controls passed. On the preserved measured engine build, the real-effect semantic regression measured warm/audition p95 16.09 / 16.20 ms, and 21 queries passed with per-process network access denied. Reopen encoded 0 windows. Pure analysis processed 74 windows / 382.5 source seconds at 15.15 windows/s and 78.28 source-audio hours per wall hour on this small warm-OS-cache set; capacity timings are separate.

Schema-v3 → v4 migration preserved identical asset/vector digests for 100,005 assets and separately 43 assets/74 vectors; SQLite integrity and foreign keys passed. The 500k recovery retest served the old generation during replacement, interrupted the builder while preserving its published manifest, reopened with a live delta and atomically published a replacement (9232.65 ms). This is owned-worker interruption, not an OS SIGKILL test. Its setup now waits for any prior update delta to be fully published before interrupting the next generation; the initial setup-assumption failure is retained.

The disposable HFS+ volume test passed stored volume identity, offline retained entries, remount at a different path, stable asset ID and unchanged revision. This does not qualify a physical USB/Thunderbolt drive. Native CUA observed the centered transport and brackets outside Play Whole / Play Match, stopped playhead, case-insensitive I/O, hidden inline inputs, candidate/reference range popups, Return apply, Escape cancel, and the same exact range in the pinned reference. Exact artifacts, logs and remaining gates are in the portable JSON.
<!-- END_REGRESSION_RESULTS -->

Development attempts remain visible: the first 500k cache pilot mixed cold construction with its warm samples; the qualified warm controller now waits for index readiness. A button-name/local-snapshot shadow caused a compile failure and was fixed. Dense-window and metadata-corruption fixtures initially reused inode identities or modified a copied JSON value; those fixtures were corrected before final passes. An offline-reference assertion was updated to test a truly missing external reference, because indexed references correctly resolve the current catalog location by UUID. Failed attempts are not folded into passing distributions.

| Requirement | Implemented slice | Remaining limits |
| --- | --- | --- |
| SRCH-04 | Indexed, marked and external references; actual selected audio; alternative recordings/moments; pinned reference, edits, audition, include-source, clear | Individual-file-only catalog roots; references themselves are session UI state |
| SRCH-02/03/05/06 | Current request generations, exact cached retrieval, consistent source filters/exclusion, stable recording ranking, long-reference policy and original-sample moments | Structured metadata/tag filters, feedback preferences; strongest-32 moment budget |
| SRCH-07 / DATA-01/03 | Authoritative versioned vectors, live delta, source invalidation, checked generations, migration, interruption recovery/reuse, indexed relink resolution | Full hot dual-model rollout/rollback; cryptographic original-file identity |
| LIB-02/04/06 | Progressive existing analysis retained; cache construction/reuse, bounded scheduling and pause/resume; actionable failures | Physical/network blocked-I/O and capture-aware scheduling qualification; global cache budget UI |
| DSP-03/04 / UX-01/02/03 | I/O-free callback, explicit analysis-mix/pair audition, shared region path, icons/tooltips/names, numeric alternatives, pinned state | Human VoiceOver, full DSP/plugin latency, external-interface and 256-frame qualification |
| PERF-01/02/03 | Native scaling, cold/warm separation, reference timing, exact agreement, memory/storage, real-effect annotations and preserved regressions | 200 held-out queries/two listeners; reference M2; sustained thermal/acoustic qualification |
| AI-04 / OPS-01 | Same installed local model, namespace checks, offline operation and repairable derived cache | AI-05 model-update qualification; OPS-05/06 signing/notarization/distribution audit; OPS-07 full hardware matrix |

An isolated catalog under Documents stalled during LaunchServices startup, with its worker sampled inside SQLite `guarded_open_np`; the UI remained responsive, but Quit waited on that I/O and the owned test process was terminated. OS protected-path handling is suspected, not established. The same fixture copied to a temporary catalog opened and quit normally. Keep the normal catalog in Application Support or use a temporary test catalog; this protected-path / blocked-I/O case remains unqualified.

Physical USB/Thunderbolt, hung SMB/NFS, sleep/wake cycling, older supported macOS and a 30-minute hardware/listening soak remain unqualified. Designer, plugin hosting and Resolve placement are excluded. This completes the implemented engineering workflow; it is not the architecture's complete release gate.

## Reproduce and inspect

The subsequent transport-behavior revision passed all four CTest suites and the native reference/transport regression (including stopped candidate/reference seeks, parked-cursor playback, active seeks, inactive-waveform independence and immediate Stop-then-seek). Native CUA also verified the borderless toolbar, distinct action icons and hidden routine transport line. Its artifact and receipts are in `subsequentUIRevisions`. Run the focused check with a fresh output directory:

```sh
python3 scripts/benchmark_similar.py --controls-only \
  --app "$PWD/build/SoundPlus_artefacts/Release/SoundPlus.app/Contents/MacOS/SoundPlus" \
  --fixture build/semantic/real-effects/evaluation/manifest.json \
  --output build/my-transport-check
```

After the recorded milestone, Mark IN/OUT glyphs were reduced to a maximum 14-pixel height with shorter horizontal arms. Only `IconButton.h` changed. The app was rebuilt and visually checked; the performance and functional results above retain their recorded artifact identities. The later cosmetic build fingerprint is in `subsequentUIRevisions` in the portable JSON.

Build, model-installation, launch and the five-minute own-library test are in [README](../README.md). Use fresh output directories and run benchmarks sequentially. A logged-in macOS session and available Core Audio output are required. The AI helper requires macOS 13.3+; the Library/audition build minimum remains 12.0, without older-OS hardware qualification.

```sh
.tools/cmake/data/bin/ctest --test-dir build --output-on-failure

# Existing attributed fixture in this workspace. On a fresh checkout generate it first:
# python3 scripts/make_semantic_fixture.py build/semantic/real-effects
build/SoundPlusTests_artefacts/Release/SoundPlusTests --similar-workflow \
  "$PWD/build/SoundPlus_artefacts/Release/SoundPlus.app/Contents/MacOS/SoundPlus" \
  "$HOME/Library/Application Support/SoundPlus/Models/clap-htsat-unfused-q8-c28f2883575e" \
  "$PWD/build/semantic/real-effects/media" "$PWD/build/similar-workflow-output"

python3 scripts/benchmark_similar.py \
  --app "$PWD/build/SoundPlus_artefacts/Release/SoundPlus.app/Contents/MacOS/SoundPlus" \
  --fixture build/semantic/real-effects/evaluation/manifest.json --output build/my-reference-run

# Repeat prepare/run for 10000, 100000 and 500000, with fresh fixture/run paths.
python3 scripts/benchmark_capacity.py prepare --regions 500000 --output build/capacity-500000
python3 scripts/benchmark_capacity.py run \
  --app "$PWD/build/SoundPlus_artefacts/Release/SoundPlus.app/Contents/MacOS/SoundPlus" \
  --fixture build/capacity-500000/fixture.json --output build/my-capacity-run --updates
build/SoundPlusTests_artefacts/Release/SoundPlusTests --capacity-accuracy \
  "$PWD/build/capacity-500000/catalog/library.sqlite" \
  "$PWD/build/SoundPlus_artefacts/Release/SoundPlus.app/Contents/MacOS/SoundPlus" \
  "$HOME/Library/Application Support/SoundPlus/Models/clap-htsat-unfused-q8-c28f2883575e" \
  "$PWD/build/my-capacity-accuracy.json"
build/SoundPlusTests_artefacts/Release/SoundPlusTests --capacity-recovery \
  "$PWD/build/capacity-500000/catalog/library.sqlite" "$PWD/build/my-capacity-recovery.json"
```

The real-model workflow recreates only a directory named `similar-workflow-output`. Capacity preparation creates an owned temporary media tree and a dedicated catalog. Recovery changes only an owned `capacity-*` fixture's vector-change sequence. `--updates` adds a new 120-file evaluation folder; prepare a fresh fixture to repeat the exact original region count. The baseline can be rebuilt at `8043b22` with [the timing-only instrumentation patch](verification/capacity-baseline-instrumentation.patch); pass that build's test binary to `prepare --tests` to create schema-v3 baseline fixtures. Its preserved app is `build/similar/ExactBaseline.app`.

Portable results and exact artifact/source fingerprints are in [verification/find-similar.json](verification/find-similar.json). Raw native plans, per-request samples, summaries, logs, migration records and observed UI state remain under `build/similar`. Full generated vector/media files and model weights are gitignored. The previous keyword and semantic benchmark commands remain in their reports.
