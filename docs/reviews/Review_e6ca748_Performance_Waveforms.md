# SoundPlus Performance Mixer review and waveform milestone

## Scope and verdict

| Field | Scope |
|---|---|
| Episode | All projects; application development |
| Scene | All; no scene-specific media work |
| Department | SoundPlus engineering, sound design workflow, UI and QA |
| Priority | Fix two functional issues, then deliver compact source waveforms and the usability pass |
| Next action | Implement Performance Mixer v1.1 using the companion Codex prompt |

Reviewed commit: [`e6ca7484830d6bfc8d3c95a22b7b8b6e889d1e6f`](https://github.com/Tekie79/Sound-Plus/commit/e6ca7484830d6bfc8d3c95a22b7b8b6e889d1e6f), “Add shared Performance Mixer with macros and static snapshots.” GitHub HEAD was reconfirmed during this review on November 18, 2026. The principal performance implementation was reviewed against parent `43f5776`; the intervening fade changes from `527c44d` were also accounted for.

**Disposition: retain the shared architecture, correct two P2 functional issues, and complete a focused waveform/usability milestone before signing off the new Perform experience.** No additional audio integration blocker was identified in the inspected paths. This is not a blanket certification of every application feature.

This review inspected source, the attached implementation report, the repository evidence record, the CTest log, and normal/minimum-window screenshots. It independently executed a mathematical reproduction of the sparse-blend behavior and checked the recorded suite names against CMake. The host is Linux; the application explicitly targets macOS Apple Silicon. No native application build, JUCE pointer test, audio playback, or human listening test was executed here. Application source and remote repository contents were not modified.

## Finding 1 — P2: the blend changes abruptly at an empty corner or edge

**Source:** [`PerformanceState.cpp`, `blendWeights`, lines 296–325](https://github.com/Tekie79/Sound-Plus/blob/e6ca7484830d6bfc8d3c95a22b7b8b6e889d1e6f/src/PerformanceState.cpp#L296-L325).

The implementation calculates bilinear corner amplitudes, removes unoccupied slots, and normalizes the remaining squared amplitudes. When those amplitudes all reach zero, it substitutes an equal-power mixture of the occupied slots. That endpoint substitution does not agree with the mixtures approached from neighboring positions.

The user's screenshot has exactly the relevant arrangement: top-left, top-right, and bottom-left assigned; bottom-right empty. With Blend enabled, an independently executed reproduction of the source formula gives:

| X | Y | Top-left amplitude | Top-right amplitude | Bottom-left amplitude |
|---:|---:|---:|---:|---:|
| 1.000 | 0.999 | 0 | 1 | 0 |
| 1.000 | 1.000 | 0.577350269 | 0.577350269 | 0.577350269 |
| 0.999 | 1.000 | 0 | 0 | 1 |
| 0.999 | 0.999 | 0.000707814 | 0.707106604 | 0.707106604 |

The final tiny move into the empty corner introduces other sounds and changes the previous dominant source by approximately −4.77 dB. Approaching from a different direction produces a different limit. With only the two upper slots assigned, the bottom edge also has this problem: at X=.1, Y=.999 the amplitudes are approximately .993884/.110432; at Y=1 they become .707107/.707107.

These are computed amplitude weights, not loudness percentages, measured output peaks, or an independently heard click. Existing smoothing can soften the temporal change, but it does not correct the unexpected endpoint mixture.

The current tests check bounds, power normalization, unchanged unassigned layers, and idempotence on a grid. They do not check continuity between neighboring positions. They also use the first N slots rather than every occupancy arrangement. See [`PerformanceStateTests.cpp`, lines 202–227](https://github.com/Tekie79/Sound-Plus/blob/e6ca7484830d6bfc8d3c95a22b7b8b6e889d1e6f/tests/PerformanceStateTests.cpp#L202-L227).

**Required correction:** define a deterministic, continuous law for partially occupied pads, including every missing corner and empty edge. Preserve zero-slot no-op and one-slot unity. Test all 16 occupancy masks, exact endpoints, and approaches from several directions. Do not depend on the last pointer position or last valid weights: that would make saved snapshots history-dependent. A geometrical interpolation over the occupied corners is a possible design; changing only the equal-weight fallback cannot remove the direction-dependent limits of the existing formula.

Changing the law can alter the sound of existing saved control configurations. Record and implement a compatibility decision: retain legacy behavior for existing versions, or provide an explicit migration with recovery. Never silently reinterpret existing saved mixes or overwrite delivered WAVs.

## Finding 2 — P2: Escape does not suppress the remainder of a slider drag

**Sources:** [`PerformView.cpp`, macro callbacks, lines 237–246](https://github.com/Tekie79/Sound-Plus/blob/e6ca7484830d6bfc8d3c95a22b7b8b6e889d1e6f/src/PerformView.cpp#L237-L246); [`endControl` and `setPad`, lines 349–394](https://github.com/Tekie79/Sound-Plus/blob/e6ca7484830d6bfc8d3c95a22b7b8b6e889d1e6f/src/PerformView.cpp#L349-L394); [X/Y callbacks, lines 135–154](https://github.com/Tekie79/Sound-Plus/blob/e6ca7484830d6bfc8d3c95a22b7b8b6e889d1e6f/src/PerformView.cpp#L135-L154).

Once Escape is handled, `endControl(true)` restores the original session gesture and sets both `controlGesture` and `staleControl` to false. Subsequent value callbacks from a slider whose mouse button is still held are therefore accepted. They can write another value as an ordinary edit, creating an undo entry after the user cancelled.

The pad itself checks `controlValid()` on every drag and stops applying changes after Escape. The sliders use a different callback path. This is why the existing pad cancellation test does not cover the slider case.

**Native reproduction to run:** drag a macro slider; press Escape while holding the mouse button; continue moving; release. Repeat with both X/Y sliders. Expected: the original value remains restored, with no cancelled edit in history. This is a source-confirmed control-flow finding; that native reproduction and the proposed callback sequence were not executed here.

**Required correction:** latch cancellation until the original pointer gesture ends. Suppress its remaining value callbacks and finish release without committing. A subsequent new drag must work. Keep keyboard and numeric entry working outside a drag, and preserve document/variation identity guards.

Tests should cover all four macro sliders and both X/Y sliders; movement after Escape; release; a fresh drag; undo/redo; and switching document or variation during a gesture. The current tests cover pad Escape and ordinary macro undo, not this continued-drag sequence. See [`PerformanceStateTests.cpp`, lines 95–108](https://github.com/Tekie79/Sound-Plus/blob/e6ca7484830d6bfc8d3c95a22b7b8b6e889d1e6f/tests/PerformanceStateTests.cpp#L95-L108).

## What is supported by the inspected implementation

| Functionality | Assessment |
|---|---|
| Shared effective state | The resolver copies the base mix, applies controls, and clears the copied control state. This supports idempotent resolution. |
| Live playback and rendering | Both use the common resolver; no confirmed double-application path was found. |
| Primary-layer rendering | Full-mix performance state is resolved before isolating the selected layer, retaining its blend contribution. |
| Continuous edits | Performance metadata and continuous values are excluded from the structural signature; effective values use the parameter-update path. |
| Static snapshots | A kept variation stores the editable base mix plus performance controls, rather than baking controls and then applying them again. |
| Mapping ownership | Stable layer/effect identities and conflict validation prevent reordering from redirecting a target. Missing targets remain unresolved. |
| Existing editing | The Edit canvas remains the detailed layer editor; the new source cards should reuse its data without replacing its trim, fade, slip, seek, or selection operations. |

Evidence: [resolver](https://github.com/Tekie79/Sound-Plus/blob/e6ca7484830d6bfc8d3c95a22b7b8b6e889d1e6f/src/PerformanceState.cpp#L328-L361), [live state](https://github.com/Tekie79/Sound-Plus/blob/e6ca7484830d6bfc8d3c95a22b7b8b6e889d1e6f/src/DesignLive.cpp#L48-L138), [graph construction](https://github.com/Tekie79/Sound-Plus/blob/e6ca7484830d6bfc8d3c95a22b7b8b6e889d1e6f/src/DesignAudio.cpp#L476-L489), [snapshot creation](https://github.com/Tekie79/Sound-Plus/blob/e6ca7484830d6bfc8d3c95a22b7b8b6e889d1e6f/src/DesignSession.cpp#L684-L702), and [selected render](https://github.com/Tekie79/Sound-Plus/blob/e6ca7484830d6bfc8d3c95a22b7b8b6e889d1e6f/src/DesignSession.cpp#L965-L981).

## Evidence validation and remaining qualifications

**Independently checked here:** exact GitHub/local HEAD; the two supplied implementation reports are byte-identical; the committed CTest log lists 13 passing suites and 50.67 seconds; the suite names match CMake and the evidence record; both referenced screenshots exist and decode; the sparse-blend formula produces the values above. The inspected screenshot files are 1171×768 and 1041×681 pixels; these are capture sizes, not claims about additional physical displays.

**Recorded native evidence, not re-executed here:** 4,831 performance-state assertions, 1,449 canvas assertions, 79 view-command assertions; native static pre-monitor/render PCM maximum error 0.0; the short eight-layer/26-insert workload with zero underruns; saved snapshot reopening; internal drops; full/primary-layer output receipts and print-back. The report records output hashes and an app hash, but those Mac-local binaries and WAVs were unavailable here, so their hashes could not be independently recalculated.

The performance workload was 12 seconds at 48 kHz/512 frames on the stated M5 Pro. It is useful development evidence and does not establish a 30-minute soak, 256-frame operation, other hardware/interfaces, perceptual filter smoothness, or human listening acceptance. The different control protocols in the baseline and Perform workloads also prevent a strong statistical “faster” conclusion.

The report explicitly leaves two practical qualifications open:

- Some native captures omitted idle toolbar glyphs until resize. The cause may be the capture path or actual macOS repainting. Establish whether it occurs on the visible display before choosing a fix. Passing an offscreen icon atlas does not close this question.
- Physical SoundPlus-to-Resolve receiving-app drop and persistence remain unverified. Internal drops and generated WAVs do not establish import, timeline placement, save/quit/reopen, or continued media availability in Resolve.

Sources: [CTest log](https://github.com/Tekie79/Sound-Plus/blob/e6ca7484830d6bfc8d3c95a22b7b8b6e889d1e6f/docs/verification/performance-ctest.log), [verification record](https://github.com/Tekie79/Sound-Plus/blob/e6ca7484830d6bfc8d3c95a22b7b8b6e889d1e6f/docs/verification/performance-mixer-v1.json), and [implementation report](https://github.com/Tekie79/Sound-Plus/blob/e6ca7484830d6bfc8d3c95a22b7b8b6e889d1e6f/docs/PERFORMANCE_MIXER_V1.md).

## Recommended next milestone: Performance Mixer v1.1 — waveforms and usability

### Source cards

Every occupied Perform assignment should show a genuine compact waveform, approximately 24–32 logical points high, inside a compact source card. Keep the four corner relationships visible around the pad. Show one useful title, a choose/replace affordance, the waveform, and a brief duration hint. Display a custom alias separately from the filename only when they differ. Use ellipsis and a full-name tooltip for long filenames; do not collapse distinct layers merely because they reference the same file.

The card's waveform should display the assigned layer's selected source range `[IN, OUT)`, with reverse represented consistently and duration reflecting the linked rate. It is a source overview before fades, inserts and macro DSP. Do not imply that its amplitude is a rendered output meter. If using the existing all-channel peaks, identify them truthfully as an all-channel source overview for multichannel files; selected-pair waveforms need a corresponding cache representation.

An empty card should offer “Drop sound” and a choose-layer action. Loading, unavailable source, and decode failure need distinct states. A silent file should show a truthful flat waveform, not the same state as a missing file.

A thin playhead can follow the existing Designer transport, using the same timeline-to-source mapping as the Edit canvas. It must account for placement, source range, rate and reverse, and disappear outside the active source interval or when playback belongs elsewhere. Clicking a card or painting its waveform must not start a new player or steal Library audition.

The same lightweight renderer should be reusable for small waveforms in visible Library/Browser rows. Treat that as a separate delivery slice after the four source cards: virtualize rows, request visible assets first, and retain one bounded scheduler/cache rather than starting work for the whole catalog. The existing detailed audition waveform and Edit timeline do not need redundant replacement controls.

### Cache and performance

Reuse the Designer's immutable waveform provider and completed-data map. Its current serial scheduler can feed the cards as well as the Edit canvas. `WaveformCache` has one pending request and a shared generation: calling `request()` for four slots in a loop can replace pending work and invalidate earlier requests. Use the existing serial scheduling or a deliberate deduplicated queue.

The peak cache currently combines minimum/maximum across all channels. When reducing peaks to a tiny display, aggregate every peak bin covered by each screen column. Sampling only one bin can hide short transients. Define edge-bin behavior for trimmed ranges, acknowledging the cached peak resolution rather than promising sample-exact boundary detail.

Reuse source data for repeated layers, and transform/crop it for each card. Source revision and relink must invalidate the right data. Layer binding, range, reverse and channel interpretation must remain correct after rapid replacement, undo, snapshot recall and document changes. Clear old images immediately when the binding changes; late results must not be painted under a new filename. Keep decoding, disk reads and cache writes outside painting and audio callbacks. XY/macro movement must not cause source re-decoding. Keep memory bounded.

Sources: [Designer provider](https://github.com/Tekie79/Sound-Plus/blob/e6ca7484830d6bfc8d3c95a22b7b8b6e889d1e6f/src/DesignerView.cpp#L19-L29), [scheduler](https://github.com/Tekie79/Sound-Plus/blob/e6ca7484830d6bfc8d3c95a22b7b8b6e889d1e6f/src/DesignerView.cpp#L1232-L1247), [request generations](https://github.com/Tekie79/Sound-Plus/blob/e6ca7484830d6bfc8d3c95a22b7b8b6e889d1e6f/src/Waveform.cpp#L54-L74), and [peak generation](https://github.com/Tekie79/Sound-Plus/blob/e6ca7484830d6bfc8d3c95a22b7b8b6e889d1e6f/src/Waveform.cpp#L155-L177).

### Simple default view, advanced controls on demand

| Current issue | Recommended behavior |
|---|---|
| Repeated source names | One title/selector per card, optional distinct alias, waveform and concise duration |
| Large empty pad and stretched macro panels | Balanced pad size, compact macro rows, consistent spacing; preserve usable pointer precision |
| Visually unlabeled X/Y controls | Visible “X · Left–Right” and “Y · Top–Bottom” labels with numeric entry |
| Bypassed pad still moves without an obvious explanation | Clear Blend on/off state; a short cue that position changes are prepared while Blend is off |
| Expert mapping details compete with performance controls | Default view shows sounds, Blend and four macros; mapping ranges, curve and inversion stay in a contextual editor |
| Extra source drag selector consumes space | Preserve its capability through an obvious drag handle on source cards or another tested compact control |
| Many similar action icons | Keep the existing Lucide family, accessible names, meaningful tooltips and clear focus/on/off states |

The reference is Krotos-like ease of use: direct manipulation, readable hierarchy and immediate feedback. The implementation should remain SoundPlus's own interface and shared engine.

### Completion gates

1. Both functional findings have failing-before/passing-after behavioral tests and native reproductions.
2. Every assigned card has a real source waveform and correct binding after replace, clear, undo, redo, snapshot recall and relink.
3. Fixtures cover silence, short transients, stereo/multichannel semantics, short and long files, trim, reverse, rate, and loading/error states.
4. Warm thumbnails reuse the Edit data; control motion does not decode files; cold requests remain responsive during playback and search.
5. The supported minimum and normal window layouts keep source cards, pad, macros/mapping, transport and outputs reachable. Test keyboard focus and scaling as actually available.
6. Existing fade/trim/slip/scrub/selection operations, audition ownership, mappings, snapshots, rendering, print-back and undo/redo remain intact.
7. The icon disappearance issue is either reproduced and corrected or explicitly retained as unresolved with evidence.
8. Build/test results, native observations and human listening are reported separately. Resolve drag remains open until the actual receiving-app sequence succeeds.

The companion implementation prompt is `Codex_Performance_v1_1_Waveforms_Usability.md`.
