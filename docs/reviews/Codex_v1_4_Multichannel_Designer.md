# Codex implementation prompt — SoundPlus v1.4

Paste this entire prompt into Codex in the Sound-Plus repository. It is self-contained; attaching the review is helpful but not required.

---

Implement **SoundPlus v1.4 — Multichannel Designer: Layouts, Mixing, Takes and Delivery**.

The user requires mono, stereo, 5.1 and other multichannel operation throughout browsing, design, manipulation, mixing, recorded performance and export. Deliver a working end-to-end feature. The Designer must remain simple, modern and friendly, with advanced controls available contextually. Keep the JUCE/C++ architecture and current Lucide/theme system.

## 1. Start from the actual repository and protect working data

Read the full current AGENTS.md, this prompt, relevant requirements and existing implementation before editing. Inspect git status and current HEAD. This review used HEAD `3609486e6b1313f2ad80181022c04e0db372657f`, implementation `d51cfdb7ba8811d6854838ce3566c1e2b8ab43d4`, over baseline `3eae9b569f1edb9bb94b345c9af5e4354ef7d64e`. Inspect later commits if present; preserve later correct work.

Show and maintain a compact monochrome checklist using ◉, ○ and ✓; persist it in docs. Protect the latest actual user document, catalog, media, renders and settings before native QA/rebuild. Use separate generated fixtures and QA catalogs. Never restore a historical QA backup over newer work. Close processes running from the app bundle before rebuilding it.

The reviewed R5–R9 corrections are closed. Preserve their guards, tests and evidence boundaries. Finish with meaningful local commits and a concise completion report containing actual verification and commit IDs. Do not push unless authorized.

## 2. First commit: correct R10 context restoration

At the reviewed HEAD, `DesignSession.cpp:321–323` stops context-mismatched audition only when the previous context starts with `take:`.

Repro: select Take A → delete A → start Live Design playback → Undo deletion. The document restores A while Live audio continues. A held pending Live preview has the same problem: its completion can call `services.play()` after Undo selects the take.

Make active/pending context invalidation symmetric for Live→Take, Take→Live and Take A→Take B. A cancelled/stale preparation must never start. Preserve legitimate Live→Live editing. Add targeted active and held-preparation Undo/Redo tests, including displayed context versus actual audible stream ownership. Commit this focused correction before the feature work.

## 3. Define a real channel model and supported range

Separate and persist these concepts:

1. Source channel count, detected layout, ordered channel roles and any explicit interpretation override.
2. Layer input interpretation and source-to-layer/layer-to-bus mapping.
3. Design/master bus layout.
4. Export layout, ordering and any conversion policy.
5. Monitor/device routing, independent of rendered content.

Use stable versioned layout IDs, explicit role/order arrays and explicit discrete-channel identities. Do not infer 5.1 from a count of six. Unknown multichannel files remain labelled `6 ch · Unknown layout` or equivalent until explicitly assigned. Preserve metadata confidence and user overrides; relink validation must not silently change interpretation.

Initial qualified processing/export range: **1–16 channels**. Provide Mono, Stereo, LCR, Quad, 5.0, named 5.1 side/rear variants, 7.1 side+rear and discrete layouts. Specify the exact channel roles of every preset. Keep the representation extensible. Preserve existing 1–64-channel source admission and legacy pair selection; sources wider than the qualified bus range require explicit supported selection/routing and must never be silently truncated.

Channel count, layer count and DSP workload are separate limits. Preserve legacy limits and define bounded preparation/memory admission for new expensive combinations. Do not claim that every maximum can be combined without measurement. Publish the actual qualified matrix.

Use the repository's pinned JUCE APIs, with explicit adapters and index queries. Check current official documentation where necessary. Relevant references:

- https://docs.juce.com/master/classjuce_1_1AudioChannelSet.html
- https://docs.juce.com/master/classjuce_1_1WavAudioFormat.html
- https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/channel-mask

Do not assume that a factory's prose list, internal channel indices, hardware output order and WAV interleaving order are identical. Map by roles and test the actual bytes.

## 4. Preserve legacy documents and take identity

The current `PerformTake::recipeHash` hashes `recipeJson()`, which serializes `designMixToJson()`. Adding layout fields unconditionally changes old manifest-4 take hashes.

Retain version-specific canonical serialization and DSP behavior for existing documents and `stock-takes-v1` recipes. Preserve legacy stereo-pair mapping, pan/balance, effects and mono `(L+R)/2` export. Introduce an explicit new document/routing/processing contract for layout-aware designs and takes. Older readers must reject it safely. Never relabel old recipes with new semantics or silently recompute old take hashes on load.

Support a deliberate conversion/copy of a legacy live design into a layout-aware design while keeping existing takes and receipts immutable. Verify historical fixtures through load, save and reopen; preserve their identity and legacy PCM. New recipes must hash source interpretation, ordered bus roles, routing/panner policy, processor channel policy and all relevant version identifiers.

## 5. Generalize the entire audio path

Replace fixed stereo assumptions in layer conversion, graph accumulation, effect state, fades/transitions, resampling, FIFO storage, IPC/status, metering, optional PCM evidence capture, rendering and device output mapping. Current examples include `DesignAudio.cpp`, `AudioStream.h/.cpp` and `AudioEngine.cpp`; audit the whole path.

Prepare bounded buffers, effect histories, routing matrices and conversion state outside the real-time callback. No parsing, allocation, file/database I/O or graph preparation in that callback. Retain the bounded capture queue, consumed-frame clock, lifecycle latch and finalization guarantees.

All existing operations must act correctly on every intended channel: trims, slip, placement, fades and curve edits, reverse, linked rate/pitch, gain, mute/solo, duplication, variations, source relink and render selection. Keep sample alignment across linked channels. Preserve fade controls at clip corners, 10 logical points, subdued two-tone styling and unchanged IN/OUT drag directions.

Define every stock effect's channel policy. Gain and filters need appropriate independent histories; delay needs explicit channel/link/crossfeed semantics. Reverb must have an honest supported multichannel mode, such as linked independent per-channel processing, without calling duplicated stereo a spatial reverb. Preserve the old stereo implementation for legacy contracts. All stock effects in the declared new layout matrix must work; unsupported configurations must be explicit, never silent fold-downs.

Design the processor/bus interface to negotiate layouts for future VST3/AU hosting. Implementing the plugin host itself is outside this milestone.

## 6. Routing, spatial manipulation and LFE

Default matching multichannel sources to role-preserving bed routing. Do not collapse an existing surround bed into a point source. Provide a reviewable source-to-bus matrix with channel gain/trim, polarity, mute and clear channel identification. Channel solo used for listening must be clearly separated from a destructive/export-affecting mix edit.

Keep simple pan for mono→stereo and the established balance behavior for legacy stereo. Add a separately named **Spatial Panner** for appropriate sources routed to horizontal surround layouts. Provide smooth 360-degree horizontal positioning, deterministic interpolation and a documented panning law. Expose width/divergence/center behavior progressively. For stereo inputs, explicitly define image width/orientation or a selected fold-down policy. Discrete layouts without speaker positions get routing controls rather than a fictional speaker diagram.

Keep the existing four-corner pad named **Source Blend**. Its X/Y coordinates control source contributions; do not reinterpret old blend events as speaker positions.

For mono point sources, LFE send defaults off. Preserve an existing LFE channel's explicit routing. Provide deliberate LFE send/filter settings where supported, separate from monitor bass management. Do not silently add LFE gain to exports. Document conversion coefficients, headroom, channel linking and LFE treatment; test them with correlated, anti-phase and isolated-channel fixtures.

## 7. Keep the UI compact and informative

Add a compact Design Layout selector/badge. Keep three concepts visibly distinct: **Design**, **Monitor** and **Export**. Default export to Same as design.

Preserve the small real waveform for each visible source card and Browser result. Add a compact source layout badge. Keep combined extrema overviews for dense browsing; provide expandable labelled per-channel waveforms for the selected source and Designer layer. Show loading, unknown layout, offline and changed-source states accurately. The displayed identity must match the adopted source, mapping and audio.

Reuse the shared waveform scheduler. Extend cache identity/versioning for source revision, channel selection/layout and detail level. Bound resident bytes and work across owners; do not multiply existing peak storage by channel count without admission/eviction. Preserve the corrected summary ceiling of 2,048 pairs per summary, existing count limits where applicable, and settled-idle behavior. Document and test any necessary budget change.

Add compact labelled master meters and contextual channel details. Keep routing matrices, speaker/device assignments and advanced panner settings behind disclosure. Support normal and minimum window sizes, keyboard focus, accessible names, useful units and Undo/Redo. Ordinary mono/stereo actions should remain quick.

## 8. Separate monitoring from the design

Permit genuine 5.1 design/export on a machine with stereo speakers through a clearly labelled **Stereo monitor downmix**. Hardware output assignment, monitor gain and monitor solo must not change frozen design audio or offline output.

Define and test monitoring conversion matrices and headroom. Unknown device maps require explicit assignment; do not duplicate the right channel into extra outputs. Surface unavailable channels and preserve valid project state when hardware changes. A device/layout interruption during capture must follow the existing explicit failure/finalization policy, including Arming and held final commits.

Only claim physical 5.1 monitoring after it has been tested on actual mapped multichannel hardware. Stereo-device and offline tests remain useful, separately labelled evidence.

## 9. Record and replay multichannel performance correctly

Before Arming completes, freeze source interpretations, bus layout, routing, panner defaults and DSP channel policies into the take recipe. Monitor configuration stays outside that recipe.

Include moving spatial controls in a versioned, typed applied-event contract with stable targets, finite validated values, deterministic smoothing and the consumed-audio-frame clock. New spatial motion must replay/render on every channel. Keep existing Source Blend/macros semantically unchanged for old takes. Do not expose record-time controls whose changes are neither captured nor frozen visibly.

Layout changes, structural routing edits and source replacement are disabled during Arming/Recording/Finalizing. Keep runtime event/preparation work bounded. Preserve the existing 120-second, 8,192-event and 16-take limits unless a separately justified change is tested; new event types share the bounded capacity and failure behavior. Test overflow, stop, cancellation, stale generations and device failures.

Editing live layouts, source mappings, effects or future pools after a take is complete must not change that take's replay or render. Metadata operations and output receipt completion must retain R5 protection. Compare pre-monitor captured/replayed/rendered PCM per channel, including rate conversion and stateful tails.

## 10. Export, receipts and durable delivery

Implement interleaved multichannel WAV with correct role/order metadata and a split-mono export option. Show actual channel order, layout, rate, depth, conversion policy, tail, normalization and dither before render. A surround→stereo/mono export is an explicit conversion. Larger-file handling must be deliberate: qualify RF64 where supported or reject oversized unsupported containers before work with a clear size explanation. Never publish truncated files.

Preserve interchannel balance when normalizing: use one documented shared gain for a linked multichannel mix rather than independently normalizing each speaker. Dither/quantization must be deterministic under the declared policy and must not create unintended perfectly identical noise across all channels. Check each channel for clipping and nonfinite output.

Receipts must identify design/export layouts and versions, actual ordered channels, conversion/routing policy, source interpretations, take recipe/event/processing identity, rate/frame count and final file hashes. For split files, record all channel roles, filenames and hashes as one output set; publish/clean up the set transactionally without deleting earlier durable outputs.

Bind ready delivery to the actual finalized output selection. If a take has stereo and 5.1 renders, show which file/set is ready. Changing the export preset must not relabel an old stereo render as 5.1. Preserve R6 checks at render completion, reopen, drag and Print. Print/import must preserve multichannel source identity rather than silently choosing a stereo pair.

Verify actual receiving-app import/drop, channel interpretation, timeline placement, save, exit SoundPlus and reopen the receiving project. If unavailable, report that gate as blocked/unverified with the reason; successful WAV writing or drag initiation is not receiver qualification.

## 11. Required verification and completion evidence

Use meaningful generated fixtures with unique impulses/tones per channel, including anti-phase material. Test every declared named preset—Mono, Stereo, LCR, Quad, 5.0, each 5.1 variant and 7.1—plus unknown six-channel discrete input and the 16-channel bound. Include mixed source rates/channels/durations, reverse, fade curves and nondefault mappings.

Prove channel identity through import → manipulation → routing/panning → every stock effect → resampling → Perform capture/replay → render → independent reimport. Detect channel swaps, dropped channels, repeated-right output, unintended crosstalk and incorrect LFE routing. Test channel gains/meters/solo semantics, linked normalization, split-set failures and WAV metadata/order against actual PCM.

Cover legacy canonical hashes/receipts and PCM parity; layout-aware save/reopen/Undo/Redo; same-bytes relink versus changed sources; active/pending R10 transitions; mismatched live edits during take replay; multiple output layouts; stereo monitoring without render changes; async document/selection changes; capture lifecycle errors and every finalization boundary touched by the new path.

Run the existing 20 suites plus relevant new tests. Because the audio graph, persistence and output paths change, the full existing suite is warranted. Run native normal/minimum UI workflows and focused search/index/waveform pressure with multichannel details visible. Report channel/layer counts, rates, buffer sizes, peak RSS, waveform bytes, preparations, underruns and audio evidence. Do not extrapolate to untested maximum workloads.

Separate automated checks, native UI, PCM comparisons, hardware routing, human listening and receiver persistence. Keep the historical unexplained helper restart and zero-peak observations accurately qualified unless diagnosed. Do not convert old evidence into a new surround qualification.

Deliver working code in reviewable commits, migration/compatibility documentation, a supported-layout/operation matrix, raw logs/measurement records, real screenshots at normal/minimum sizes, and concise reproduction steps. Reopen the latest user design without replacing it with a QA snapshot. Do not finish at a mockup, dropdown or architecture proposal.

## 12. Follow-on: Source Pools

Update `docs/SOURCE_POOLS_NEXT.md` so Source Pools follows this foundation. Each alternate needs stable IDs, source layout/range and explicit mapping to its layer/bus. Seeded resolution must persist the chosen identity and mapping; missing selected sources must not silently reroll. Inactive unavailable alternatives must not block a healthy selected source. Freeze resolved entries before recording; later pool changes cannot alter takes. Keep source switching disabled during capture until an explicit event/voice model supports it.

Do not implement Source Pools, plugin hosting, object-audio delivery, Ambisonic transforms or video/Resolve automation in this milestone. Preserve their extension paths while completing the declared mono/stereo/surround/discrete workflow.

Start by inspecting HEAD and creating the checklist, then complete the R10 prerequisite and the multichannel implementation through verification and local commits.
