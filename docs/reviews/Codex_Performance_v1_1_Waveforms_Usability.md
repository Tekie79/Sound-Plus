# Codex prompt — Performance Mixer v1.1: waveforms and usability

Implement the next SoundPlus milestone: correct the two reviewed performance interactions, add real small waveforms for audio sources, and make Designer/Perform simple, modern, advanced and user friendly.

## Start and scope

Read the complete prompt, current repository `AGENTS.md`, the companion `Review_e6ca748_Performance_Waveforms.md`, and applicable architecture/verification documents before implementation. Inspect current HEAD and local changes. The review baseline is `e6ca7484830d6bfc8d3c95a22b7b8b6e889d1e6f`; preserve newer work and do not reset to that commit. Verify whether either reported issue has already been corrected.

Maintain the repository's live monochrome task checklist in chat and `docs/`. Protect the latest working design and source media. Use separate QA documents/catalogs and generated fixtures. Close running bundle processes before rebuilding. Preserve unrelated changes.

Use the existing JUCE/C++ architecture, shared document/session/audio/render paths, Lucide registry and theme. Keep Edit as the precise timeline editor and Perform as the direct creative surface. Keep the user's current fixed 10-logical-point fade handles at clip corners, with subdued two-tone accent fill and stable IN/OUT direction. Preserve trim, move, fade-curve editing, slip, selection and playhead/scrub operations.

Deliver the work in this order: (1) functional fixes; (2) four Perform source cards and usability; (3) reuse the compact waveform renderer for visible Library/Browser file rows with bounded scheduling. Keep each slice reviewable. If the final row slice exposes a larger dependency, finish and verify the first two slices and report the exact remaining dependency without calling the broader per-file rollout complete.

## 1. Correct blending with empty slots

Inspect `PerformanceState.cpp::blendWeights`. With TL, TR and BL occupied and BR empty, the current source formula produces weights `(0,1,0)` at `(1,.999)`, equal weights `(1/sqrt(3),1/sqrt(3),1/sqrt(3))` at `(1,1)`, and `(0,0,1)` at `(.999,1)`. Two assigned upper slots also jump at the bottom edge. This is a reproducible state discontinuity, not a claim of a listening test.

Define a deterministic, continuous interpolation for sparse assignments over the whole pad. Preserve zero-slot no-op, one-slot unity, nonnegative finite weights, the intended power normalization, unaffected unassigned layers, and stable ID ownership. A change only to the exact zero-energy fallback cannot reconcile the current direction-dependent limits. Do not use the last gesture position or last valid weights as hidden state.

Document the interpolation choice. Consider geometrical interpolation over occupied corners if appropriate; the implementation is free to choose another law that meets the behavior. Preserve the existing all-four-slot behavior where practical.

Handle compatibility deliberately. Existing saved documents and snapshots must not silently produce a different mix on open. Implement a versioned legacy/new-law distinction or an explicit recoverable migration, and update format gates/tests if required. Existing durable WAVs remain intact. Keep the same resolver for live playback, static snapshots, accurate preview, full render and selected-layer render.

Add meaningful continuity tests for all 16 occupancy masks, exact corners/edges, and approaches from multiple directions with decreasing distances. Check bypass, one-slot behavior, saved/reopened state, deterministic render parity, and relevant live transitions. Perform a native pad/keyboard/numeric reproduction and listen on the actual available output, reporting listening separately from automated checks.

## 2. Correct slider Escape cancellation

Inspect macro callbacks around `PerformView.cpp:237–246`, X/Y callbacks around `135–154`, and `endControl`/`setPad` around `349–394` at the baseline. After `endControl(true)`, both guard flags are false, so a value callback from continued pointer movement can edit again after cancellation.

Introduce a robust cancelled-until-release state, or equivalent lifecycle fix. Escape must immediately restore the original gesture state and suppress subsequent events from that same held-pointer gesture. Release must not commit it. A fresh drag must work, and keyboard/numeric edits outside a drag must still work. Preserve stale document/variation guards.

Add a failing-before/passing-after test sequence for every macro slider and both X/Y sliders: begin drag, change, Escape, continue changing before release, release, verify original values and no cancelled edit in history, then verify a fresh gesture and undo/redo. Include document/variation switching while held. Exercise actual handlers/callbacks rather than testing only a new boolean in isolation. Verify the same sequence in the native application.

## 3. Real compact source waveforms

Create a reusable lightweight waveform renderer and a source-card component. Every occupied Perform assignment must show:

- One useful title and choose/replace affordance. Preserve a custom alias; show the underlying filename separately only when it differs. Do not repeat identical filenames in two adjacent lines. Keep full names available via tooltips/accessibility.
- A real waveform approximately 24–32 logical points high, using the layer's source and selected `[startSample,endSample)` range. Distinct layers using the same file may have different ranges and remain distinct cards.
- A concise duration hint reflecting the source rate and linked playback rate. Represent reverse consistently. Treat the waveform as a source overview before fades, gain, inserts and macro processing, not as a post-DSP output meter.
- A subtle selection/layer-color cue. Keep the waveform readable when blend contribution is low; show contribution with a separate small indicator if needed.
- A passive playhead linked to the shared Designer transport. Reuse the existing timeline-to-source mapping for placement, IN/OUT, linked rate and reverse. Hide it outside the source interval and when playback belongs elsewhere. Effect tails do not invent new source waveform content.

An empty slot must clearly offer “Drop sound” and a keyboard-accessible choose-layer action. The whole appropriate card area must accept the existing supported layer/Library drops. Keep missing-layer, offline-file, loading, decode-error and true-silence states distinct. Show an honest empty or flat display; never use decorative random waveforms as audio data.

The new display must not autoplay, change audition ownership, seek on ordinary selection, create another transport, or start a render simply to draw a waveform. Detailed trim/fade/slip editing remains in Edit. Provide a discoverable route to the assigned layer in Edit if adding it can reuse existing selection behavior safely.

### Reuse the current peak infrastructure correctly

Inspect `DesignerView.cpp`'s immutable provider at `19–29`, source key at `1068–1071`, and serial result/request scheduler at `1232–1247`. Inject/reuse equivalent data access in Perform and repaint the relevant cards when data completes.

`WaveformCache` currently has one pending asset and a shared generation token. Do not issue four requests in a loop and assume four outstanding jobs exist. Retain serial scheduling or introduce an explicit bounded, deduplicated queue. Prioritize visible cards/rows. Reuse immutable peaks for the same source identity and avoid a decoder thread per slot/row. Keep memory and retained results bounded.

The current peaks combine extrema from all source channels. A first version may use an explicitly described all-channel source overview; do not mislabel it as the selected mono/stereo-pair waveform. If showing selected-pair waveforms, extend the data/cache format so they are actually correct. Use extrema rather than a signed channel average that could cancel opposite-phase material.

When drawing small previews, aggregate all covered peak bins per pixel column so brief transients survive reduction. Define trimmed-edge bin handling and acknowledge the available peak resolution. Cache display geometry where useful; gain/pan/macro/XY changes must not re-decode source files. Source range/reverse transformations can reuse decoded data.

Use correct source identity, revision and relink validation. On rebinding, remove the old image immediately. Late results must match the current document/layer/source binding before display. Handle replace, clear, remove, undo/redo, snapshot recall, close/reopen, and rapid switching. File reads, decoder creation and cache serialization must be absent from paint and the real-time callback.

### Reuse for file-list previews

After the four-card implementation passes, use the same lightweight renderer for small waveforms in visible Library and compact Browser file rows. Keep their waveform secondary to the filename and key metadata. Respect existing row selection, selection-follow audition, drag operations and the detailed selected-file audition panel.

Use virtualized rows and a bounded request queue. Request visible rows and a small bounded prefetch region, deduplicate shared source requests, and reject late results when a recycled row binds to another file. Do not instantiate one worker or decode the entire catalog when the list opens. Retain smooth text/semantic search, scrolling, indexing and Designer playback. Measure cold and warm behavior with the actual fixture/library size stated.

## 4. Simplify the Designer/Perform presentation

Use Krotos-like clarity and direct manipulation as the usability reference while retaining SoundPlus's design and behavior.

- Keep the four source corners spatially connected to the XY pad. Rebalance card/pad dimensions so waveforms fit without enlarging the window or creating avoidable scrolling. Keep enough pad area for precise use.
- Use compact, consistent macro rows. Do not stretch each row into a largely empty panel to fill height.
- Add visible axis labels such as “X · Left–Right” and “Y · Top–Bottom”. Retain numeric entry, arrow/fine-arrow navigation, Home/reset and accessible names.
- Show a clear Blend on/off control and state. While off, communicate that movement prepares a position and enabling Blend makes it audible. Keep explicit activation; selecting a source must not unexpectedly enable processing.
- Keep four macro names and values readable. Show unassigned, enabled and bypassed status clearly. Explain “no mapped targets” in plain language.
- Default view shows sources, Blend and macros. Target lists, ranges, response curve, inversion and detailed effective values stay in the contextual mapping editor. Preserve reset, remove mapping, ownership feedback and expert access.
- Rework the redundant bottom source selector/drag tool into a compact, discoverable equivalent if needed; do not remove its capability without a tested replacement. Preserve accessible picker alternatives to dragging.
- Preserve the shared transport, comparison, snapshots, output actions, current Lucide family, theme tokens, neutral idle icons, active/focus cues and useful tooltips. Avoid adding a toolbar of new miniature buttons to every waveform.
- Verify the actual supported minimum and normal window layouts. Respect long filenames, focus order, keyboard use, visible text contrast and usable pointer targets. State which display scales were actually checked.

## 5. Resolve or accurately qualify the icon capture issue

The baseline report says some native partial-repaint captures omit idle toolbar glyphs until resize. Establish whether icons disappear on the physical/native display or only in the capture pipeline. Exercise workspace switching, focus, occlusion/exposure, minimize/restore and resizing. If a real application issue reproduces, fix the appropriate invalidation/rendering path. Repeated full-window repaints and a passing offscreen atlas alone do not establish a fix. Report the actual outcome and evidence.

## 6. Functional and performance acceptance

Run the build and relevant existing tests on the supported Mac. Add only tests that resolve the concrete risks above. Broaden to the existing regression suite because this pass touches shared peak rendering, source selection and controls; record exact results rather than assuming prior results still apply.

Verify:

1. Empty and occupied slots, assignment/replace/clear, existing-layer drop, Library drop, alias edits, and one-step undo/redo.
2. New blend continuity and compatibility; corrected Escape gestures; target mapping, bypass/reset/unlink; snapshot recall/reopen.
3. Waveform truthfulness using silence, isolated transients, mono/stereo/multichannel fixtures, two ranges of the same file, reverse/rate, very short/long files, offline/relinked files, and decoder failure.
4. No stale images after rapid reuse, no UI/audio-thread file decoding, deduplicated warm cache behavior, bounded memory/work, and correct transport ownership.
5. Playback/search/indexing while loading cold waveforms and scrolling visible rows. Report workload size, cache condition, machine, buffer size, observed latency/underruns and limits. Do not claim a new speed guarantee from a short descriptive run.
6. Existing detailed editing, gain/pan/fades, snapshots, full and primary-layer render, monitor exclusion, ordinary-layer print-back, and durable output paths.
7. Real native screenshots at minimum and normal size, and a functional walkthrough covering source cards, mapping editor, focus, bypass and error states.

Keep human listening distinct from PCM checks. Do not claim physical Resolve delivery until an original/full/selected output is actually received, placed, saved, and available after SoundPlus exits and the receiving project reopens. Carry that item as open if the receiving-app sequence cannot be completed; it is not evidence of a new waveform failure.

## Deliverables

- Implemented source changes and behavioral regression tests.
- Updated milestone document and live checklist.
- Verification record distinguishing executed automated tests, native UI checks, measurements, human listening and unresolved qualifications.
- Before/after screenshots with the same document, comparable window size and clearly stated capture conditions.
- Compatibility notes for any blend-law/schema changes and a clear statement of waveform channel/range semantics.
- Summary of completed slices, actual evidence and any remaining per-file row rollout work.
- A coherent local commit at the end as required by repository instructions, with its actual commit ID reported. Preserve unrelated changes and report remote publication only if it actually occurred and was authorized.

Complete the authorized implementation and verification; do not stop at a design proposal or a static mockup. Keep later plugin hosting, MIDI, recorded motion, sampler pools and surround outside this focused milestone.
