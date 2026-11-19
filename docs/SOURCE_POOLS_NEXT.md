# Source Pools — next milestone brief

Prepared after the v1.4 multichannel foundation; no Source Pools implementation is included in this milestone. Keep the shared JUCE session, source resolver, DSP and take architecture.

## First useful slice

A layer with one source stays the simple default. An optional pool holds alternate recordings with stable pool-entry/source IDs, ranges, channel mapping, source revision and content fingerprint. An explicit selected entry determines the layer's waveform and playback. Next/Previous changes that selection before generation or Record; the selected-source name and availability must be clear without another toolbar row.

Seeded choice resolves deterministically before a variation or take starts. Persist the seed, algorithm/version and resolved entry; never silently choose another recording after a missing-source error. Variation locks independently preserve the chosen source and existing parameter locks. Undo/Redo, save/reopen, references and content-verified relocation retain entry identity and range provenance.

Freeze the resolved choice into the immutable take recipe before Arming completes. Existing take playback, metering, rendering and receipt guards then use that exact resolved snapshot. Store pool origin/entry provenance for explanation without allowing later pool edits to alter recorded audio. Preserve current static and manifest-4 documents; define and test any schema migration before accepting new files.

## Acceptance outline

- One-source documents sound and behave exactly as before; no pool configuration is required.
- Selected waveform, Next/Previous, Undo/Redo and save/reopen agree with the actual source.
- Equal seed + version + ordered pool gives the same resolved choice; locked choices remain fixed.
- Record freezes source ID/range/fingerprint; later pool edits do not change replay or WAV hashes.
- Missing/changed/relocated files have explicit availability and verified recovery, including external-drive cases.
- Native Library-to-layer assignment, Edit/Perform, rendering and durable Print-back remain functional at minimum and normal sizes.

## Explicit capture boundary

The current XY/macro event format cannot represent sample triggering or a source change during capture. Disable/defer pool switching while Arming/Recording/finalizing in the first slice. Live triggering needs new typed events, ordering/coalescing rules, source-readiness guarantees, overlapping voice lifetime, cut/crossfade behavior, effect-history ownership, consumed-frame trimming, replay/offline parity and a compatibility gate. Do not add an unrecorded source switch to the existing control state. Design that separately before enabling it.

## Multichannel contract for the next slice

Every alternate must carry its stable entry/source IDs, source revision/fingerprint, detected ordered layout and confidence, explicit interpretation override, range, selected physical channels, and reviewed mapping to the layer and design bus. A six-channel count is insufficient to declare 5.1. Wider-than16 sources need an explicit supported subset; unresolved entries cannot become the active recording source.

Seeded resolution persists the chosen entry ID, source range, interpretation and mapping alongside its algorithm/version. Missing or changed selected media fails explicitly; it never silently rerolls. Inactive unavailable alternatives do not block a healthy selected entry. Freeze the resolved fields into the versioned recipe before Arming; later pool or bus edits cannot alter existing takes or their receipts. Retain `stock-takes-v1` canonical identity and the new `stock-layout-takes-v1` ordered-channel contract.

Source switching remains disabled during Arming, Recording and Finalizing until a typed source/voice event model, bounded preparation and consumed-frame replay/render tests qualify it. Keep the Source Blend pad's four contributions separate from speaker positioning. Plugin hosting, object delivery, Ambisonic transforms and video/Resolve automation remain outside this slice.
