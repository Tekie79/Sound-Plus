# Codex prompt — SoundPlus v1.4.1

Paste this complete prompt into Codex in the Sound-Plus repository. The review is useful supporting material but is not required to understand the task.

---

Implement **SoundPlus v1.4.1 — Multichannel Correctness and Persistence**. Complete the seven concrete findings below before implementing Source Pools. Preserve the working mono/stereo/5.1/other named/discrete workflow, small waveform previews, per-channel detail, current Lucide/theme UI and progressive disclosure.

## 1. Starting point and working rules

Read the current AGENTS.md, this entire prompt, `docs/MULTICHANNEL_V1_4.md`, the v1.4 report and applicable requirements before editing. Inspect git status and actual HEAD. This review used `6364dfdeda2a60388b7fd6c1cc52a726be69a5c4`, over `3609486e6b1313f2ad80181022c04e0db372657f`. The reviewed tested source commit is `b4fac3d0bbac4fa5d53907356a60a215ade40f82`. Inspect and preserve later correct changes if HEAD has advanced.

Show a compact monochrome checklist with ◉, ○ and ✓; persist it in docs and update it at meaningful milestones. Protect the actual latest user session/catalog/settings/media/renders. Use separate generated fixtures and QA catalogs. Do not restore old QA documents over newer working edits. Close running app/helper/test processes before rebuilding their bundle.

R5–R9 and the symmetric R10 context correction are closed in the reviewed paths. Preserve their guards and regressions. This task changes application code and tests, not just documentation. Finish with reviewable local commits and a concise evidence-based completion report. Do not push unless authorized.

## 2. R11 — Clean up deleted-take output preferences transactionally

Current locations: `DesignTakeSession.cpp:204–209,253–265`; `DesignModel.cpp:290–295,443–450`.

`chooseOutput()` stores an output preference keyed by take ID. Deleting a take leaves its key. Loading rejects more than 17 entries, and saving verifies its staging file by loading it. Therefore 16 take preferences + Live, followed by deleting/replacing one take and choosing its output, creates 18 entries with only 16 current takes and blocks save/autosave.

Remove the deleted take's preference in the same undoable transaction. Undo restores both; Redo removes both. Keep finalized output receipts/files durable and independent of take-preference cleanup.

Make validation consistent across edit, save and load. Keys must be Live or extant take IDs, and current preferences must fit the current-context bound. Add a narrow recovery path for already affected documents with dangling preference keys; prune only invalid preference metadata, not takes, receipts or files. Preserve the existing document size and corruption guards.

Regression: more than 17 lifetime take IDs while never exceeding 16 current takes; explicit output selection for each; Live preference; Delete/Undo/Redo; autosave, explicit save and reopen. Confirm that the previous valid save remains protected on unrelated write failure.

## 3. R12 — Correct spatial periodicity and version its processing behavior

Current locations: `ChannelLayout.cpp:293–308`; `DesignAudio.cpp:530–535`.

Shortest-path smoothing accumulates unwrapped angles. The expression `fmod(degrees + 540, 360) - 180` fails to normalize sufficiently negative angles. The ring search can return all-zero gains.

Reachable case: mono point on 5.1 Side, divergence 0, LFE off. Enter/jump through these valid UI targets with each settled beyond 10 ms:

`0, -90, -180, 90, 0, -90, -180, 90, 0, -90, -130`

The internal settled targets are:

`0, -90, -180, -270, -360, -450, -540, -630, -720, -810, -850`

At the reviewed HEAD, −130° gives unit full-range energy, while the equivalent −850° gives zero. Correct the normalization for arbitrary finite accumulated angles while preserving shortest-path smoothing and expected speaker geometry.

Before changing the DSP, define the compatibility policy. A repair changes PCM for affected existing `stock-layout-takes-v1` recordings. Introduce explicit processing/version dispatch as necessary: correct new live processing/new takes, preserve old frozen recipe/receipt semantics, and offer an explicit corrected copy/migration if needed. Do not silently reinterpret an old frozen take under its unchanged processing identity. Legacy stereo contracts remain unchanged. Do not clear/recompute old hashes to hide incompatibility.

Tests must include many positive and negative revolutions, periodicity `g(a)==g(a+360*n)`, nonzero/unit-energy and LFE-exclusion invariants, boundary continuity, mono and stereo images, zero/nonzero divergence and real graph parameter adoption. Record/replay/render the reachable sequence and compare against independent expected energy/channel behavior, not only parity between two implementations that could share the bug. Include old-version fixture behavior and explicit new-version identity.

## 4. R13 — Bind routing editors to the selected route

Current location: `DesignerLayouts.cpp:366–395`, plus panel refresh/selection wiring.

The panel applies trim, polarity and mute together but never loads the current selected route into those editors. Reopening a −6 dB inverted route shows defaults; toggling only Mute then Set route overwrites it to 0 dB/non-inverted.

Load the selected route's actual trim/polarity/mute on construction and input/output selection. Define how an implicit role route or absent route appears; do not accidentally materialize a new unity route merely by inspecting it. Synchronize after Apply, Undo/Redo and external model changes. Preserve deliberate uncommitted editor input until its context changes, then rebind safely. Never apply stale editor values to a different layer or route.

Keep the UI compact. This is a state-binding correction, not a cosmetic redesign.

Tests: two routes with different trims/polarities/mute states; close/reopen/reselect; edit one attribute at a time; implicit/default/absent routes; layer/context changes; Undo/Redo. Assert both displayed values and actual resulting routing/PCM.

## 5. R14 — Invalidate incompatible source-layout overrides on replacement

Current locations: `Catalog.cpp:453–457,515–558`; `LayoutDesign.cpp:61–62,112–120`.

Changed-file discovery retains layout_override. Metadata refresh updates channel count/detected layout but never revalidates that prior override. A six-channel file explicitly interpreted as 5.1 can be replaced at the same path by stereo media and become Ready with a stale six-role override. New Designer Add and full-layout audition then fail.

Define a revision-aware override retention/reconfirmation policy. Incompatible channel counts must never remain active. Distinguish changed content from ordinary rename and verified same-byte relocation. Guard delayed override updates with source ID/revision. Expose a clear needs-review state or safe detected-layout fallback when an interpretation is invalidated.

Correct catalog behavior for future operations without rewriting the interpretation frozen into existing layers/takes. Those retain their source-identity/change handling.

Tests: 6→2, 2→6, same-count replacement, unchanged reopen, rename, verified same-byte relocation and asynchronous stale override edits. Exercise the actual indexing/metadata path, Library display/audition and Designer Add. Do not prove this solely with setLayoutOverride's initial count check.

## 6. R15 — Keep audition layout bound to its actual owner

Current locations: `LibraryView.cpp:410–425,1698–1714`.

The global audition-channel handler takes the override from selected(), then replays playingId/playingPath. Those may belong to the pinned reference while selected() is another candidate.

Repro: pin an unknown six-channel reference with a valid 5.1 override; select a candidate with no override; play the reference in pair/analysis mode; switch to Layout→Stereo monitor. The reference loses its interpretation and fails. A differently sized candidate override causes a count mismatch.

Resolve layout/override from the actual audition owner and revision. Preserve path, range and source identity when switching audition modes. A catalog interpretation change may update only a matching source stream. Do not steal playback ownership from Designer or another protected context.

Tests: reference versus candidate owner; candidate selected while reference plays; empty/same-sized/different-sized overrides; pair/analysis/layout transitions; active/pending playback; source revision change; stopped selection remaining silent. Assert the actual replayed path/spec and heard channel mapping, not just labels.

## 7. R16 — Preserve legacy source interpretation during layout conversion

Current locations: `LayoutDesign.cpp:35–80`; historical conversion tests near `MultichannelTests.cpp:1344`.

setMixLayout passes preserveLegacySelection=true, but initialiseRouting honors it only above 16 source channels. For 1–16 channels it selects all inputs. A legacy six-channel layer using pair 1 (physical channels 2/3, zero-based) becomes all six inputs and can fail unknown→named conversion. Legacy stereo Analysis Mix becomes ordinary stereo instead of the arithmetic mono mean.

Preserve the selected pair or analysis interpretation when converting existing layers. Handle non-first pairs and odd final channels correctly. If a legacy interpretation cannot be represented in the admitted new graph, require an explicit choice and leave the old mix unchanged until conversion succeeds. Conversion must be undoable and transactional across the mix.

Do not change the intentional new Library Add behavior of importing a complete supported source. This finding concerns existing legacy-layer conversion.

Tests: legacy mono/stereo; analysis mean including anti-phase channels; non-first pairs from 4/6/16-channel sources; odd final channels; wide-source pair/analysis cases; known/unknown metadata; Stereo/5.1 targets. Assert physical input indices, actual converted PCM, Undo/save/reopen and unchanged existing take hashes/receipts. Checking only frozen take hashes is insufficient.

## 8. R17 — Persist legacy split-output identity independently of DSP layout

Current locations: `DesignModel.cpp:95–109,256–260`; `DesignerView.cpp:1965–1974`; split rendering in `DesignAudio.cpp`; `LayoutDesign.cpp:4–22`.

The render UI permits Split on an unconverted legacy design/take. However, splitMono is serialized only if the export layout string is nonempty, and output files are serialized only if channelProvenance exists. A legacy split render can therefore lose its setting and stem membership on save/reopen. Delivery falls back to Mix.wav, and missing-stem validation disappears.

Persist split settings, set membership and selected finalized-output identity independently of whether audio processing is legacy. Preserve old canonical recipes and legacy PCM. Metadata extensions must use the appropriate compatibility/version gate without injecting new fields into old frozen recipe hashes.

If a genuinely unsupported legacy combination cannot be implemented, disable/reject it before rendering with a precise explanation. Never successfully create a split set that loses identity on the next save. Prefer completing the existing exposed workflow.

Tests must start from a mandatory historical fixture and use actual render-panel callbacks. Render split and interleaved outputs; save/reopen; verify every stem role/path/hash, selected output, ready drag file list, Print master and missing-member invalidation. Cover Undo/Redo and preserve existing take/output hashes. A new-layout-only test does not close this finding.

## 9. Verification and qualification gates

Run focused regressions first, then all existing 21 CTest suites plus new coverage. These changes cross persistence, source identity, routing and audio, so the full suite is warranted. Use fresh isolated fixtures; never mutate real user media to reproduce replacement cases.

Perform native normal/minimum UI workflows for route editing, pinned-reference mode changes, replacement/recovery, legacy conversion, split output and repeated take deletion/replacement. Retain small waveforms, per-channel detail, correct mono/stereo/5.1/discrete identities and current 10-point fade controls. Keep the callback free of allocation, parsing, file/database I/O and graph preparation.

For the panner, record enough repeated motion to exercise the failing wraps and verify per-channel expected signal, legacy/new processing dispatch and offline parity. Re-run focused search/waveform/indexing pressure only as needed to qualify the changed paths; do not substitute broad assertion counts for named scenarios.

The reviewed archive reports 21/21 tests in 84.22 s. An independent review verified 61 artifact hashes, 70 viewport records and 35,328 six-channel consumed-prefix frames. Those are historical baseline records, not evidence for this new patch. Produce fresh records tied to the new source/binary identity.

Keep these claims separate:
- Automated/source-derived checks.
- Native UI actions.
- PCM comparisons.
- Physical hardware mapping/unplug.
- Human listening.
- Native receiver drag and persistence.

Resolve API import/placement/save/reopen has separate recorded evidence; native SoundPlus→Resolve drag remains unqualified. A correctly targeted native drop, actual imported item/channel mapping, timeline placement, save, SoundPlus exit and receiver reopen are needed to close that gate. If unavailable, report the precise blocked step. Do not treat the prior mis-targeted gesture as proof of Resolve rejection or modify drag code speculatively.

Physical surround/unplug and human listening remain separate open qualifications. Six below-threshold observations in the baseline capture lack individual timestamps; do not dismiss or retroactively diagnose them without new evidence. Correcting the panner does not establish the cause of those historical observations.

Deliver working fixes, targeted tests, fresh complete-suite log, source/artifact identities, real UI evidence and a finding-by-finding closure table. Reopen the latest actual user session and verify its preserved identity. Commit completed work locally in reviewable groups.

## 10. Next feature milestone after closure

Keep Source Pools as v1.5. Update its brief to reference the corrected source/routing/persistence contracts, but do not implement the feature in this patch.

Its first slice remains: optional bounded pool per layer; stable entry/source IDs; entry-specific range, detected layout/override, physical channels and reviewed mapping; lazy small waveforms; explicit Next/Previous; independent source-choice lock; deterministic seed/algorithm/version/resolved-entry persistence. Single-source layers stay the default.

Inactive unavailable alternatives must not block a healthy selected entry. Missing selected content must not silently reroll. Freeze the resolved source/mapping before Arming; later pool edits cannot alter a take. Keep source switching disabled during Arming/Recording/Finalizing until a separate bounded source/voice event model supports it.

Do not add plugin hosting, object/Ambisonic delivery or video/Resolve automation in this correction milestone. Complete R11–R17 and their gates first.
