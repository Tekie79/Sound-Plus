# Plugin qualification matrix — macOS arm64

This is a capability/evidence matrix, not a promise that every installed effect accepts every bus. Exact input/output role negotiation is mandatory. Initial hardware: M5 Pro / macOS 26.3.2 / JUCE 8.0.6, Release arm64.

| Format / path | Discovery | Processing / state | Editor | Status |
|---|---|---|---|---|
| Owned QA VST3 | Isolated standard/custom registry | Persistent rack, finite Process, state round-trip; named-layout regression suite | Same-instance live native editor, bidirectional controls and lifecycle regressions | Implemented; see [live report](LIVE_INSERT_REPORT.md) |
| Apple AUDelay AUv2 | Registered system component, resolved bundle fingerprint | Finite and persistent PCM / JSON state, Mono/Stereo/5.1 Side/Rear | Retained live editor architecture; actual AU native GUI remains instance-specific | Implemented; see final report evidence |
| LV50A 1.0.0 / OldSkoolVerb 2.14 VST3 | Installed native universal bundles, exact classes | Fresh Mono/Stereo Track/Master inline/state/render checks | LV50A live native changes/Undo/Save/render verified; OldSkoolVerb custom gestures not confirmed | See [live report](LIVE_INSERT_REPORT.md) |
| TAL-Reverb-3 2.3.0 VST3 | Exact installed identity retained in diagnostics | Duplicate native IDs rejected | Not insertable normally | Compatibility open; no renumbering |
| Youlean Loudness Meter 2 VST3 | Enumerates with advertised layouts | Signal-driven Track and Master known peaks/silence/reset, Stereo and accepted 5.1 Rear bus | Actual retained-instance meter responds; viewport/hit-test correction | Qualified to these cases; see [live report](LIVE_INSERT_REPORT.md) |
| Other vendor VST3 / AUv2 | Supported discovery adapters | Instance-specific negotiation required | Instance-specific | Not qualified by owned fixtures |
| AUv3 extension | Registered enumeration, asynchronous JUCE instantiation path | No available runtime fixture qualified | Unqualified | Open requirement |
| VST2.x | No adapter compiled | Absent lawful SDK/distribution path in this checkout | Unavailable | Required / blocked |
| Intel-only / 32-bit / Windows | No bridge | No processing qualification | Unavailable | Unsupported platform combinations |

| Layout | Owned VST3 exact arrangement | Apple AUDelay | Notes |
|---|---|---|---|
| Mono | Production regression | Production regression | True mono source-stage bus |
| Stereo | Production regression | Production regression | True stereo bus |
| LCR / Quad / 5.0 | Production regression | Not qualified | Never inferred from count |
| 5.1 Side | Production regression | Production regression | L R C LFE Ls Rs |
| 5.1 Rear | Production regression | Production regression | L R C LFE Lb Rb |
| 7.1 | Production regression | Not qualified | Explicit ordered roles |
| Discrete / other up to 16 channels | Only if adapter accepts exact roles | Not qualified | Pinned JUCE/VST3 restrictions remain; no relabeling |

Per-rack limits: eight slots, 512 parameters per slot, 16-channel fixed exchange, up to one second reported latency. Whole mix: at most 16 populated external racks / 64 slots / 24 MiB serialized settings. These bounds are not a real-time throughput guarantee.

Optional MIDI-capable audio effects may run with empty MIDI input and discarded output; Ordinary effect racks do not receive MIDI. Dedicated VST3/AUv2 Instrument support is specified in the MIDI milestone below. Optional declared auxiliary buses are disabled only when accepted. Sidechain routing, automatic multi-mono, silent surround adaptation and cross-format substitution are not implemented. A mono insert before a surround panner stays mono. Master processing uses the actual ordered output bus. Physical surround monitoring, human listening and receiving-app save/quit/reopen remain separate evidence.

Correction-source execution and vendor reruns are recorded separately in the [Live Insert Correctness and Recovery report](LIVE_INSERT_CORRECTIONS_REPORT.md). Owned native-schema/scale and sanitizer tests do not qualify a vendor custom editor or an additional format.

Final correction Release: 30/30 with actual vendor, AUv2 and device branches; final native LV50A and Youlean Track/Master Stereo + exact Rear front-pair reruns pass to the report scope. OldSkoolVerb native custom-input acceptance stays open after renewed unconfirmed gestures. See [new native evidence](verification/live-corrections/native/pcm-recomputed.json); no change to VST2/AUv3 capability status.

## Ordered-chain milestone

The [ordered-inserts report](ORDERED_INSERTS_REPORT.md) supersedes signal-order and rack-bypass descriptions: one canonical typed sequence per Track/Master, eight stock plus eight hosted slots, interleaving supported. Exact same OrderedRack recipe is used live/offline. Track remains before spatial routing, Master before master gain. The owner chain's summed hosted latency is limited to one second; whole-chain bypass preserves that latency and all slot flags, with a 10 ms crossfade and evolving wet history.

Owned noncommutative order and role-impulse tests cover all eight named layouts above, including both Side and Rear. A Stereo-only fixture on a true Mono owner rejects rather than adapting. This expands owned-fixture correctness evidence, not the set of qualified third-party formats/buses. New stock-only frozen take recipes can retain ordered bypass; that ordered-chain checkpoint still rejected hosted takes (superseded by the plugin-aware milestone below). VST2 dependencies/adapter, AUv3 runtime qualification, physical surround, listening and receiver persistence remain open separately.

Final ordered-chain build `d9f62b2`: required **31/31 / 264.24 s**, vendor/AUv2/device enabled. Fresh native LV50A tone/history/reorder/open Save/fresh recall render passes; Youlean Track Stereo, Master Stereo and accepted exact Rear front-pair high/Reset/low/silence reruns pass to the [report's stated scope](ORDERED_INSERTS_REPORT.md). OldSkoolVerb's final custom Dry drag/Dry Mute input remains unconfirmed: parameters/hash/RMS unchanged. No expanded VST2/AUv3 or physical/receiver qualification is implied.


## Plugin-aware recorded Perform takes v1

Manifest 10 / recipe 3 freezes canonical Track/Master plugin state and uses independent fixed-state processors. Existing Perform motion is recorded; native-knob automation is not. Owned VST3 fixture tests exercise all eight named layouts, PDC, noncommuting chains, body/tail boundaries, event replay and split output. Native Record/Stop/audition/Keep/name/Save/quit/reopen/render/print-back runs cover **LV50A 1.0.0 VST3 Stereo** and **Apple AUDelay 1.6.0 AUv2 Stereo** (the AU native case uses its dry setting). These results do not qualify other vendors/buses or universal deterministic replay. See [source identities, final gate, receiving evidence and limitations](PLUGIN_TAKES_REPORT.md). VST2 dependency/adapter, AUv3 runtime, physical surround and human listening remain open. Native file-drag acceptance is distinct from the verified Resolve API import/place/save/quit-SoundPlus/reopen test.

## MIDI Instruments v1

Dedicated isolated Instrument purpose precedes MIDI Track inserts. Qualified owned VST3 **SoundPlus MIDI Oracle 0.1.0 arm64** and installed **Apple AUMIDISynth 1.0.0 AUv2 arm64** accept exact Mono/Stereo main outputs. Actual MIDI offsets, PCM, state/program persistence, failure isolation and native editors are covered to the scope in [MIDI report](MIDI_MILESTONE_REPORT.md). Synth registrations are included in AU discovery. DLSMusicDevice is rejected because its additional output cannot be disabled. Unsupported instrument buses fail explicitly; this does not extend vendor surround, VST2, AUv3, physical MIDI or listening qualification. External Instrument automation remains read-only.
