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

Optional MIDI-capable audio effects may run with empty MIDI input and discarded output; MIDI routing and instruments remain unsupported. Optional declared auxiliary buses are disabled only when accepted. Sidechain routing, automatic multi-mono, silent surround adaptation and cross-format substitution are not implemented. A mono insert before a surround panner stays mono. Master processing uses the actual ordered output bus. Physical surround monitoring, human listening and receiving-app save/quit/reopen remain separate evidence.
