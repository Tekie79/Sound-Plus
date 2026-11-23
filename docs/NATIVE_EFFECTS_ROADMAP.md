# Effects roadmap after advanced delay/reverb v1

Delivered in this increment: separate versioned native definitions/state, four-band EQ with HP/LP, compressor, expander/gate, sample-peak lookahead limiter, transient shaping, Tanh/cubic saturation, editable effect metadata presets, existing-owner inserts and automation, latency alignment and frozen-take recipe6. Historical Gain/Filter/Delay/Reverb remain unchanged.

Delivered next: Advanced Delay, Room/Plate/Hall Algorithmic Reverb and referenced/partitioned Convolution Reverb, manifest16/recipe7, shared tempo/tail/IR controls, same collapsed automation lane and targeted independent PCM/device qualification. See [advanced report](ADVANCED_EFFECTS_MILESTONE_REPORT.md) and [contract](ADVANCED_EFFECTS_CONTRACT.md).

Pending increments, with separate implementation and qualification:

- Further reverb: gated/reverse, more IR matrices or non-uniform/zero-latency processing require separate contracts and measurements.
- Creative transforms: granular, pitch/formant/time, spectral freeze/blur, stutter, bitcrush, ring modulation, chorus/flanger/phaser/frequency shifting. Optional oversampled saturation and true-peak limiting require separate anti-alias/reconstruction measurements.
- Hosted VST3/AUv2 parameter automation; other formats only when feasible and legally qualified.
- Perform A–D tabs and per-pad non-destructive automation overdubs on one clock; no additional pad UI is claimed here.
- Short-event refinement beyond current coarse Find Similar windows.
- Optional external sound generation with provider-neutral interfaces, BYO credentials, provenance, explicit cost and user-controlled uploads.
- Human listening review, physical multichannel speaker/controller/DAC qualification, real Resolve import/placement/save/quit/reopen delivery and signed/notarized public release.

See [native v1 contract](NATIVE_EFFECTS_CONTRACT.md) for exact processing, bounds and exclusions, and [executed evidence](NATIVE_EFFECTS_MILESTONE_REPORT.md) for the measured scope.
