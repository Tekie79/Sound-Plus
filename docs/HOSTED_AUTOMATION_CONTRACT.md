# Hosted parameter automation v1

Work begins at `3da199dfe3e48db19e08daec88c420962ccfb8bc`. This is a staged contract: implementation and qualification status is recorded in HOSTED_AUTOMATION_CHECKLIST.md; a declared policy alone is not a pass.

## Identity and compatibility

A saved binding contains the owning Track/Master and optional source Clip or Instrument UUID, insert UUID, plugin format/class identity, binary fingerprint, and stable native parameter ID. Display names, ordering and native indices are metadata, never identity. A processing request uses a SHA-256-derived 64-bit transport token for the exact slot/class/fingerprint/native ID, after full string binding resolution in the prepared graph. The helper resolves the native index against its prepared IDs and validates the token and automatable flag. Unknown or mismatched targets must remain preserved and unavailable, without name/index fallback.

HostedRackExchange SPR5 is transient IPC, not a document migration. Empty hosted automation has no effect on document or take policy. Existing document encodings and recipes 1–7 must remain unchanged; the new saved policy is reserved for actual hosted curves and is not enabled by inserting a plugin or selecting a parameter.

## Bounded events and threading

One producer request carries at most 16,384 events, 512 audio frames, 16 named audio channels, eight slots and the existing bounded MIDI packets. An event contains slot/native index, identity token, normalized finite value in [0,1], monotonically increasing sequence, project source frame, processing-input frame, sample offset and policy0. Offsets are nondecreasing and inside [0,frames); inputFrame equals request inputFrame+offset. The right boundary belongs to the next block. Sequence is lease-scoped and cannot repeat within that producer lease. Invalid input fails closed before plugin processing.

Policy0 applies a normalized step immediately before the addressed sub-block. The graph's continuous interpolation is sampled on globally anchored 32-frame boundaries plus exact keyframes; discrete parameters hold. Splitting external render requests must not shift that grid. The isolated helper performs one message-thread dispatch for the entire block, splits at parameter offsets and rebases MIDI into each slice. It retains the same prepared processor, state, delay lines and buses. No reset/recreation occurs per point. Events address only their slot. Each slot receives the original instrument MIDI according to existing rack semantics (following effects receive no instrument MIDI).

This is **host-controlled sub-block automation**, not a universal claim about vendor internal sample accuracy. Vendors can smooth, quantize or defer updates internally. JUCE8.0.6 hosted parameter setters are called on the helper message thread; ordinary unautomated live DSP keeps its existing serialized PCM-worker path. DSP preparation, IPC waits, plugin calls, event compilation, JSON and state capture remain outside the actual device callback.

## State precedence and overlays

Required precedence: saved opaque/program state -> explicit unautomated model/native manual intent -> Read overlay -> qualified Touch for its armed target. Reading a curve must not rewrite saved base parameter values, programs or opaque state. A live worker must preserve a separate base snapshot and restore it before coherent persistent capture, then reinstate Read values without resetting DSP history. Host automation must suppress listener notifications as user gestures. Read-off/removal restores manual base state. Frozen processors reject external manual changes.

Touch captures owner+full parameter identity at gesture begin, independently from the visible selector. Read return is 100ms and one pass is one Undo transaction. Native editor gesture recording requires explicit begin/value/end provenance and actual processing-frame acknowledgement, including close/stop/crash and late callbacks. Latch/Write/Trim/Preview/Punch remain unavailable until independently implemented and tested.

## Timing, PDC and frozen output

Project frames address output samples after each slot's declared latency, with upstream latency and named routing included. Processing-input frames identify the corresponding producer boundary. MIDI offsets remain source-clock events. Seek/reset barriers invalidate previous leases and pending events; latency/bus/parameter-schema changes require explicit reprepare and cannot silently certify a render.

Hosted automation requires a new immutable frozen policy recording full base state, binary identity, curves, routing/PDC, source/IR dependencies and event interpolation policy. Original recipes1–7, hashes and PCM stay authoritative for old takes. Live edits cannot modify a retained take. Tail extension is explicit and automation recording is separate from microphone/MIDI capture.

## Verification obligations

Independent owned VST3 and AUv2 effect+instrument PCM/parameter oracles, rates44.1/48/96k, blocks1/32/61/257/512, duplicate names/instances, stable-ID drift, reorder/removal, exact event boundaries, stateful tails, program/opaque recall, hidden curves, PDC, fixed takes and failure recovery. Native editor gestures and minimum1040×680/desktop UI checks are distinct from automated component tests. Resource measurements, device output, physical paths, human listening and receiving-app persistence are reported separately.

## Implemented Read and frozen encoding

Manifest17 / `hosted-automation-v1` is selected only when a nonempty curve carries a hosted binding (also when Read is disabled, so its identity remains preservable). The optional binding field is omitted from old/empty curves. Registry entries capture full ownership/class/fingerprint/native identity only on the first point. Missing/mismatched bound Read curves fail graph preparation; disabling Read preserves their data without certifying unintended output. Split explicitly clones source bindings to its new clip and slot UUID; ordinary copy does not inherit another instance's curve.

Recipe8 / `hosted-automation-takes-v1` adds `normalized-32frame-offset-overlay-v1`, retaining complete mix/base state, binary identities, curves, routing and source hashes. Recipes1–7 keep their original serialization branches. Recipe8 retains the compensated pipeline input dependencies used by recipe7. Tests compare actual consumed pre-monitor PCM with frozen replay at equal and converted output rates and verify finalized render receipts and immutable Save/Reopen hashes. Stereo-monitor downmix is a separate stage, excluded from that raw PCM equality.

Effect curves use each slot's output-aligned source clock (stage input minus own declared latency). Instruments use the note/input source clock, before their declared output delay, so automation and MIDI notes/CC feed the instrument together. Both are explicit host schedules; a plugin applying a control elsewhere internally can smooth, quantize or delay its response. The owned effect intentionally quantizes real gain to0.01; its continuous oracle independently accounts for that quantization. UI normalized values are not a promise of unlimited plugin resolution.

Live Read uses separate atomic manual-base and overlay values. Model and native manual intent update base; host Read writes suppress user-gesture notifications. Read-off restores base without resetting DSP history. Coherent Save captures base values/opaque state under the existing control barrier, then reinstates the overlay. Only VST3 uses a zero-length process to settle its parameter dispatcher. AUv2 setters are synchronous; zero-frame AudioUnitRender can cache the render timestamp and suppress the next real block, so AUv2 state capture must never use that flush. Owned retained-instance tests exercise first-block Read, manual edits underneath Read, capture acknowledgement, Read-off and fresh-helper restoration for both formats.

Owned AUv2 test bundles are registered inside the isolated helper only when `SOUNDPLUS_QA_LOCAL_AU` explicitly names that bundle. They are never installed into user/system plugin folders. Production registered-AU discovery remains unchanged.
