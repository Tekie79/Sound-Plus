# VST3 finite-processing compatibility

Qualification target: Apple Silicon macOS, native arm64, JUCE 8.0.6. The shared Library/stock transport remains the only audio-device owner. These are finite-file jobs, not live plugin inserts. No automatic architecture bridging or plugin installation is performed.

## Executed fixture matrix

Owned `SoundPlus QA Effect` / `SoundPlus QA`, fixture version 0.1.0, native arm64 VST3, one main input/output, generic editor only. Exact bundle fingerprints, class descriptors and executable hashes are retained with the evidence. The narrower `SoundPlus QA Stereo` is the negative-layout control.

| Input/output arrangement | Fixture status | Identity check |
|---|---|---|
| Mono | Passed | Tagged impulse, gain/polarity, latency |
| Stereo | Passed | Tagged impulse, gain/polarity, latency, rate/block edges |
| LCR | Passed | Ordered L/R/C tags, mask, split roles |
| Quad | Passed | Ordered L/R/Lb/Rb tags, mask, split roles |
| 5.0 | Passed | Ordered L/R/C/Ls/Rs tags, mask, split roles |
| 5.1 Side | Passed | LFE and Side roles, latency, split master/Print |
| 5.1 Rear | Passed | LFE and Rear roles distinct from Side |
| 7.1 | Passed | Rear/Side role identity, mask and split-channel PCM |
| Discrete 4 / 8 / 16 | **Unavailable** | Pinned JUCE VST3 adapter rejects these untyped arrangements; never relabeled or downmixed |
| Native / Intel-only / universal owned binaries | Passed | Actual Mach-O slices: arm64 and universal accepted; Intel-only unavailable with no bridging |
| 5.1 through stereo-only fixture | Rejected as expected | Explicit incompatible-layout reason |
| 64 physical inputs | Explicit subset/Mono mean only | First/last-channel subset and independent arithmetic-mean oracle; no 64-channel plugin DSP claim |

The discrete limitation is in JUCE 8.0.6 `juce_VST3Common.h`: `getVst3SpeakerArrangement` has no representable arrangement for untyped discrete channels beyond its special single channel. The JUCE VST3 wrapper also requires a non-discrete default layout. This milestone does not patch that pinned dependency or pretend a named arrangement is discrete. Discrete stock Designer/render support is unchanged. Wider/other plugin arrangements must be actually accepted by the installed effect; count alone never qualifies them.

The worker reads bounded Mach-O thin/fat headers from the actual bundle executable; it does not label moduleinfo-only discoveries with the worker CPU. [Actual architecture regression](verification/vst3-v16/architecture-check.json) builds the owned effect separately for x86_64, verifies native rejection, and combines native/Intel slices into a private universal test bundle. It never installs or edits the original fixtures.

## Installed vendor effects

No third-party VST3 effect was found in `/Library/Audio/Plug-Ins/VST3`, `~/Library/Audio/Plug-Ins/VST3`, or the checked application tree. Native workflow verification therefore uses the owned QA effect selected from its build folder, without installing it. **A real installed vendor-effect pass is blocked by availability.** AU/VST2 effects in other folders do not qualify VST3. A user's installed effect is available only for its accepted arrangements and native architecture; cached discovery is not a guarantee that a licensed/interactive effect will process successfully.

## Qualified processing boundaries

- Exact main bus in/out; one effect, no sidechains, instruments or MIDI. State/parameters are restored off audio callbacks; no vendor editor is opened.
- Actual rate checks include 44.1/48/96 kHz; configured admissible range 8–192 kHz. Blocks 32–4096 frames; boundary tests include 32, 127, 512 and 4096. Configured bounds outside those executed points are not additional hardware qualification.
- Input <=600 seconds, <=64 physical channels, <=16 processing channels. Current plugin transport cannot negotiate untyped discrete multichannel buses, despite the general 16-channel host buffer bound.
- Tail is an explicit zero-extension duration, capped at 30 seconds regardless of unknown/infinite plugin reports. Fresh reset history at frame zero; reported latency <=10 seconds is discarded exactly. Creative delay remains content. Runtime bus/latency changes reject the result.
- One supervised child at a time per Process panel; one Process panel per workstation. Candidate scan 15 s, describe 20 s, processing 120 s. Cancellation kills/reaps the active child. Host buffers use <=4096 frames. States <=4 MiB, <=512 parameters, records <=32 MiB, <=2048 candidates, <=4096 members and <=1 GiB aggregate bundle fingerprint input. The supervisor checks child physical footprint every 10 ms and terminates above 1 GiB; this polling rule is not an instantaneous allocation sandbox.
- Float32 WAV is the authoritative completed representation. Optional normalization is derived from that pass before Ready/audition; toggling it after Ready derives from the verified raw PCM without a new plugin run. Create Copy and split export reuse the represented samples. Monitor gain is independent. Other delivery profiles can subsequently use the ordinary Designer renderer after Print; no arbitrary plugin is put in its two-pass normalization path.
- Processing records use a separate `soundplus-process` version 1 checksum envelope and version 1 job/result/receipt. Opening a job starts a new local edit history; parameter/reset/bypass Undo/Redo remain within that job. Stock document/take formats and canonical identities are untouched.

No claim is made for Windows, Rosetta/Intel bridges, real-time-only effects, interactive authorization/custom editors, AU, MIDI, chains, plugin automation, physical surround speakers, or real external-volume removal. Native receiver delivery is reported separately in the milestone report.

## Reproduce architecture rejection

Build only the owned fixture in a separate Intel QA tree; do not rebuild/replace the running app. With the normal native fixtures already built:

```sh
.tools/cmake/data/bin/cmake -S . -B build/plugin-intel-qa -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$PWD/.tools/bin/ninja" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=x86_64 -DBUILD_TESTING=ON
.tools/cmake/data/bin/cmake --build build/plugin-intel-qa \
  --target SoundPlusFixture_VST3 --parallel 6
python3 scripts/check_plugin_architecture.py \
  build/SoundPlus_artefacts/Release/SoundPlus.app/Contents/Helpers/SoundPlusPlugin \
  'build/SoundPlusFixture_artefacts/Release/VST3/SoundPlus QA Effect.vst3' \
  'build/plugin-intel-qa/SoundPlusFixture_artefacts/Release/VST3/SoundPlus QA Effect.vst3' \
  --output build/plugin-architecture-check.json
```

The script inspects actual slices with `lipo`, creates/signs a private universal copy, then calls the actual native scan worker for each. It preserves both input bundles and never installs them. Its three executed cases are additional checks, not extra CTest registrations.
