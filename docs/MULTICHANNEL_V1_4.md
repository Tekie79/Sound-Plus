# Multichannel Designer v1.4

The Library remains project-free. New Designer documents use an explicit Stereo bus; existing documents retain their legacy processing until the user deliberately chooses a Design layout. Design, Monitor and Export are independent contracts. QA evidence and qualification boundaries are in `MULTICHANNEL_V1_4_REPORT.md`.

## Ordered layouts

These are both internal role order and new WAV sample order. The writer queries JUCE channel indices and adapts by role; it never assumes a factory's description is the interleaving order.

| Stable ID | Ordered channels |
|---|---|
| `mono-v1` | C |
| `stereo-v1` | L, R |
| `lcr-v1` | L, R, C |
| `quad-v1` | L, R, Lb, Rb |
| `5.0-v1` | L, R, C, Ls, Rs |
| `5.1-side-v1` | L, R, C, LFE, Ls, Rs |
| `5.1-rear-v1` | L, R, C, LFE, Lb, Rb |
| `7.1-v1` | L, R, C, LFE, Lb, Rb, Ls, Rs |
| `discrete-N-v1` | D1 through DN; no speaker positions |
| `unknown-N-v1` | D1 through DN; unidentified source order |

N is 1–16 for design/processing/export and 1–64 for source admission. A count of six never implies5.1. A recognized WAVE channel mask supplies roles and takes precedence over free-form BWF text. A valid SoundPlus BWF declaration must include its exact ordered role list and matching source count; it supplies discrete/unmasked identity. Invalid or unsupported explicit masks stay unknown. Confidence records `container-mask`, `producer-declaration`, `conventional` (mono/stereo), or `unknown`. The detected identity and the user's explicit source interpretation are separate persisted fields. A layer separately freezes its interpretation, selected physical channel indices and routing. Catalog interpretation edits apply to future layer creation; they do not mutate an existing take.

The full Library uses compact `5.1S` / `5.1R` / `DN` / `?` channel badges; `*` marks an explicit override. Accessibility names and channel-cell tooltips retain complete detected/confidence/override/order details.

A wider-than16 source newly dropped into a layout-aware design remains unresolved until a supported pair/subset is selected. Legacy conversion preserves its already explicit pair. All source channels remain available to Library pair audition and paged per-channel detail. An unknown source can be auditioned as a pair or analysis mean immediately; full-layout monitor requires a deliberate interpretation. Relink verifies source stamps/content and preserves interpretation; changed bytes require explicit replacement.

## Routing and spatial controls

Design layout is a compact selector. Routing / Spatial Panner opens the selected layer's contextual matrix. Rows identify interpreted roles and original physical indices; columns identify bus roles. Set route commits trim, polarity and mute. Role routing restores the default conversion. Route by index is an explicit alternative for discrete/unknown sources. These are mix edits and affect exports. Monitor solo is a separate listening operation.

Matching beds retain roles. Mono→Stereo uses equal-power simple pan; stereo balance retains the existing behavior. Spatial Panner is optional for mono/stereo point/image sources on Quad,5.0,5.1 or7.1 horizontal speaker layouts; it is not automatically applied to a surround bed. LCR/discrete layouts use routing rather than a fictional circular panner.

Spatial azimuth is clockwise, with L−30°, R+30°, C0°, Lb−150°, Rb+150°, Ls−90°, Rs+90°. The two adjacent full-range speakers receive cosine/sine equal-power gains, including wraparound. Azimuth follows the shortest path and smooths over10ms. Stereo channels are two images at azimuth±width/2, each scaled by1/√2; width smooths over10ms. Divergence blends toward all full-range speakers and renormalizes energy. Center participates as a real speaker; divergence0 uses adjacent speakers only. LFE never participates in this panning law.

A mono/stereo point's optional LFE send defaults off (−90dB). An explicit send sums the input mean, applies a separate second-order lowpass (default120Hz,20–200Hz), and applies the chosen send gain. Existing bed LFE is preserved by role without an automatic filter or gain boost. Monitor downmix omits LFE; this is not bass management.

Conversion coefficients:

- Identity/role-preserving conversions: unity on matching roles; unsupported/dropped-role conversions require explicit routing.
- Mono→Stereo:1/√2 to each side at center.
- Stereo→Mono: (L+R)/2, including legacy behavior.
- Surround→Stereo: L/R unity, C and same-side surround/rear1/√2, followed by a shared0.5 headroom coefficient. LFE0. Surround→Mono averages that stereo pair again.
- Discrete/unknown conversion never guesses roles. An explicit same-sized discrete declaration can preserve indices; other conversions require reviewed routing.

Pan/balance is disabled when the matrix or Spatial Panner owns position; incompatible existing macro pan mappings remain visibly unresolved instead of being silently remapped. Available new macro targets follow that policy.

Matrix coefficients smooth over5ms during supported parameter adoption. Structural routing/layout/LFE/divergence changes prepare replacement graphs. Quick live seeks retain the documented bounded approximate effect-history policy. Use Accurate audition from playhead for judging long delay/reverb tails; offline rendering uses full history.

## Stock processing and limits

All declared1–16-channel buses support linked edits, trims/slip/placement, reversal, rate/pitch, fades/custom curves, gain, mute/solo, duplication/variations, selection rendering and resampling. Channels remain sample-aligned. Fade icons retain the current10-point subdued corner styling.

| Processor | Layout-aware policy | Legacy policy |
|---|---|---|
| Gain | One smoothed gain applied to every channel | Preserved |
| HP/LP filters | Linked coefficients, independent histories per channel;32-frame invariant coefficient cadence | Preserved historical contract |
| Delay | Independent channel buffers/taps; shared time, feedback and wet controls; no crossfeed | Preserved |
| Reverb | One independent mono JUCE reverb per channel, linked room/damping/wet; no spatial crossfeed or stereo-width control | Original stereo reverb retained |

This is linked independent reverb, not a spatial room model. The processor bus boundary validates ordered input/output contracts; a future host must negotiate an explicit adapter for differing buses. AU/VST3 hosting is not implemented.

Legacy layer/insert limits remain. New graphs additionally admit at most2048 input×output routing cells,512 channel-insert work units,128MiB estimated delay histories,256MiB estimated prepared buffers and16 spatial targets. These are deterministic admission ceilings, not a promise that all maxima combine at real-time speed. Buffers/history/FIFO/routing are prepared on workers. The callback consumes prepared PCM, applies a fixed monitor matrix/gain, copies bounded peaks and advances the capture clock. It performs no graph construction, file/database I/O or JSON parsing.

The shared waveform pool retains48 full-source entries,160 display summaries (≤2048 peak pairs each),96 geometries and40 demands per owner. Each selected detail page requests at most16 channels, with explicit channel/layout/revision/detail cache identity; changing pages replaces demand. Full-source detail decoding itself is bounded to2048 pairs per channel request. Resident budgets remain global across owners; detail does not reserve a full peak cache for every channel. Six UI owners share those ceilings. Loading/offline/changed/error states remain explicit.

## Monitoring and recording

Default monitor is labelled Stereo downmix for named designs and pair1/2 for discrete designs. Extra hardware outputs are silent until explicitly assigned. Monitor menu supports source-channel solo and explicit device output assignment/count; unavailable physical outputs are not offered. A missing bus index is silent. A newly discovered device resets the monitor map to Stereo/default; it never inherits another device's channel-index assignments. Rate/buffer changes on the same device retain its map. Monitor gain/solo/assignment stays outside recipes/receipts and never changes offline content. Hardware output-count changes are disabled during capture. Device interruption follows the existing Arming/Recording/Finalizing failure latch and cannot publish an affected take as Ready.

Before Arming, a take freezes source interpretations, bus roles, routes, panner defaults and DSP policy. New takes use `stock-layout-takes-v1` and typed `source-blend-spatial-v1` controls. Spatial targets are stable indices in the immutable layer recipe (whose IDs remain hashed). Events contain finite azimuth/width values and are recorded only when consumed audio adopts them. Layout/routing/source/divergence/LFE edits are disabled during Arming/Recording/Finalizing; only captured Source Blend/macros and enabled spatial motion remain editable during Recording.

Limits stay120seconds,8192 applied events and16takes/document. The fixed event queue now occupies3,866,624bytes (472bytes×8192), allocated before capture. New spatial data shares the same event capacity/overflow semantics. Arming also reserves a conservative worst-case event JSON allowance under the64MiB document limit. This can reject recording a very large design before allocation/publication. Existing R5 metadata, R6 output-context and R7–R9 source/device/finalization guards remain.

## Export and durable outputs

Export defaults Same as design. The settings disclosure shows actual ordered roles, rate/depth, tail, normalization, dither and deliberate conversion. Layout-aware integer WAV quantization uses symmetric nearest rounding and deterministic independent-channel TPDF seeds170526+7919×channel. Normalization uses a single linked gain to−1dBFS across all channels; it never normalizes each speaker separately. Receipts record per-channel peaks/clipping and the actual policies.

Interleaved WAV writes correct named channel masks; discrete WAV uses mask0 plus an explicit BWF layout/order declaration. RIFF sizes approaching4GiB are rejected before processing (1MiB container allowance). RF64 is not qualified. No truncated file is published.

Split mono stages all role-named stems, a durable interleaved `Mix.wav` Print master, and `output-set.json` in one owned hidden directory. Every member is finalized, hashed and synced before one directory rename publishes the set. Cancellation/failure removes only that operation's stage; earlier durable outputs remain. The receipt binds each stem role/path/hash as one set. Drag sends stems; Print imports `Mix.wav` with its complete multichannel interpretation. Missing members invalidate readiness.

The finalized-output selector names the actual file/set and layout. Changing the next render preset cannot relabel an earlier render. Selected receipt identity survives save/reopen. Print and drag use the selected finalized output under the existing context guards. Render Selection uses the current primary layer with placement reset to0, mute/solo cleared, layer duration and master processing; it is not a time-range crop or multi-selection render. Render Take exports the full immutable performance.

## Compatibility and migration

Manifest1–4 serialization and `stock-takes-v1` DSP remain version-specific. No new layout fields are injected into old canonical recipes. Selecting Design layout deliberately converts the active Live mix; old takes and output receipts remain immutable. New features use manifest5 / `stock-layout-v1`, routingVersion1 / `independent-linked-v1`, receipt channel provenance version1, SQLite schema6 and private IPC6. Older applications reject newer document/catalog contracts rather than reinterpret them. Source media is never rewritten. Keep backups if moving a catalog back to an older application.

IPC remains a bounded65536-byte single-writer frame contract. Nonblocking writes handle partial progress and transient backpressure for at most250ms on non-audio workers; a timeout fails the connection rather than continue after a partial frame. ASCII JSON escapes preserve Unicode source names across arbitrary read boundaries. This fix was prompted by a new measured pressure failure; it does not retroactively diagnose historical restart observations.

Source Pools follows this foundation; see `SOURCE_POOLS_NEXT.md`. Pools, plugin hosting, object/Ambisonic delivery and Resolve automation remain outside this milestone.
