# SoundPlus Source Pools v1.5 — review and next step

Reviewed 19 November 2026. Episode: all projects. Scene: all. Department: SoundPlus development / sound design. Priority: correct two interaction regressions before expanding the feature set. Next action: v1.5.1 Source Pool corrections.

## Decision

**Changes required: fix R18 and R19 before closing v1.5.** The Source Pools architecture, deterministic choices, frozen provenance and multichannel foundation are sound in the reviewed scope. Two reachable interaction defects remain: ordinary pooled Live edits stop playback, and accepting an unchanged mapping review can change the source channels or spatial/LFE settings.

This is a focused correction milestone. Preserve the implementation and UI organization; neither defect warrants a new engine or wholesale redesign.

Reviewed latest HEAD: **`5ab604837cc6d82e123fca6f367f9c773b8f72b6`**, “Add bounded Designer source pools and immutable take provenance.” The review also includes the previously unreviewed parent **`6c3e18a60a8bd088de3aa03c9265782e900379e7`**, which added Library repeat modes and thresholded similarity search after the previous reviewed `6331a72`. [Latest commit](https://github.com/Tekie79/Sound-Plus/commit/5ab604837cc6d82e123fca6f367f9c773b8f72b6) · [Library update](https://github.com/Tekie79/Sound-Plus/commit/6c3e18a60a8bd088de3aa03c9265782e900379e7).

## R18 — P2: normal pooled Live edits stop playback

### Reproduction

1. Use a Mono or Stereo layer in a 5.1 design, enable its Spatial Panner, and add alternatives to the layer.
2. Play Live Design.
3. Drag azimuth, or stereo width for a Stereo input. Ordinary trim-in/trim-out changes expose the same classification problem.
4. The first changed value stops the owned playback even though the selected entry and source have not changed.

### Cause and evidence

`DesignSession::edit()` calls `stop()` when `pooledSourcesDiffer()` returns true. That predicate compares the complete routing serialization, including azimuth and width, plus the range's first/last samples. The actual spatial controls invoke this edit path. Existing `designStructure()` explicitly removes azimuth, width and trim bounds from the structural signature so those controls can use the qualified Live parameter path. The new stop decision happens before that update is scheduled. [Stop branch](https://github.com/Tekie79/Sound-Plus/blob/5ab604837cc6d82e123fca6f367f9c773b8f72b6/src/DesignSession.cpp#L420) · [Overbroad predicate](https://github.com/Tekie79/Sound-Plus/blob/5ab604837cc6d82e123fca6f367f9c773b8f72b6/src/SourcePools.cpp#L416) · [Actual panner callbacks](https://github.com/Tekie79/Sound-Plus/blob/5ab604837cc6d82e123fca6f367f9c773b8f72b6/src/DesignerLayouts.cpp#L453) · [Existing Live signature](https://github.com/Tekie79/Sound-Plus/blob/5ab604837cc6d82e123fca6f367f9c773b8f72b6/src/DesignLive.cpp#L64).

A compiled portable reproduction used the unmodified production predicate and minimal stand-in types/serialization:

| Operation | Predicate says source changed |
| --- | --- |
| No-pool azimuth edit | No |
| Unchanged pooled layer | No |
| Pooled azimuth or stereo-width edit | **Yes** |
| Pooled trim-in or trim-out edit | **Yes** |
| Actual entry switch | Yes |

This proves the decision-logic defect. Source inspection connects it to the UI and transport. It is not a native JUCE application run. The Recording branch returns earlier and is not the path described here; this finding concerns ordinary Live editing.

### Required correction

Distinguish actual source/entry activation, relocation and relevant structural mapping changes from supported continuous edits to the current resolved source. Preserve the intentional stop/reprepare policy for actual source changes. Restore normal azimuth/width/trim playback and Undo/Redo behavior on pooled layers. Do not fix this by bypassing source safety or allowing unrecorded source switches during capture.

Add tests through the real session and control callbacks, asserting unchanged stop count/play generation for ordinary edits, correct parameter adoption, and continued stopping for an actual entry switch.

## R19 — P2: unchanged mapping review changes channels or spatial state

### Reproduction A: custom physical inputs

1. Add a six-channel alternative in a layout-aware design.
2. Review **Custom subset: 1, 4**, interpret it as Stereo, and use an explicit reviewed physical-order mapping.
3. Select it, then reopen **Options → Review input and mapping**.
4. Press **Use source** without changing the controls.

The constructor recognizes a pair from the input count and first index without checking the entire ordered selection. Zero-based `[0,3]` becomes `[0,1]`: physical **1,4 changes to 1,2**. A three-input subset `[0,2,4]` defaults to Complete source for a six-channel file and can become all six inputs. [Preset initialization](https://github.com/Tekie79/Sound-Plus/blob/5ab604837cc6d82e123fca6f367f9c773b8f72b6/src/DesignerPools.cpp#L85).

### Reproduction B: implicit spatial/LFE routing

Create a pool from a layer already using the Spatial Panner with nondefault azimuth, width, divergence and LFE settings. Reopen the initial entry's mapping review and press Use source unchanged.

The existing-routing option is offered only when the explicit route list is nonempty. Implicit spatial routing normally has no such list. Refresh starts with `resolved = {}` and restores the complete previous routing only through that unavailable option. The unchanged Use callback then submits the reconstructed defaults. [Existing-routing option](https://github.com/Tekie79/Sound-Plus/blob/5ab604837cc6d82e123fca6f367f9c773b8f72b6/src/DesignerPools.cpp#L99) · [Routing reconstruction](https://github.com/Tekie79/Sound-Plus/blob/5ab604837cc6d82e123fca6f367f9c773b8f72b6/src/DesignerPools.cpp#L217) · [Use callback](https://github.com/Tekie79/Sound-Plus/blob/5ab604837cc6d82e123fca6f367f9c773b8f72b6/src/DesignerPools.cpp#L364).

The compiled extracted-statement reproduction observed:

| Existing state | Reopened/rebuilt state |
| --- | --- |
| Physical inputs `[0,3]` | `[0,1]` |
| Physical inputs `[0,2,4]` | `[0,1,2,3,4,5]` |
| Spatial mode on | Off |
| Azimuth −130° | 0° |
| LFE send −12 dB | −90 dB |

### Required correction

Initialize the editor from the exact complete reviewed entry. Select Complete source only for the exact ordered full input list; select a pair only for an exact real pair or valid odd final channel; otherwise preserve Custom subset and its order. Preserve implicit routing, explicit matrices, spatial mode/position/width/divergence and LFE policy when their meaning has not been edited.

Accepting an unchanged review of the active entry must preserve its source/routing semantics. Explicit user mapping changes still need validation. Keep unknown-source interpretation and mismatched-bus review requirements intact.

The new regression must instantiate the actual review editor and exercise constructor, refresh and Use. Current pool list/session tests do not cover this complete UI round trip. [Existing pool tests](https://github.com/Tekie79/Sound-Plus/blob/5ab604837cc6d82e123fca6f367f9c773b8f72b6/tests/SourcePoolTests.cpp).

## What validates successfully

The reviewed architecture correctly keeps optional pools separate from ordinary resolved layers and frozen recipes. Selection uses content/header checks, entry signatures, per-layer generations and document/history context. Independent accepted additions remain separate. Seeded resolution is explicit and missing media is not a candidate filter. The selected source is frozen before capture, mutable pools are removed from the recipe, and versioned origin metadata binds back to its source fingerprint, range and routing. [Ownership contract](https://github.com/Tekie79/Sound-Plus/blob/5ab604837cc6d82e123fca6f367f9c773b8f72b6/docs/SOURCE_POOLS_V1_5_CONTRACT.md) · [Freeze/validation code](https://github.com/Tekie79/Sound-Plus/blob/5ab604837cc6d82e123fca6f367f9c773b8f72b6/src/SourcePools.cpp#L452).

No additional blocking defect was confirmed in the reviewed Library Repeat One/All, similarity cutoff/order, asynchronous activation, serialization, frozen-origin or multichannel processing paths. That is a scoped review result, not an assertion that every possible application workflow is qualified.

### Independent work executed here

The review audit completed **286 checks**. Its disposition remains **changes required**, because two checks confirm the defects rather than proving correct application behavior.

- All **199** declared compiled-source file hashes match the current checkout.
- The compact sorted source-manifest digest reproduces **`880dc2c3c24102d047554aeade9db9ec0cfd2f946498e5e67027b4539edf8d8a`** exactly.
- **34** retained evidence files match the reviewed Git commit; compressed JSON/logs were decoded as relevant.
- The recorded **24/24 CTest result in 101.86 seconds** matches the registered suite names. The earlier fresh pad baseline records a 3.61-second pass.
- All eight populated control/pool latency distributions were recomputed from raw samples.
- Actual playing active-end snapshots, timestamped observation counts, excluded diagnostics and reported graph/cache bounds were checked.
- All **20** fixed deterministic choices were recalculated independently from the published integer algorithm.
- Both extracted-source C++ reproductions compiled and ran successfully.

The native Mac suites were not run here. Mac binary hashes were inspected as declared identities; their actual binary bytes are not included. This repository explicitly targets macOS Apple Silicon, whereas this review environment is Linux. [Source/artifact identity](https://github.com/Tekie79/Sound-Plus/blob/5ab604837cc6d82e123fca6f367f9c773b8f72b6/docs/verification/source-pools-v15/artifact-identity.json) · [Recorded full test log](https://github.com/Tekie79/Sound-Plus/blob/5ab604837cc6d82e123fca6f367f9c773b8f72b6/docs/verification/source-pools-v15/ctest-final.log).

## Performance and visual assessment

| Measurement | Final control p95 | Pool run p95 | Pool observations |
| --- | ---: | ---: | ---: |
| Library query → paint | 12.06 ms | 21.24 ms | 30 |
| Library selected waveform | 52.28 ms | 55.27 ms | 30 |
| Library complete viewport | 200.07 ms | 266.49 ms | 30 |
| Alternatives focused preview | — | 85.72 ms | 32 |
| Alternatives complete viewport | — | 252.45 ms | 32 |

The final pool run records zero underruns/restarts, one graph preparation throughout, 11,046/11,046 above-threshold status observations and 177 retained timestamped observations. The two earlier incomplete pool runs are correctly excluded. This improves the evidence over the prior null active-end status. [Raw-result summary](https://github.com/Tekie79/Sound-Plus/blob/5ab604837cc6d82e123fca6f367f9c773b8f72b6/docs/verification/source-pools-v15/pools-summary.json) · [Audio observation record](https://github.com/Tekie79/Sound-Plus/blob/5ab604837cc6d82e123fca6f367f9c773b8f72b6/docs/verification/source-pools-v15/audio-observations.json).

The comparison is bounded: 10,000 Library files, warm Library derivatives, 33 pool entries with 32 newly imported short recordings, mixed cold/warm pool observations, and uncontrolled OS cache state. At the end, semantic processing still had 1,499 files complete and 8,534 pending. This does not qualify completed full-corpus semantic search, maximum capacity, or every long multichannel recording.

Critically, the pool pressure run changes preview focus while the original source keeps playing. It does not activate each source or test normal pooled panner/trim edits. Therefore its green audio counters do not contradict R18.

The retained 1040×680 Designer screenshot shows real compact waveforms in both Browser and Alternatives, the existing timeline, clear channel badges and a separate active-entry checkmark. The contextual disclosure preserves the four-card/creative-canvas organization. Keep that approach. Correct the interaction regressions rather than introducing another global toolbar or decorative waveform pass. [Minimum-size UI evidence](https://github.com/Tekie79/Sound-Plus/blob/5ab604837cc6d82e123fca6f367f9c773b8f72b6/docs/verification/source-pools-v15/edit-minimum.jpg).

## Evidence that cannot be independently recomputed from this commit

The retained native audit reports a five-second, 48 kHz, six-channel, 32-bit output, zero sample error, the chosen Rear source's reviewed mapping to a 5.1 Side bus, a frozen take, Print-back and document reopen. The detailed CTest log separately records consumed capture/replay/offline comparisons. These are useful reported results. [Native audit](https://github.com/Tekie79/Sound-Plus/blob/5ab604837cc6d82e123fca6f367f9c773b8f72b6/docs/verification/source-pools-v15/native-workflow-audit.json).

However, this milestone's committed evidence directory does not contain the underlying new saved `.soundplus` document, generated source WAV, rendered WAV or raw capture/replay bytes. Their local Mac paths are references, not portable evidence. Consequently this review does **not** claim to have independently repeated the new PCM comparison, output-file hash or saved-document checksum validation. Add a compact generated-fixture archive and independent relative-path auditor with v1.5.1.

Working-session restoration to Rain glass design r17543 / Variation 5 is also recorded evidence, not access to the user's live Mac performed here. Physical surround/listening, real external-volume removal/reconnect, cross-volume relocation and native Resolve receiving-project persistence remain open qualifications. [Restoration record](https://github.com/Tekie79/Sound-Plus/blob/5ab604837cc6d82e123fca6f367f9c773b8f72b6/docs/verification/source-pools-v15/restoration.json).

## Next step — v1.5.1

Fix R18 and R19 through the current session/UI architecture. Add focused production-callback regressions and a native minimum/normal-size workflow covering the exact failures. Run the integrated suite once after final fixes and retain exact source/binary identity. Preserve all existing DSP tags, frozen take hashes/provenance, output receipts, multichannel interpretation and the capture boundary.

Include portable generated PCM/document evidence so the next review can recompute the numerical claims. Broader pressure testing is needed only if the fixes touch scheduling/cache behavior or a concrete performance risk remains; do exercise pooled Live panning/trim under background activity, since that is the missing workflow.

Use `Codex_v1_5_1_Source_Pool_Corrections.md` for the implementation handoff. Live sampler triggering, plugin hosting and Resolve automation remain separate feature milestones after these corrections. This review changed no tracked application source.
