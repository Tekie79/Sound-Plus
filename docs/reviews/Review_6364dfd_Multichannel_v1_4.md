# SoundPlus v1.4 review — corrections before Source Pools

## Decision

**R10 is closed and the multichannel foundation is substantially implemented. Seven concrete correctness defects remain. The next milestone should be v1.4.1 — Multichannel Correctness and Persistence, followed by v1.5 — Source Pools and Repeatable Variations.**

Reviewed HEAD: [`6364dfdeda2a60388b7fd6c1cc52a726be69a5c4`](https://github.com/Tekie79/Sound-Plus/commit/6364dfdeda2a60388b7fd6c1cc52a726be69a5c4). Baseline: `3609486e6b1313f2ad80181022c04e0db372657f`. The declared tested source commit is `b4fac3d0bbac4fa5d53907356a60a215ade40f82`; HEAD adds README/documentation/evidence changes, without subsequent application-code changes.

The full supplied completion report, repository instructions, multichannel contract, relevant implementation/tests, evidence records and screenshots were reviewed. Application source was not modified. Native macOS builds, UI interaction, hardware and listening were not rerun here. This Linux review independently checked committed artifacts/PCM and performed portable numerical, SQL and state reproductions for selected findings.

## Findings requiring correction

All seven are P2 functional issues with concrete triggering conditions. Prioritize the save/autosave failure and audio/routing errors first. Passing existing tests does not cover the missing scenarios below.

### R11 — Deleted-take output preferences eventually block saving

**Locations:** [`chooseOutput/deleteTake`](https://github.com/Tekie79/Sound-Plus/blob/6364dfdeda2a60388b7fd6c1cc52a726be69a5c4/src/DesignTakeSession.cpp#L204), [17-entry loader bound](https://github.com/Tekie79/Sound-Plus/blob/6364dfdeda2a60388b7fd6c1cc52a726be69a5c4/src/DesignModel.cpp#L290), [staged-save reload](https://github.com/Tekie79/Sound-Plus/blob/6364dfdeda2a60388b7fd6c1cc52a726be69a5c4/src/DesignModel.cpp#L443).

`chooseOutput()` stores a preference keyed by take ID. `deleteTake()` removes the take but retains that preference. The loader rejects more than 17 preferences, while editing validation does not enforce or repair this invariant.

**Repro:** choose finalized outputs for 16 takes and Live Design, delete one take, then create/render/select its replacement. There are still 16 current takes, but 18 preference entries. Save/autosave writes a staging file and fails its verification reload with “Too many output selections.” The previous valid save remains protected; current work cannot be saved through this path.

**Correction:** remove a deleted take's preference in the same undoable transaction. Undo restores both. Make key/bound validation consistent at editing, save and load boundaries. A narrowly scoped cleanup of dangling preference keys can recover affected metadata without changing takes, receipts or durable files.

**Regression:** exceed 17 lifetime take IDs while never exceeding 16 current takes. Cover explicit save, autosave, reopen, deletion, Undo and Redo. The current single Delete→Undo test does not exercise accumulation.

### R12 — Repeated negative panner rotations can mute a source

**Locations:** [angle normalization](https://github.com/Tekie79/Sound-Plus/blob/6364dfdeda2a60388b7fd6c1cc52a726be69a5c4/src/ChannelLayout.cpp#L293), [shortest-path accumulation](https://github.com/Tekie79/Sound-Plus/blob/6364dfdeda2a60388b7fd6c1cc52a726be69a5c4/src/DesignAudio.cpp#L530).

The smoothed internal azimuth deliberately accumulates revolutions. However, `fmod(degrees + 540, 360) - 180` does not normalize sufficiently negative inputs into the intended interval. The speaker-ring search then fails to find a segment.

**Repro:** on a mono point routed to 5.1 Side, with divergence zero and LFE off, enter/jump through these valid targets while allowing each to settle beyond the 10 ms smoothing interval:

`0, -90, -180, 90, 0, -90, -180, 90, 0, -90, -130`

The internal angles become `0, -90, -180, -270, -360, -450, -540, -630, -720, -810, -850`. A portable reproduction of the actual equations gives full-range gain energy **1.0 at −130° but 0.0 at the equivalent −850°**. With no LFE send, the latter is silence.

This affects live movement and recorded spatial events. Matching replay and offline output alone cannot catch it because both can reproduce the same incorrect silence.

**Correction:** use periodic normalization valid for arbitrary finite accumulated angles. Retain smooth shortest-path movement. Decide the processing-version policy explicitly: repairing the equation changes PCM for affected existing `stock-layout-takes-v1` recordings. Preserve their declared immutable behavior or offer an explicit corrected copy; do not silently change the processing meaning under an unchanged identity.

**Regression:** many positive/negative revolutions, periodicity, nonzero/unit energy, LFE exclusion, mono/stereo-image motion, divergence, real graph adoption, and capture/replay/render against an independent expected-energy oracle. Existing tests cover the principal angle interval and a single wrap.

### R13 — Editing a route can overwrite its existing trim and polarity

**Location:** [route editor application](https://github.com/Tekie79/Sound-Plus/blob/6364dfdeda2a60388b7fd6c1cc52a726be69a5c4/src/DesignerLayouts.cpp#L366).

The panel writes trim, polarity and mute together, but does not load the selected route's saved values into those controls. Input/output selection lacks synchronization handlers; refresh updates spatial controls and the matrix picture, not the route editor values.

**Repro:** create a −6 dB, inverted route. Reopen the panel and select it. Toggle only Mute, then Set route. The route becomes 0 dB, non-inverted and muted because the other controls retained defaults. Unmuting later exposes the wrong gain/polarity.

**Correction:** populate the selected route's actual values, including effective default-role coefficients where applicable. Synchronize on route selection, reopen, Undo/Redo and external changes without overwriting deliberately uncommitted input.

**Regression:** reopen/reselect routes with different settings and change one attribute at a time. Test absent routes, implicit role routes, explicit muted/inverted routes and Undo/Redo. Existing tests fill all fields explicitly before applying, so they miss this failure.

### R14 — Source replacement can retain an incompatible interpretation override

**Locations:** [changed-file discovery](https://github.com/Tekie79/Sound-Plus/blob/6364dfdeda2a60388b7fd6c1cc52a726be69a5c4/src/Catalog.cpp#L453), [metadata refresh](https://github.com/Tekie79/Sound-Plus/blob/6364dfdeda2a60388b7fd6c1cc52a726be69a5c4/src/Catalog.cpp#L534), [layer validation](https://github.com/Tekie79/Sound-Plus/blob/6364dfdeda2a60388b7fd6c1cc52a726be69a5c4/src/LayoutDesign.cpp#L112).

Changed-file discovery retains `layout_override`; header refresh changes detected layout/count without revalidating that override. The count check only occurs when the override is initially set.

**Repro:** explicitly interpret a six-channel file as 5.1, then replace the file at the same catalog path with stereo media. After reindexing, the asset is Ready with two channels and a six-role override. New Designer layers prefer the stale override and fail validation; full-layout audition also rejects it. The portable SQL lifecycle reproduction retained exactly this mismatch.

**Correction:** make override validity revision-aware. Incompatible channel counts cannot remain active. Define retention/reconfirmation for changed content, verified same-byte relocation and ordinary rename. Clear or mark only the catalog override as needing review; never mutate existing frozen layer/take interpretations. Guard delayed override writes with source ID/revision.

**Regression:** 6→2, 2→6, same-count changed content, unchanged reopen, rename and verified relocation, including asynchronous stale override edits.

### R15 — Audition-mode changes can use the candidate's layout for a pinned reference

**Locations:** [mode-change handler](https://github.com/Tekie79/Sound-Plus/blob/6364dfdeda2a60388b7fd6c1cc52a726be69a5c4/src/LibraryView.cpp#L410), [reference playback setup](https://github.com/Tekie79/Sound-Plus/blob/6364dfdeda2a60388b7fd6c1cc52a726be69a5c4/src/LibraryView.cpp#L1698).

The handler updates `playingSpec.sourceLayoutOverride` from `selected()`, then replays `playingId/playingPath`. Those playing fields can belong to the pinned reference while the selected row is another candidate.

**Repro:** pin an unknown six-channel reference with a 5.1 override. Select another candidate without an override. Play the reference in pair/analysis mode, then switch to Layout→Stereo monitor. The candidate's empty override replaces the reference's valid interpretation, causing unknown-layout rejection. A differently sized candidate override instead causes an interpretation/count mismatch.

**Correction:** resolve channel interpretation from the actual audition owner and revision. A catalog override edit may update only its matching stream. Preserve the reference's path/range/identity when switching audition mode.

**Regression:** pinned-reference and candidate playback with differing selections/overrides, all audition modes, pending playback and stopped selection behavior.

### R16 — Legacy layout conversion discards existing source-channel selection

**Location:** [`initialiseRouting/setMixLayout`](https://github.com/Tekie79/Sound-Plus/blob/6364dfdeda2a60388b7fd6c1cc52a726be69a5c4/src/LayoutDesign.cpp#L35).

`setMixLayout()` asks to preserve legacy selection, but that flag is honored only for sources wider than 16 channels. For 1–16-channel sources, conversion selects every physical input.

**Repro:** load a legacy six-channel source layer with `channelPair=1` (zero-based physical channels 2/3). Choose a new Design layout. The selection becomes all six channels. With legacy unknown metadata, named-bus preparation fails; with known metadata, audio becomes a full bed/downmix instead of the chosen pair. A two-channel legacy Analysis Mix also changes from arithmetic mono mean to ordinary stereo.

**Correction:** preserve pair/analysis semantics during conversion, including odd final channels. If an interpretation cannot be represented, require an explicit choice and leave the old mix unchanged until that choice succeeds. New Library Add intentionally importing a full supported source is a separate behavior and should remain intact.

**Regression:** legacy mono/stereo, analysis mean, non-first pairs of 4/6/16 channels, odd/wide sources, actual converted PCM, Undo/save/reopen and unchanged frozen take identities. Existing historical tests verify old take hashes after Live conversion, not converted Live channel mapping.

### R17 — Legacy split exports lose their stem identities after reopening

**Locations:** [render settings serialization](https://github.com/Tekie79/Sound-Plus/blob/6364dfdeda2a60388b7fd6c1cc52a726be69a5c4/src/DesignModel.cpp#L95), [receipt serialization](https://github.com/Tekie79/Sound-Plus/blob/6364dfdeda2a60388b7fd6c1cc52a726be69a5c4/src/DesignModel.cpp#L256), [Split UI](https://github.com/Tekie79/Sound-Plus/blob/6364dfdeda2a60388b7fd6c1cc52a726be69a5c4/src/DesignerView.cpp#L1965).

Split export is available for an unconverted legacy design/take. But `splitMono` is serialized only when the export layout string is nonempty, and stem membership is serialized only when channel provenance exists. A legacy render can generate stems without that new provenance.

**Repro:** open a historical legacy design, keep Legacy mono/stereo export, enable Split and render. Save/reopen. The split setting and stem list disappear; `readyFiles()` returns only `Mix.wav`. Delivery sends the master instead of the stems, and missing-stem detection no longer has a persisted membership list. The WAV files themselves are not deleted.

**Correction:** persist output-set membership/settings independently of legacy audio processing. Preserve legacy PCM/canonical take identity. If the combination cannot be supported, disable/reject it before rendering rather than generating an output that loses its identity on save.

**Regression:** actual legacy render-panel callbacks, interleaved/split outputs, save/reopen, selected output, stem roles/paths/hashes, drag file list, Print master, missing-member invalidation and Undo/Redo. Existing split tests use new layout-aware designs.

## What is successfully implemented

R10's symmetric context fix is present both in `changed()` and before preparation completion can publish playback. Controlled tests cover active/pending Live↔Take and Take A↔B, and native-helper records check actual ownership/nonzero PCM. [Session guard](https://github.com/Tekie79/Sound-Plus/blob/6364dfdeda2a60388b7fd6c1cc52a726be69a5c4/src/DesignSession.cpp#L321), [native regression](https://github.com/Tekie79/Sound-Plus/blob/6364dfdeda2a60388b7fd6c1cc52a726be69a5c4/tests/MultichannelTests.cpp#L1216).

The code now has explicit ordered channel contracts, generalized buffers/effect histories, separate monitoring, spatial events, channel-aware rendering and split-output publication. Existing immutable-take/source/finalization guards remain. This is substantially more than an export-menu change. The reviewed paths revealed no additional high-confidence capture-clock/finalization defect.

The current screenshots show small waveforms, layout badges, channel detail, compact meters and contextual advanced controls at normal/minimum sizes. Retain this visual direction; the needed UI work is accurate editor state and identity, rather than another cosmetic redesign. [Normal Designer](https://github.com/Tekie79/Sound-Plus/blob/6364dfdeda2a60388b7fd6c1cc52a726be69a5c4/docs/verification/multichannel-v14/designer-final-normal.png), [16-channel minimum layout](https://github.com/Tekie79/Sound-Plus/blob/6364dfdeda2a60388b7fd6c1cc52a726be69a5c4/docs/verification/multichannel-v14/designer-final-discrete16-minimum.png), [per-channel detail](https://github.com/Tekie79/Sound-Plus/blob/6364dfdeda2a60388b7fd6c1cc52a726be69a5c4/docs/verification/multichannel-v14/pressure-release-live.png).

## Independent evidence audit

The accompanying `Review_6364dfd_Evidence_Audit.json` records **65 passing consistency checks**:

- All **61 manifest files** match size and SHA-256.
- The committed log matches **21 currently registered suites**, all passing in **84.22 seconds**. These were not rerun on this Linux host.
- Six latency distributions were recomputed from raw nearest-rank samples; **70 viewport records** account for visible rows.
- The focused six-channel/eight-layer capture/replay records retain stable helper/process/play/graph/preparation identities and report zero interval underruns/helper-health events.
- Six saved document payload checksums were recalculated from their exact stored canonical text. Frozen capture/replay fields and historical take identities remain consistent in these fixtures.
- **35,328 six-channel float32 frames at 48 kHz** from the committed native consumed prefixes were directly compared and are byte-identical. Their sizes, decompressed hashes and finite samples agree. LFE is silent in this particular fixture; it is not an independent nonzero-LFE capture test.
- The corpus manifest reproduces **10,000 paths, 133 distinct content hashes and 1,253,073,116 bytes**.
- Three portable source-derived reproductions confirm the panner periodicity failure, stale override lifecycle and output-preference overflow condition. These are not native JUCE/UI executions.

| Focused measurement | Capture | Replay |
| --- | ---: | ---: |
| Active interval | 60.001 s | 45.051 s |
| Query-to-paint p95 | 13.09 ms | 10.23 ms |
| Selected waveform p95 | 56.21 ms | 90.85 ms |
| Whole viewport p95 | 203.55 ms | 225.95 ms |
| Applied capture events / replay metadata edits | 2,316 events | 180 edits |
| Below-threshold status observations | 6 / 22,798 | 0 / 12,790 |

Sources: [committed measurements](https://github.com/Tekie79/Sound-Plus/blob/6364dfdeda2a60388b7fd6c1cc52a726be69a5c4/docs/verification/multichannel-v14/measurements.json), [CTest log](https://github.com/Tekie79/Sound-Plus/blob/6364dfdeda2a60388b7fd6c1cc52a726be69a5c4/docs/verification/multichannel-v14/final-ctest.log), [native PCM extent](https://github.com/Tekie79/Sound-Plus/blob/6364dfdeda2a60388b7fd6c1cc52a726be69a5c4/docs/verification/multichannel-v14/native-pcm-prefix.json).

The six capture observations were not individually timestamped, so initial pre-consumption status cannot definitively explain all of them. Original Mac binaries, source media and full rendered WAVs were unavailable for fresh execution/rehashing here; those records were checked for consistency. The retained generated corpus is an I/O workload, not 10,000 distinct commercial effects. No maximum-load/hardware/listening extrapolation is justified.

## Delivery and qualification status

Resolve API import, 5.1 placement, save, SoundPlus exit and project reopen are recorded as passed for three exact files. **Native SoundPlus→Resolve drag remains unqualified.** The reported gesture did not establish a correctly targeted receiving drop; it does not prove Resolve rejected one. Preserve this distinction. [Receiver record](https://github.com/Tekie79/Sound-Plus/blob/6364dfdeda2a60388b7fd6c1cc52a726be69a5c4/docs/verification/multichannel-v14/resolve-api-reopen.json).

Physical surround monitoring, physical unplug and human listening remain unqualified. Library full-layout audition is explicitly a stereo-monitor downmix; physical multichannel assignments are exposed for Designer. The supported operation range and evidence do not imply universal physical-device qualification.

## Next milestones

### v1.4.1 — Multichannel Correctness and Persistence

Implement R11–R17 in bounded commits. Preserve current mono/stereo/5.1/other named/discrete features, R5–R10 guards, source files, latest working session and durable outputs. Add the missing targeted regressions, run the full existing suite because these fixes cross persistence/audio/UI, and qualify focused native workflows. Make the panner compatibility decision explicit before changing its math.

Do not expand into Source Pools during this correction milestone. Its entry/layout selection would depend directly on the catalog, audition, routing and persistence behavior being corrected.

### v1.5 — Source Pools and Repeatable Variations

After the correction gates pass, implement optional bounded alternatives per layer, stable entry/source IDs, source layout/range/physical-channel mapping, lazy small waveforms, explicit Next/Previous, independent source-choice locks and seeded reproducibility. Single-source layers remain the default.

Resolve and persist selected identity/mapping before playback or Record. Inactive unavailable alternatives must not block a healthy selected entry; missing selected content must not silently reroll. A take freezes the resolved source/mapping and origin provenance. Later pool changes cannot alter its audio. Keep source switching disabled during Arming/Recording/Finalizing until a separate bounded source/voice event model is implemented.

Continue native receiver delivery and physical monitoring/listening qualification separately with appropriate hardware. Their open status must not be replaced with API or offline test claims.
