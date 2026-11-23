# Four-pad Perform v1

This follows the hosted-automation milestone. Original `DesignMix.performance` is Pad A, unchanged for historical single-pad projects. Up to three value-owned extra pads have stable UUIDs. Pad selection is editor metadata: it never solos a pad or changes the shared transport. The adjacent + adds B, C and D; it disappears at four.

## Sources, controls and routing

Each pad has four source assignments, XY, four macro controls, level (−90 to0dB), mute and solo. Explicit source ownership is exclusive: duplicate the ordinary layer if the same media must be processed independently by two pads. Pad A keeps unassigned audio layers. Other pads own their assigned layers; cross-pad macro writers are rejected. Extra-pad macro target choices are limited to their own sources; historical Pad A Master mappings retain their global meaning. Existing macro target families are unchanged. Advanced/hosted parameters remain available in the ordinary track automation selector.

All pads resolve into one existing graph, source clock, Track/Group/Master routing and PDC. Shared MIDI instruments stay on that timeline, outside the four audio-source slots. Muting, soloing and tab selection are independent. No plugin is rebuilt for each pad point. Typed controls use fixed capacity (A plus three extra pad values), evaluated on the globally anchored32-frame grid plus exact keys. Block partitioning does not shift this grid. No new work is introduced into the native device callback.

There is still exactly one collapsed, Volume-first automation lane per owner. The existing Master lane exposes14 fields per pad: XY, four macro values, four enable states, blend, level, mute and solo. All hidden Read curves continue processing. Pad actions can show the chosen pad in that lane or switch its recorded curves' Read state, without deleting points. Read remains authoritative over ordinary manual edits; disable its Read or record another pass to replace motion.

## Recording and editing

Choose Live Design, select the pad, and Record. The selected pad identity is captured at start. Preparation disables only that pad's previous Read curves in the frozen seed; other pads keep replaying. One applied-frame control stream records actual producer-applied values. At completion, changed controls become independent editable curves in the existing Master lane, while the complete frozen take remains available. Unchanged controls do not gain unnecessary curves. Recording B cannot rewrite A's points or IDs. One Undo reverses the completed pass; Save/Reopen retains both curves and immutable takes.

Existing120second/8192applied-event/16take limits remain. Editable curves share the project8192point budget and2048points percurve. Materialization validates a copy before adoption. If editable-curve capacity is exceeded, existing curves remain untouched, the completed frozen take is retained, and an explicit message explains the limit. Shorten or simplify motion before another editable pass. Touch writing, Audio input capture, MIDI capture and Perform recording remain distinct modes.

## Rendering and compatibility

Render Design prints the combined live design, including all enabled pad curves and shared MIDI. Pad actions → Render this pad prints its owned audio through the existing routing and common Master. It excludes shared MIDI and ignores other-pad Solo states, while respecting selected-pad level/mute. The finalized result uses the existing durable output selector/export/drag path. Common nonlinear/stateful Master processing means separately rendered pad outputs need not sum to the combined output.

Render Selection retains the complete pad assignments/motion for correct blend weight, mutes unselected clips, rebases the selection and bounds its duration. The existing hosted pre-Master stem policy remains: nonpad Master processing/automation is excluded; pad motion is retained. Selection does not mean a timeline marquee.

Manifest18 / `multi-pad-perform-v1` and recipe9 / `multi-pad-perform-takes-v1` / `one-clock-independent-pad-curves-v1` are selected only when additional pads or nonneutral Pad A level/mute/solo are used. Editor tab metadata is omitted for A. Historical manifest/recipe branches1–8 remain intact; neutral single-pad JSON, hashes and PCM remain unchanged. New recipes preserve hosted/native/MIDI identities, source dependencies, processing policy and frozen state.

## Verification boundaries

`multi_pad` supplies independent constant-amplitude mix/stem/level/mute/solo PCM oracles, all three rates and five block partitions, consumed-stream versus frozen PCM, saved identities, actual view commands and capacity handling. `multi_pad_device` records A then B through the real CoreAudio output, checks unchanged graph/generation and no added underruns, retains independent curves, renders the actual session pad and Render Selection and compares finalized PCM. Native pointer/keyboard checks and screenshots are in the milestone report. These are not human listening, acoustic latency, physical surround or receiving-application delivery qualification.
