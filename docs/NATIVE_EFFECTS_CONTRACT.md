# Professional native effects v1

## Compatibility boundary

Legacy Gain, Filter, Delay and Reverb retain their original four doubles, bit locks, `parameter-0`…`parameter-3` automation identities, canonical JSON and DSP. Native v1 is a separate `StockEffect.nativeVersion` plus definition-keyed values and locks; it does not enlarge or reinterpret that array. Each new type has an explicit version, stable parameter IDs, real-unit bounds/defaults, interpolation/discrete metadata and latency/tail policy. Canonical serialization writes IDs in definition order, validates complete typed state, and rejects unknown IDs/types/versions/policies. Failed preset parsing cannot partially replace current settings.

New effect documents require manifest15 / `native-effects-v1`. New native-containing takes require recipe6, `native-effects-takes-v1` and `native-effects-v1-fixed-latency`. Existing manifest1–14 and take recipes1–5 remain byte/hash compatible. Native state is included at each existing Clip-source, Track, Group and Master insert owner, retaining authoritative mixed stock/hosted order. Copies rekey instances; parameter targets use instance ID plus definition ID, never slot name or row number.

All supported live parameters appear in the existing one collapsed-by-default automation lane per processable owner. Hidden read-enabled curves remain active. Lookahead is deliberately structural/read-only in the automation chooser: changing it requires graph preparation and latency realignment. It is saved/preset-editable; continuously changing graph latency is not promised. Other native parameters use the existing project-anchored32-frame scheduling segments and declared smoothing. Controls and preset edits are undoable existing session commands. Locks constrain seeded variations; explicit manual edits/preset loads remain deliberate.

## Preparation and availability

The definition-driven inspector has collapsible groups, named units, numerical fields, logarithmic ranges, discrete menus and per-parameter variation locks. Its parent Inspector owns scrolling. Preset files are bounded local `.soundplus-effect` metadata, with no media/owner IDs. Available DSP definitions alone appear in Add Effect; an unavailable processor fails explicitly and is never silently omitted.

OrderedRack includes native and hosted latency in its aligned dry whole-chain bypass and automation prefixes. Native instances prepare state/buffers off the device callback. The existing graph handles parallel source/Track/Group/Master PDC, priming once and bounded output tails. The device callback continues to consume prepared PCM; it does not instantiate DSP, parse JSON, load presets, allocate graphs or do file/database I/O.

## Library availability correction

Offline full files and saved regions remain visible in All Sounds, Favorites and Collections with their existing IDs, ranges and membership. Favorite and organization commands remain available. Play, Process with Plug-in, Add to Designer and internal audio delivery require both the asset and its source root to be online, plus current/non-unresolved range identity. Add to Designer is deliberately disabled for offline sources; this increment does not create offline placeholders. Actual action callbacks repeat the availability guard, so a stale enabled action cannot dispatch missing media. Reconnect restores availability without replacing virtual references.

## Qualification boundaries

Stage A establishes persistence, UI/automation adapters and graph latency plumbing before new DSP is enabled. Algorithm-specific contracts and executed numerical/PCM evidence follow in the stage reports. Peak limiting must not be labelled true-peak without a separately verified reconstruction/oversampling mode. Human listening, physical surround and receiving-app persistence remain separate from numerical/native checks.

## EQ v1

Four independently enabled Bell/Low Shelf/High Shelf/Notch bands have stable enable/type/frequency/Q/gain IDs, plus enabled HP/LP stages with frequency/Q and one to four cascaded biquads (12/24/36/48dB per octave), and output trim. Slopes describe repeated user-Q sections, not an undocumented Butterworth alignment. Shelves use resonance Q. Requested frequency is retained in metadata and clamped to49% of the actual processing rate. Audio roles/channels remain independent. Ten-millisecond parameter smoothing and twenty-millisecond coefficient interpolation preserve state; coefficient targets update on a continuous32-sample phase independent of caller block partition. Individual bypass ramps aligned dry/wet while wet history continues.

The transfer functions follow the [W3C Audio EQ Cookbook](https://www.w3.org/TR/audio-eq-cookbook/). Independent tests use the separate JUCE coefficient factory, scalar Direct Form I impulse recurrence and complex z-domain response/phase, rather than the production coefficient/state implementation. Fresh tests cover all six filter modes,44.1/48/96k,1/32/61/257/512-sample partitioning and rapid/extreme frequency/Q sweeps. Stage B maximum measured impulse error was9.35295e-8; the four-suite compatibility/automation/view gate passed in6.37s. Native UI inspection and the final whole-application gate remain Stage D acceptance items.
