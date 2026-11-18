# Local sound-description search milestone

Implemented and exercised on native Apple Silicon hardware on 17 November 2026. The app can install its optional model, analyze original files progressively, search anonymous recordings by sound description, and audition an editable matching region with a cached waveform. The Library works without a project, DAW, account or installed model. This is a working development milestone; the full architecture evaluation and distribution gates remain open.

The starting revision was `758aa09afacf978508c10ad4f504b54ade30119a` (`performance`). [The preceding performance report](LIBRARY_PERFORMANCE.md) is the baseline: its four owned filesystem workers, serialized writer, bounded batches, durable header backlog, virtual pages, active-query cancellation, pinned selection, directory checkpoints and supervised audio process are retained. Its outstanding physical-drive, blocked-network-I/O, older-OS, accessibility, listening and release qualification items remain applicable. The first old test run exposed a timing race in the interrupted-import test: the import could finish between observing partial progress and destruction. The test now pauses at a durable boundary before asserting interruption; both baseline suites then passed. This was a test synchronization defect.

## Native workflow

Use **Install AI Model** to download the verified pack or import the same files from an offline folder. Installation stages files privately, checks the embedded SHA-256 allowlist and publishes atomically; cancellation leaves the active pack intact. A damaged installed pack is moved aside before repair. The native UI download was exercised against the real service. The installed pack survives app restarts. Filename search stays available during download, verification/loading, incomplete analysis or model failure.

**Add Folder** indexes files in place. Filenames appear first, then headers, then semantic windows; waveform peaks are generated for the selected recording on demand. Coverage displays analyzed files, indexed regions, pending work and retryable failures. **Pause Indexing** drains the current work at a safe boundary and pauses further audio analysis; queries and audition remain available. **Retry Analysis** retries failed windows and can restart a failed model worker. Selection prioritizes that recording's remaining analysis. Audition increases the delay between analysis windows from 20 to 150 ms.

Type an everyday description into the existing search field; Return bypasses the normal 200 ms typing debounce. Results explain **Filename**, **Exact filename**, **Asset ID**, **Sound similarity**, or a combination. A semantic hit displays an original-source time range. **Matching moments** exposes up to five representative nonoverlapping moments, with overlapping candidate windows grouped. Cosine similarity is shown as a ranking value, never a calibrated confidence percentage.

**Play Match** plays the chosen half-open range; optional context adds exactly 250 ms on each side, clamped to the file. Numeric IN/OUT fields use seconds converted to integer original-source samples. Click the waveform to seek, drag a selection or its boundaries to adjust it, **Zoom Range** to inspect it, and **Play Whole** to start the full recording. A seek inside a matching range plays its remaining portion. A seek outside it switches to whole-file playback from that point. Custom ranges are session state, not saved Designer edits.

Changing the query does not retarget the pinned playing recording. Selecting or paging freezes result order; a later semantic refresh offers **Refresh Results**. Other matching moments survive metadata refreshes. Command-F focuses search; Down moves to results; Space toggles audition with results focused and remains text input in editors.

## Model and runtime

The architecture's CLAP candidate is implemented using the paired [LAION `clap-htsat-unfused` checkpoint](https://huggingface.co/laion/clap-htsat-unfused) and [Xenova's quantized ONNX conversion](https://huggingface.co/Xenova/clap-htsat-unfused), pinned to `c28f2883575e590e04d3146ff0713c2448d691ba`. It was selected because it provides matching audio/text encoders, a native Apple Silicon runtime, and usable results on actual sound effects with a modest optional download. This is not a comparative model tournament or a claim of optimal relevance.

| Property | Implementation |
| --- | --- |
| Model ID | `clap-htsat-unfused-q8-c28f2883575e` |
| Preprocessing ID | `soundplus-clap48k-slaney-repeatpad-sinc32-v1` |
| Embedding | 512 float32 values, L2 normalized; exact cosine / dot-product ranking |
| Runtime | [Microsoft ONNX Runtime 1.22.1](https://www.nuget.org/packages/Microsoft.ML.OnnxRuntime/1.22.1), native arm64 CPU/MLAS; two intra-op / one inter-op threads, sequential execution, spinning and telemetry disabled |
| Acceleration | CPU SIMD; no CoreML, GPU or ANE provider enabled |
| Text | Native RoBERTa byte-level BPE from the pinned tokenizer JSON; 77 tokens including special tokens; variable-length input; the pinned text graph accepts `input_ids` only (its all-ones attention mask is folded) |
| Audio | All-channel arithmetic mono downmix; deterministic 32-tap / 1,024-phase anti-alias sinc conversion to 48 kHz; 10 seconds / 480,000 samples; short input repeated then zero-padded |
| Features | Periodic Hann 1,024-point STFT, hop 480, centered reflect padding; 64 Slaney-normalized mel bands, 50–14,000 Hz; log-power dB floor 1e-10; input `[1,1,1001,64]` |
| Windowing | 10 s length, 5 s stride, final partial window retained; stored boundaries are exact `[startSample,endSample)` at the original sample rate |
| Pack payload | 163,034,742 bytes across eight verified files; installed with manifest: 163,037,981 bytes |
| Default pack location | `~/Library/Application Support/SoundPlus/Models/clap-htsat-unfused-q8-c28f2883575e` |
| Deployment | Library/audition macOS 12.0 minimum; optional AI helper macOS **13.3+** because of the native runtime binary; old OS hardware not qualified |
| Licensing | Checkpoint card: Apache-2.0; upstream LAION CLAP code: CC0-1.0; ONNX Runtime: MIT plus third-party notices; JUCE distribution license remains to be selected. See [dependency record](DEPENDENCIES.md). |

The runtime dylib and both supervised helpers are bundled with the app. The AI helper resolves the dylib relative to `Contents/Frameworks`, without a vendor-checkout dependency. Python packages are optional build/reference tools only. `resources/clap-model.json` is the embedded trust root with full download URLs, hashes, sizes, tokenizer and checkpoint provenance. Both model-source licenses and runtime notices are bundled.

Native preprocessing was checked against Transformers 4.46.3 audio utilities: maximum log-mel difference **0.0003281 dB**, mean 0.000009318 dB on a deterministic 48 kHz fixture. Eight tokenizer cases, including Unicode, contractions, special tokens, empty text and truncation, matched tokenizers 0.22.2. Outputs were finite 512-dimensional unit vectors. This establishes implementation parity for the tested cases, not a model-quality certification.

## Persistence, isolation and retrieval

Schema v3 adds source sample counts, versioned analysis jobs, window embeddings and revision invalidation. Jobs and outputs carry asset UUID, source revision, model and preprocessing IDs. Each window is committed through the existing serialized writer before advancing its durable checkpoint. A failed window retains its start for retry, so retry cannot skip audio. Source size, mtime and inode are checked before/after decoding; stale outputs are rejected, and source-revision changes delete affected derivatives. These are fast source identity checks, not cryptographic content identity.

A single owned analysis thread and a separate coalescing text-query thread share a supervised inference helper. Queries take priority at window boundaries. There is one analysis request in flight, one latest pending query, a 64-entry text-vector LRU, and generation checks before searching/publishing. The helper validates the active model pair. Bounded watchdogs handle ordinary worker exits/timeouts; repeated transport failures offer explicit Retry. Decoder errors remain per-file and do not prevent healthy jobs progressing. Offline roots keep indexed vectors and search visibility; playback requires the source to return.

The vector index is currently an **exact filtered SQLite scan**, accelerated by vDSP dot products. It considers the active model/preprocessing and current source revision, selects up to 256 window candidates, and groups up to 100 assets / five representative moments each. Lexical and semantic candidates use reciprocal rank fusion (`k=60`), the same implemented source filter, and exact filename/UUID promotion. UUID lookup remains exact. The disk-backed browse sequence retains the complete lexical tail, including results beyond 1,000. There is no separate availability exclusion filter in the existing UI; both paths preserve offline results. No ANN library or large semantic-corpus performance claim is introduced.

Waveform decoding and cache I/O run on one owned latest-selection worker. Peaks are bounded to 65,536 min/max bins and keyed by asset/revision/source stamps. Publication is atomic, and generations reject stale results. Playback uses the shared `PlaybackRegion` API for future Designer/search-by-example clients. Seeking constructs a fresh PCM stream and discards its obsolete FIFO; command generations reject stale transport statuses. Original sample intervals convert explicitly to output frames, with interpolation clamped at the exclusive end. The real-time callback performs no file/database/model I/O, allocation or locking. Model work is in a separate process from audio.

## Relevance evidence

The repeatable evaluation contains **43 anonymous WAV files, 382.5 seconds, 33,738,392 bytes and 74 indexed regions**. Forty are real five-second ESC-50 excerpts across 20 environmental, mechanical, material and movement categories; three are explicitly constructed 60 / 60 / 62.5-second composites of those real recordings. Events occur at 20–25, 25–30 and 40–45 seconds; the 62.5-second file exercises the final partial window. The composite construction is documented rather than presented as an untouched long field take.

The pinned [ESC-50 source](https://github.com/karolpiczak/ESC-50/tree/33c8ce9eb2cf0b1c2f8bcf322eb349b6be34dbb6), original filenames, SHA-256 hashes, Freesound IDs/links, source URLs and compilation license are retained in `build/semantic/real-effects/evaluation/manifest.json`. Only `media/clip-NNN.wav` is indexed; category labels and expected regions remain outside the retrieval inputs. The evaluation-only compilation is CC BY-NC 3.0 and is not bundled with the app. CLAP training overlap is unknown, so this is a workflow set, not held-out generalization evidence. Tones are used only for deterministic transport tests.

For **20 positive natural-language queries**, the native UI's top-ranked asset agreed with an expected event annotation in **20/20** cases (also 20/20 within ten). Queries distinguish knocks from creaks, hand saws from chainsaws, clock ticks from alarms, and drips from pouring. All three long-file events were retrieved within the top ten at overlapping source ranges:

| Description | Long file / rank | Returned region | Annotated event |
| --- | --- | --- | --- |
| Wooden door creaking | `clip-041.wav`, 3 | 20–30 s | 20–25 s |
| Drinks can opening | `clip-042.wav`, 3 | 20–30 s | 25–30 s |
| Glass breaking | `clip-043.wav`, 1 | 40–50 s | 40–45 s |

The 21st query, a horse neighing beside a trumpet, deliberately has no convincing included event. The system still returns nearest neighbors (first: a wooden knock, cosine about 0.270); it does not implement calibrated abstention. All queries, expected assets/ranges and retrieved results are retained in `build/semantic/native-final/relevance.json` and summarized in the portable verification record.

**Human listening review is pending.** The ten-pair local review is `build/semantic/listener-review.html`. Dataset annotations are an automated proxy, not listener ratings. The requested 200 held-out queries, at least two listeners and broader material/clothing/latch coverage remain open PERF-03 / model-qualification work. The latency workload's 200 phrasing variants are not 200 independently labeled relevance queries.

## Measurements

Physical MacBook Pro **Mac17,8, Apple M5 Pro, 48 GiB, macOS 26.3.2 (25D2150)**, internal APFS SSD, Release arm64, JUCE 8.0.6, Clang 21.0.0 / SDK 26.5. Model/runtime as above; 43 files / 74 regions. No OS cache purge; fresh process/catalog for imports, completed catalog for reopen/offline. Model-load time includes verification and session creation; “cold” means a fresh process, not cold disk caches. Warm runs issue 200 distinct phrasing variants three times with **zero text-cache hits**. Power/thermal samples every second remained nominal, low-power mode off, during the full native workload.

Dispatch to the first matching **painted semantic result page or completed empty state** is measured in `LibraryView::paintOverChildren`. It includes worker scheduling, encoding, retrieval/fusion, message delivery and native painting, excludes compositor/display scanout, and uses Return's 0 ms debounce. Ordinary typing adds 200 ms. The lexical first paint is not substituted for the semantic endpoint.

| Native submit → semantic paint | n | p50 / p95 / p99, ms |
| --- | ---: | ---: |
| Analysis and audition both active | 200 | 15.46 / 48.15 / 49.35 |
| Warm, analysis complete | 600 | 15.53 / **16.07** / 19.18 |
| Warm while auditioning | 600 | 15.52 / 16.08 / 19.20 |

Every concurrent-analysis sample had pending analysis and active playback; every audition-only sample had active playback. Warm-run p95 values were 16.26, 16.03 and 16.02 ms. The proposed **p95 ≤ 300 ms target passed on this corpus**. No performance repair was needed to meet it.

| Warm component | p50 / p95 / p99, ms (n=600) |
| --- | ---: |
| Text preprocessing | 0.058 / 0.087 / 0.115 |
| Text encoder | 4.854 / 5.911 / 6.943 |
| Worker round trip (includes encoder) | 5.251 / 6.358 / 7.568 |
| Vector fetch and filtering | 0.051 / 0.069 / 0.085 |
| Vector dot scoring | 0.00150 / 0.00192 / 0.00221 |
| Candidate ranking and region grouping | 0.400 / 0.433 / 0.537 |
| Lexical/semantic fusion and page retrieval | 0.627 / 0.685 / 0.833 |

Components overlap by inclusion; they must not be summed as independent totals. The remaining end-to-end time includes UI scheduling and paint cadence.

Four fresh-process loads measured **2.117 / 2.147 / 2.158 / 2.150 s**; n=4 is too small for useful p95/p99 claims. Completed-catalog process launch to first semantic paint was **2.263 s**, n=1. Reopen read **zero new headers**, enumerated **zero directories**, and produced **zero new embeddings**. It still probes roots and loads the model.

Pure analysis processed **74 windows in 4.768 s: 15.52 windows/s and 80.23 source-audio hours per elapsed hour**. The numerator is 382.5 unique source seconds, not summed overlapping/padded windows. Timing starts before first window inference and ends after the final durable publication; model load is excluded. Fresh launch to completion was 7.327 s. These are cached internal-SSD, short/composite-file measurements, not external-drive throughput or sustained thermal qualification.

Peak RSS during pure analysis: main **121.80 MiB**, AI helper **441.20 MiB**. Across the full interactive workload: main **130.58 MiB**, AI helper **436.28 MiB**, audio helper **35.94 MiB** (separate process peaks, not a simultaneous total). Vector payload was **151,552 bytes**; catalog plus WAL peaked at **2,743,736 bytes**, sampled every 50 ms and including normal catalog/index overhead. Selected waveform caches totaled 168,944 bytes after the run. Model/runtime storage is separate. No application FIFO starvation was observed at **48 kHz / 512 frames, MacBook Pro Speakers**; that counter is not a driver-underrun, acoustic-latency or human-listening measurement.

## Checks and evidence

All three final CTest suites passed: `library_and_audio`, `library_performance_regression`, `library_semantic_integrity` (16.14 s total). The actual-model workflow also passed: interruption after a durable partial index, resume to 43 healthy recordings / 74 windows, query generations while paused, isolated corrupt/nonfinite-file failure, supervised AI-worker loss/restart, exact UUID lookup, and unchanged reopen reuse. Source interval tests cover 44.1/48/96 kHz, nonaligned exclusive ends, stale source stamps, latest seek generation, model/preprocessing isolation, revision invalidation, offline results, waveform reuse and retry checkpoint correctness.

The native automated controls passed safe pause with pending work, sound query plus audition while paused, resume, long-file range, alternate moment after rescan, waveform readiness, range/context edits, seek, query changes preserving pinned playback, and whole-file switching. Separate CUA operation of the ordinary native UI, including a recheck of the final measured executable, verified the actual model download, folder picker under Documents, anonymous long-file retrieval, numeric edits, waveform click seek, Command-F during playback, text-entry Space, pause/resume and quit. Human sound quality is not inferred from callback progress.

Offline folder installation and native text inference also passed with networking denied. Changing one byte in a disposable pack made both the native helper and installer reject it; no invalid pack was published.

The installed-model replay ran the native app and its children under `(version 1)(allow default)(deny network*)`. A curl probe failed, while all **21 semantic UI queries passed**, with completed vectors reused. This proves per-process offline operation; the host's network was not disabled.

The first native benchmark attempt timed out because its cached UI snapshot did not update progress without a changed result page. Progress now refreshes independently. A subsequent keyword-control assertion compared this live pause state with root counts from an older result page; the control now takes one coherent service snapshot. Its failed run is retained in `keyword-after`; all controls pass in `keyword-final`. Earlier development failures (text graph's folded mask, native tokenizer regex lifetime) were corrected before parity/workflow/benchmark runs. Failed attempts remain in `build/semantic`; they are excluded from successful distributions rather than silently retried into a pass.

Migration of a copied **100,005-record v2 catalog to v3** preserved the exact identity/revision/state/technical-metadata digest and passed SQLite, FTS and foreign-key checks. The disposable HFS+ image test also passed detach/offline retention, remount at a different path, stable UUID and unchanged revision. This simulates removable-volume behavior and does not qualify physical external-drive hardware. Benchmark and migration catalogs are isolated from the user's normal catalog.

The representative preceding-version regression uses the same 100,000-file, 1,022-directory generated keyword fixture as the performance milestone: 90,000 WAV / 5,000 AIFF / 5,000 FLAC, 11 intentionally malformed, 266.637 seconds and 70,428,810 logical bytes. The preserved `758aa09` app and final candidate run sequentially on fresh catalogs, with AI explicitly disabled for this established workload. Its generated tones test Library responsiveness and decoding, not semantic quality.

| Keyword regression | Preserved baseline | Current |
| --- | ---: | ---: |
| During indexing, p50 / p95 / p99 ms, n=200 | 7.63 / 9.61 / 14.55 | 7.70 / 9.42 / 14.74 |
| Warm, p50 / p95 / p99 ms, n=600 | 7.38 / 8.67 / 13.18 | 7.44 / 8.56 / 12.18 |
| During audition, p50 / p95 / p99 ms, n=600 | 7.42 / 8.49 / 10.82 | 7.42 / 8.58 / 8.81 |
| Cached audition callback p95, n=20 | 42.08 ms | 42.67 ms |
| First eligible name → committed batch | 3.827 ms | 2.519 ms |
| Filename stage / files per second | 6.877 s / 14,541 | 7.095 s / 14,094 |
| All headers committed, from benchmark start | 10.578 s | 9.897 s |
| Incremental file close → UI apply p95, n=5 | 1375.72 ms | 1379.88 ms |
| Main peak RSS | 124.56 MiB | 124.59 MiB |
| 35-second reopen: new headers / source stats / entries | 0 / 0 / 0 | 0 / 0 / 0 |

Keyword warm latency remains below the previous 50 ms target; discovery remains above 500 files/s. The modest filename throughput decrease is reported rather than treated as an improvement. Both reopen runs performed 2,044 directory fingerprint checks, zero directory enumerations and zero discovery commits. All native paging/pause/selection controls passed on the candidate. The preserved baseline audition harness can include initial playback startup; the candidate waits for playback before each recorded query. Callback timing remains a separate endpoint. The six-file / 405-second longer generated corpus was fully decoded separately in both runs.

Portable evidence is [verification/local-ai-search.json](verification/local-ai-search.json). Raw files retained in this workspace:

- `build/semantic/native-final/{summary,semantic,semantic-analyze,semantic-reopen,semantic-offline,relevance}.json`, plans/logs and network-denial probe; prior successful `native-2` retained separately.
- `build/semantic/final-artifact-ctest.log`, `final-workflow.log`, `semantic-workflow-output/workflow.json`, `reference/reference-parity.json`.
- `build/semantic/native-ui-observations.json`, `listener-review.html`, corpus `evaluation/manifest.json` and license/provenance files.
- `build/semantic/keyword-before`, `keyword-final`, `migration-v2-v3`, and `volume-final` for preceding-version regression, migration and removable-volume simulation.
- `build/semantic/baseline/baseline.json`, baseline app, initial test failure and synchronized-test rerun; `native-1` timeout and debug logs retained.

## Requirement coverage and limits

| IDs | Covered here | Still partial / deferred |
| --- | --- | --- |
| LIB-01/02/03/04/06 | Existing in-place library retained; progressive semantic stages, selected waveform/analysis priority, pause/resume, errors/retry and durable window resume | Exclusion policies, BWF/iXML/UCS overrides, audio-hour backlog UI, capture-aware scheduling, full unreliable-drive matrix |
| DATA-01/02/03 | Stable IDs, source-revision validation, read-only originals, versioned derivative windows/peaks, transactional migration | Cryptographic source identity, persistent user range annotations, cache quotas/eviction |
| SRCH-01/02/03/05/06 | One field, fast lexical plus true local paired encoders, source filtering, exact lookup, RRF, reasons, bounded generations, overlapping source ranges and grouped moments | Structured metadata/tag filters, feedback preferences; **SRCH-04 search by example remains unimplemented** |
| SRCH-07, AI-04/05 | Incremental durable vectors separated by model/preprocessing; verified install/status/reuse/offline; same-space query/index guard | ANN scale qualification, atomic dual-model rollout/rollback, model update qualification and 200-query/two-listener gate |
| DSP-03/04, UX-01/02/03 | Shared original-sample region API, I/O-free callback, editable waveform/range, optional context, pinned audition, keyboard focus and honest state | Full DSP/plugin latency, VoiceOver human usability and broader audio-device qualification |
| PERF-01/02/03 | Native semantic paint/component timings, cold loads, concurrent queries, actual-window throughput, memory/storage and 21-query annotation proxy | Reference M2, long sustained workload, large semantic corpus, held-out listener judgments |
| OPS-01 | Installed-model semantic operation with network denied | OPS-05/06 distribution license audit/signing/notarization and OPS-07 full hardware/reliability matrix remain open |

Kernel-blocked filesystem calls can still delay safe pause, waveform shutdown or child reaping; workers remain owned, with no abandoned retry threads. Physical USB/Thunderbolt drives, genuinely hung SMB/NFS, sleep/wake cycling, external audio interfaces, older supported macOS, VoiceOver, 256-frame operation and a 30-minute listening/thermal soak were not qualified. The exact vector scan has no 100,000-file semantic performance qualification. The larger **keyword** regression explicitly disables AI to retain its established workload; concurrent AI contention is measured separately above. No Designer, plugin-hosting or Resolve-placement scope is included.

## Reproduce

Exact build, launch, optional pack installation and a five-minute own-library test are in [README](../README.md). Additional repeatable commands, from the repository root:

```sh
# Small deterministic signal/catalog regressions; uses the native audio helper.
.tools/cmake/data/bin/ctest --test-dir build --output-on-failure

# Real sound effects: downloads attributed evaluation-only media, choose a fresh output directory.
python3 scripts/make_semantic_fixture.py build/my-real-effects
build/SoundPlusTests_artefacts/Release/SoundPlusTests --semantic-workflow \
  "$PWD/build/SoundPlus_artefacts/Release/SoundPlus.app/Contents/MacOS/SoundPlus" \
  "$HOME/Library/Application Support/SoundPlus/Models/clap-htsat-unfused-q8-c28f2883575e" \
  "$PWD/build/my-real-effects/media" "$PWD/build/semantic-workflow-output"

# Full native semantic benchmark, region controls, reopen and network-denied replay.
# --output must be fresh; keep the app visible and run one benchmark at a time.
python3 scripts/benchmark_semantic.py \
  --app "$PWD/build/SoundPlus_artefacts/Release/SoundPlus.app/Contents/MacOS/SoundPlus" \
  --model "$HOME/Library/Application Support/SoundPlus/Models/clap-htsat-unfused-q8-c28f2883575e" \
  --fixture build/my-real-effects/evaluation/manifest.json --output build/my-semantic-run
python3 scripts/make_listener_review.py \
  --fixture build/my-real-effects/evaluation/manifest.json --output build/my-listening.html
```

The model workflow test requires its output directory's basename to be `semantic-workflow-output` and recreates only that directory. Existing keyword benchmark and disposable-volume commands remain in README. All raw benchmark paths use this checkout's build directory; the JSON verification record preserves hashes and portable summaries when those generated artifacts are not checked into version control.
