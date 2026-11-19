# Next creative slice: recorded Perform takes

Prepared during the v1.2 reliability pass. This is a design brief, not an implemented recording feature. Keep the existing Designer/Library document, transport, resolver, processors and durable output pipeline.

## User workflow

Record → move XY/macros → stop → audition → keep/name → render. Existing static snapshots remain static recipes. A take is a frozen starting recipe plus applied control events on its playback timeline; a delivered WAV is a separate immutable output with a receipt. Do not add a record button before this workflow functions.

## Data and identity

- Give each take a stable ID and name. Freeze the full starting mix, per-mix blend law, stock-DSP compatibility tag, source identities/stamps, initial controls, project sample rate, timeline origin and effect-history policy.
- Store ordered events for existing XY and four macros, including explicit enable/bypass changes. Identify targets by stable control identity. Preserve a monotonic sequence for events sharing a frame; do not derive identity from UI labels or row positions.
- Record the controls actually adopted by the shared engine, with their application frame and smoothing semantics. UI dispatch time alone is insufficient: coalescing, lookahead and pending generation adoption can change what reached PCM. Add a bounded engine-to-document event channel outside the real-time callback; explicitly handle overflow as an incomplete take instead of silently losing movement.
- Keep the take's recipe immutable after completion. Editing the current design or recalling another snapshot must not alter a kept take. Undo may remove/restore a take as a document action; normal source relink must retain provenance and revision checks.
- Add an explicit document/reader version gate. Older readers must reject unsupported take playback rather than ignore time-varying audio fields. Keep current static document versions unchanged when no take is stored.

## Playback and output

Replay through the same performance resolver and stock processors, scheduling events on integer design frames. Live replay and offline render must apply the same events in the same order, including bypass and smoothing. Seeking uses the established approximate live-history policy; Accurate audition/render rebuild complete history from the take's origin. Monitor volume stays excluded from renders.

A render receipt records the take ID, frozen recipe/event hashes, processing version, source references and render settings. Keep take, snapshot and WAV actions visibly distinct. Print-back continues creating an ordinary referenced layer from a finalized permanent WAV.

## First acceptance tests

1. Record XY plus all four macros, including bypass/reset, and replay the applied event sequence deterministically.
2. Compare live pre-monitor replay with offline PCM; use an impulse/delay tail case and an event exactly on a block boundary.
3. Save/reopen kept takes, then modify the editable recipe; replay/render still uses each frozen starting mix.
4. Cancel, undo, document switch, pending seek adoption, source offline/relink and event-channel overflow leave clear recoverable states.
5. Preserve selection, search, waveform scheduling and active playback under recording; verify memory/event limits and no callback allocations or I/O.
6. Native record/stop/audition/name/render workflow, appropriate human listening, and separate receiving-app delivery qualification.

Start with mouse/keyboard XY/macros in stereo. Sampler pools/triggers, MIDI, hosted effects, surround and picture spotting remain later slices.
