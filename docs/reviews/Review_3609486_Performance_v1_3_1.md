# SoundPlus v1.3.1 review and revised next milestone

## Decision

**Close R5–R9. Fix one new P2 context-restoration issue, then proceed to v1.4 — Multichannel Designer: Layouts, Mixing, Takes and Delivery.**

The user's latest requirement makes mono, stereo, 5.1 and other multichannel operation a first-class requirement across browsing, design, manipulation, mixing, recorded performance and export. Put this foundation before Source Pools so alternate-source entries inherit correct channel semantics from the start. Source Pools becomes the following milestone.

Reviewed HEAD: [`3609486e6b1313f2ad80181022c04e0db372657f`](https://github.com/Tekie79/Sound-Plus/commit/3609486e6b1313f2ad80181022c04e0db372657f), reconfirmed against GitHub before preparing this report. Implementation: [`d51cfdb7ba8811d6854838ce3566c1e2b8ab43d4`](https://github.com/Tekie79/Sound-Plus/commit/d51cfdb7ba8811d6854838ce3566c1e2b8ab43d4). Review baseline: `3eae9b569f1edb9bb94b345c9af5e4354ef7d64e`.

The complete supplied correction report, current repository instructions, implementation, tests, committed evidence and three screenshots were reviewed. This was a read-only source/evidence review in Linux. No application source was changed, no commit was pushed, and native macOS tests, physical audio devices and listening were not newly exercised.

## 1. Previous findings

| Finding | Decision | Evidence and practical result |
| --- | --- | --- |
| R5 — Live edits could alter immutable take audition | Closed | Metadata edits are separated from audio edits. The session, playback service, reader and graph protect immutable replay. Tests cover held preparation, deliberately stale live parameters, real output-popup callbacks and receipt arrival while a take plays. [Replay guards](https://github.com/Tekie79/Sound-Plus/blob/3609486e6b1313f2ad80181022c04e0db372657f/src/AudioPlaybackService.cpp#L356), [reader guard](https://github.com/Tekie79/Sound-Plus/blob/3609486e6b1313f2ad80181022c04e0db372657f/src/DesignAudio.cpp#L656). |
| R6 — Ready output could belong to another context | Closed | Output resolution uses the current Live/take context and receipt identity. Drag and Print resolve the actual ready file again. Late renders and reopening a take without an output do not expose another take's file. [Resolver](https://github.com/Tekie79/Sound-Plus/blob/3609486e6b1313f2ad80181022c04e0db372657f/src/DesignTakeSession.cpp#L143). |
| R7 — Relinked take render checked the wrong source snapshot | Closed | Normalization, rendering, receipt provenance and final source validation now use one verified effective input. Tests move the original away, relink identical bytes, introduce an unrelated missing live source and mutate the effective source before publication. [Render implementation](https://github.com/Tekie79/Sound-Plus/blob/3609486e6b1313f2ad80181022c04e0db372657f/src/DesignAudio.cpp#L720). |
| R8 — Device lifecycle failures could escape capture finalization | Closed | A capture reservation and first-fault latch cover Arming through final commit, including same-rate stop/start and errors. Controlled tests call the actual engine handlers and service path. Physical unplug remains unqualified. [Lifecycle latch](https://github.com/Tekie79/Sound-Plus/blob/3609486e6b1313f2ad80181022c04e0db372657f/src/CaptureDeviceLifecycle.h#L40). |
| R9 — Recovered display-only waveforms could exceed their bound | Closed | Recovery adoption and eviction use the same capped summary path. Tests exercise Offline/Changed recovery, display-only entries, selected-source promotion and settled work. [Bounded adoption](https://github.com/Tekie79/Sound-Plus/blob/3609486e6b1313f2ad80181022c04e0db372657f/src/WaveformPreview.cpp#L91). |

These closures apply to the reviewed paths and evidence. They do not establish a universal absence of audio defects.

## 2. New finding R10 — P2: Undo can show a take while Live Design keeps playing

The context-mismatch check in [`DesignSession::changed()`](https://github.com/Tekie79/Sound-Plus/blob/3609486e6b1313f2ad80181022c04e0db372657f/src/DesignSession.cpp#L319) only stops audition when the previous context begins with `take:`. The reverse transition is missed.

Reproduction derived from the code:

1. Select Take A.
2. Delete A, which returns the document to Live Design.
3. Start Live Design playback.
4. Undo the deletion.

Undo restores Take A and its selected state, but the old Live audition continues. Playback ownership checks document/generation, so the UI can present take transport while audio still belongs to Live Design. A held Live preparation can also complete after Undo restores the take, because its completion calls `services.play()` before handling the mismatched context. [Preparation completion](https://github.com/Tekie79/Sound-Plus/blob/3609486e6b1313f2ad80181022c04e0db372657f/src/DesignSession.cpp#L890).

**Correction:** invalidate active and pending audition symmetrically when a nonempty audition context differs from the restored document context. Cover Live→Take, Take→Live and Take A→Take B. Preserve normal Live→Live parameter/variation editing. Stale held completions must not start the cancelled audio.

Add focused active/pending Undo/Redo regressions. This is a small prerequisite commit within v1.4. It does not reopen R5's frozen-take mutation defect, and it does not justify another broad reliability-only milestone.

## 3. What was independently validated

The accompanying `Review_3609486_Evidence_Audit.json` records **53 passing evidence consistency checks**:

- All **24 declared repository artifacts** match their committed SHA-256 hashes.
- All six latency distributions were recalculated from raw observations using nearest-rank percentiles: three distributions each for capture and replay.
- **70 viewport observations** account for every visible row as painted and ready/offline/error.
- Recorded interval statuses preserve helper PID, process generation, play generation, graph epoch, preparation count, sample rate and buffer size. They report no underruns or helper-health events in those focused runs.
- Three saved documents contain **three distinct takes with 2,278 unique events**. Counting the repeated capture take in the replay document gives 4,545 stored event records. Frozen capture/replay payload fields match despite the metadata edits.
- Seven stored receipt records, including duplicates across documents, agree with their take/source identity and body/tail frame arithmetic.
- The committed corpus manifest reproduces **10,000 distinct paths, 133 content hashes and 1,253,073,116 bytes**.
- The committed CTest log matches the **20 current registered suites**, reporting **20/20 passed in 77.11 seconds**.
- All three JPEGs decode successfully and were visually inspected.

The native record reports 157 take-integrity checks, 36 device-lifecycle checks, 296 waveform-scheduling checks and 2,820 recorded-take checks. The targeted metadata-replay and direct-reader comparisons report maximum PCM error `0.0`, with a native tolerance of `1e-6`. These are committed native results, not tests rerun by this review. [Native evidence](https://github.com/Tekie79/Sound-Plus/blob/3609486e6b1313f2ad80181022c04e0db372657f/docs/verification/performance-v131.json), [CTest log](https://github.com/Tekie79/Sound-Plus/blob/3609486e6b1313f2ad80181022c04e0db372657f/docs/verification/performance-v131-ctest.log).

### Focused performance records

| Measurement | Capture run | Replay run |
| --- | --- | --- |
| Observed active interval | About 60 seconds | About 45 seconds |
| Consumed captured audio | 2,881,536 frames at 48 kHz = 60.032 seconds | Replays the same frozen take |
| Applied capture events / metadata-edit cycles | 2,267 events | 179 metadata-edit cycles |
| Query-to-paint p50 / p95 / p99 | 5.17 / 10.42 / 13.67 ms | 5.79 / 24.31 / 32.34 ms |
| Selected waveform p50 / p95 / p99 | 22.26 / 42.04 / 58.02 ms | 19.21 / 40.27 / 49.73 ms |
| Whole viewport p50 / p95 / p99 | 232.77 / 277.34 / 358.88 ms | 200.36 / 250.54 / 251.60 ms |
| Nonzero / total peak observations | 24,582 / 24,586 | 16,113 / 16,113 |

The four zero-peak capture observations remain undiagnosed. Zero observed underruns does not prove uninterrupted audible playback. These focused runs do not newly qualify the 120-second capture ceiling, a 30-minute workload or another buffer/device configuration. The 10,000-file corpus is a generated I/O workload with repeated content, not 10,000 independently recorded commercial effects.

Source/output audits report preserved user media and durable renders, and reopening of the latest inspected Rain glass design at r9299. This review checked those records for consistency; the original Mac files were unavailable for fresh rehashing. JUCE's canonical document/recipe/event hashes were not independently reconstructed: artifact hashes, payload equality and cross-record identities were checked instead.

## 4. Designer assessment and waveform request

The current normal and minimum-size screenshots show the requested small waveforms on the four Perform source cards and Browser results. The compact layout retains readable source cards, the Source Blend pad and macro controls. Take B's no-output screenshot shows delivery unavailable instead of exposing Take A's output. [Normal layout](https://github.com/Tekie79/Sound-Plus/blob/3609486e6b1313f2ad80181022c04e0db372657f/docs/verification/performance-v131-normal.jpg), [minimum layout](https://github.com/Tekie79/Sound-Plus/blob/3609486e6b1313f2ad80181022c04e0db372657f/docs/verification/performance-v131-minimum.jpg), [Take B](https://github.com/Tekie79/Sound-Plus/blob/3609486e6b1313f2ad80181022c04e0db372657f/docs/verification/performance-v131-b-no-output.jpg).

The waveforms are combined all-channel extrema overviews. That is useful for browsing, but it cannot show which speaker channel contains a transient. Retain those compact previews and add expandable, labelled per-channel lanes in selected-source and selected-layer details. Make source/layout/availability identity visible alongside the preview. [Current source-card semantics](https://github.com/Tekie79/Sound-Plus/blob/3609486e6b1313f2ad80181022c04e0db372657f/src/SourceCard.cpp#L47).

There is no need for another general cosmetic redesign. Continue the current Lucide/theme system, restrained active states and progressive disclosure. A compact design-layout badge, labelled master meters and contextual Spatial Panner provide the new capability without filling every mono/stereo workflow with routing controls.

## 5. Current multichannel gap

**Multichannel input acceptance is not end-to-end surround support.** Current source inspection shows:

| Component | Current state |
| --- | --- |
| Source metadata | Accepts 1–64 source channels, but stores a count without an ordered speaker-layout contract. |
| Layer mapping | Selects an adjacent stereo pair or arithmetic all-channel mono average. |
| Mixing and effects | Two-channel graph; stereo filter/delay/reverb state. |
| Playback | Two-channel FIFO, two-channel resampler and two requested device outputs. |
| Extra device outputs | Current callback would repeat the right signal into outputs above channel 2. This is a structural obstacle to enabling wider devices, not an observed current two-output regression. |
| Render | Two-channel processing buffer; output validation permits only one or two channels. Mono is a final `(L+R)/2` fold-down. |
| Takes and receipts | Freeze existing stereo semantics, but have no explicit bus layout, speaker order, multichannel routing or conversion policy. |

Evidence: [source limits/mapping and render validation](https://github.com/Tekie79/Sound-Plus/blob/3609486e6b1313f2ad80181022c04e0db372657f/src/DesignModel.cpp#L154), [source conversion](https://github.com/Tekie79/Sound-Plus/blob/3609486e6b1313f2ad80181022c04e0db372657f/src/DesignAudio.cpp#L304), [stereo FIFO](https://github.com/Tekie79/Sound-Plus/blob/3609486e6b1313f2ad80181022c04e0db372657f/src/AudioStream.h#L105), [callback](https://github.com/Tekie79/Sound-Plus/blob/3609486e6b1313f2ad80181022c04e0db372657f/src/AudioStream.cpp#L156), [two-output initialization](https://github.com/Tekie79/Sound-Plus/blob/3609486e6b1313f2ad80181022c04e0db372657f/src/AudioEngine.cpp#L15), [render conversion](https://github.com/Tekie79/Sound-Plus/blob/3609486e6b1313f2ad80181022c04e0db372657f/src/DesignAudio.cpp#L799).

These are existing feature limitations. Adding a 5.1 dropdown without replacing the stereo assumptions would produce incorrect behavior.

## 6. Revised next milestone

### v1.4 — Multichannel Designer: Layouts, Mixing, Takes and Delivery

Recommended first qualified processing/export range: **1–16 channels**, with Mono, Stereo, LCR, Quad, 5.0, explicitly named 5.1 side/rear variants, 7.1 side+rear, and discrete channels. This is a proposed initial qualification boundary, not an existing capability or a permanent product ceiling. Preserve legacy source admission up to 64 channels; wider sources require explicit supported routing rather than silent truncation. Keep the model extensible and document workload/memory limits.

Five identities must remain separate: **source layout → layer interpretation/routing → design bus → export layout**, plus an independent **monitor route**. A six-channel file without reliable role metadata remains six discrete/unknown channels until interpreted explicitly. JUCE provides named and discrete channel sets; adapters should use channel roles and index queries instead of assuming array order. WAVE channel masks also establish ordering rules. [JUCE AudioChannelSet](https://docs.juce.com/master/classjuce_1_1AudioChannelSet.html), [Microsoft channel masks](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/channel-mask).

| Area | Required delivered behavior |
| --- | --- |
| Browse and inspect | Compact real waveform and layout badge; selected-source per-channel lanes; explicit layout assignment for ambiguous files. |
| Edit and manipulate | Trim, fades, slip, reverse, linked rate/pitch, placement, gain, mute/solo, Undo/Redo and variations preserve intended channel identities and timing. |
| Mix | Real N-channel layer/master processing, channel-preserving bed routing, explicit conversions, channel trims/mutes and labelled meters. |
| Spatial design | Separate Spatial Panner for appropriate mono/stereo sources, with predictable surround motion; existing Source Blend keeps its meaning. Multichannel beds default to preserving their channels. |
| Stock effects | Explicit deterministic per-channel or linked policies for every processor. Retain legacy stereo sound. No implicit repeated-right or stereo duplication. |
| Perform and record | Layout-aware recipes and applied spatial events where controls move during recording; immutable replay/render of all channels. Controls outside the recorded event contract must be frozen visibly. |
| Monitor | Hardware channel mapping or an explicit stereo monitor downmix. Monitor volume/layout never changes the saved design or rendered master. |
| Export | Same-as-design default; correct explicit conversion, interleaved WAV layout metadata, split-mono option, actual order labels and durable receipts. |

For mono point sources, LFE send starts off; existing LFE program material remains an identifiable routed channel. LFE send/filtering, monitor bass management and output program content need distinct policies. Do not silently add LFE gain to exported files.

### Preserve old takes exactly

The current recipe hash includes `designMixToJson()`. Unconditionally adding layout fields to that serialization would change old manifest-4 take hashes and could invalidate receipts. Keep legacy canonical serialization and processing semantics versioned; introduce a new explicit multichannel contract. Do not reinterpret `stock-takes-v1`. [Recipe serialization](https://github.com/Tekie79/Sound-Plus/blob/3609486e6b1313f2ad80181022c04e0db372657f/src/PerformTake.cpp#L36), [hashing](https://github.com/Tekie79/Sound-Plus/blob/3609486e6b1313f2ad80181022c04e0db372657f/src/PerformTake.cpp#L181).

### Completion gate

Use unique impulses/tones per channel to prove identity through import, edits, every stock insert, resampling, take capture/replay, rendering and reimport. Test named 5.1 variants, 7.1 and the chosen discrete maximum, not just a six-channel file header. Compare pre-monitor capture/replay/offline PCM on every channel. Verify old fixture hashes/receipts and legacy sound. Check layout-aware output identity when a take has stereo and surround renders. Measure waveform memory by bytes as well as entry counts.

Native hardware surround playback, human listening and receiving-app persistence are separate qualification gates. Stereo speakers can qualify an explicit stereo monitoring path and offline multichannel PCM; they cannot establish physical 5.1 playback. Resolve delivery remains unqualified until actual import/drop, channel interpretation, timeline placement, save, SoundPlus exit and receiving-project reopen succeed.

## 7. Following milestones

**v1.5 — Source Pools and Repeatable Variations:** optional bounded alternates per layer, stable entry IDs, source layout/range/routing, small waveforms, explicit Next/Previous, independent source-choice locks and deterministic seeded resolution. Freeze the resolved source and mapping before recording; later pool changes cannot alter a take. Missing selected sources must not silently reroll. Disable source switching during Arming/Recording/Finalizing until a later voice/event design explicitly supports it. Update the existing [Source Pools brief](https://github.com/Tekie79/Sound-Plus/blob/3609486e6b1313f2ad80181022c04e0db372657f/docs/SOURCE_POOLS_NEXT.md) accordingly.

**Later — Plugin hosting and delivery expansion:** negotiate each effect's accepted bus layouts; show unsupported layouts and explicit adapters. Qualify additional channel widths and formats from evidence. Keep channel-based speaker/discrete support distinct from separate object-audio or Ambisonic processing implementations. Continue the independent Resolve receiving-project persistence gate.

The first v1.4 commit should fix R10; subsequent commits should deliver the complete declared multichannel workflow. The companion Codex prompt contains the implementation and verification contract.
