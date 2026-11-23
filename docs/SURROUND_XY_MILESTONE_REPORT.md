# Square Surround and Spatial Motion v1

Square Surround adds an opt-in visual panner to the Edit workspace for mono and stereo Audio Tracks and MIDI Instrument audio routed to a named 5.1 Side or Rear bus. The panner previews the speaker shares before activation. It follows the shared playhead, edits project-time paths, and writes paired Touch through the existing single-lane automation model.

Reviewed baseline `98268c50b967d013f80a4f323bb9f9eef998da48`. Stage A was pushed as `aac4b4c` and the panner and motion implementation as `952273da742e82a6d131c3ea1a2bd7ce63e006db`. Both are on `origin/main`. The local AGENTS.md changes were followed and kept outside the milestone commits.

| Scope | Implementation and evidence |
|---|---|
| A · Square 5.1 policy | [Role geometry and normalization](../src/SurroundXY.cpp), [Track binding and one-lane registry](../src/TrackModel.cpp) and [producer matrix](../src/DesignAudio.cpp). Stage A passed 6/6 in 21.89 s. Independent numeric anchors and PCM for C, L/R, both surround roles, back center and square center. Linked and separated Stereo legs, continuity, exact zero LFE, 44.1/48/96 kHz and 1/32/61/257/512-frame blocks. [Stage A log](verification/surround-xy/stage-a/ctest.log) |
| B · Edit field | [Native view](../src/SurroundPanner.cpp) and [Track Options launcher](../src/MultitrackEditor.cpp). Resizable floating window with legible role labels, target contribution bars, object/readout and editable path. Native QA exercised enable/preview, drag, Shift-fine arrows, double-click to add, absolute time entry, point dragging and Undo/Redo. A focus defect in the initial floating window and overlap at its minimum size were corrected. |
| C · Motion | [Paired producer capture](../src/SurroundTouch.h), [acknowledged session commit](../src/DesignSession.cpp), and [tests](../tests/SurroundMotionTests.inc). Independent Read curves and one Undo per Touch pass. One native pointer pass retained five points per axis, including existing keys, at matching frames. CoreAudio sustained gesture and Stop paths verified separately. Linear, Hold, and smoothstep with preserved phase at a crop/splice. Manifest 19 and recipe 10 are conditional, with retained PCM and project/take version checks. |
| D · Output | Independent 5.1 Side mask `0x60f` and Rear mask `0x3f`. All six labeled mono files match the corresponding pre-monitor bus within 2×10⁻⁶. Normalized master uses one gain across channels. Additional checks cover routing via a 5.1 Group, MIDI sampler output and frozen serialization, Master lookahead PDC and repeated accurate starts. Full Release gate passed 53/53. |

The square center distributes equal power to five speakers. Front-center routes to C, and the selected 5.1 layout determines the surround pair. Per-input coefficients transition across 10 ms and retain normalized power. Stereo preserves the two input legs with one-half power per leg. Coherent stereo can sum. [Detailed equations and limits](SURROUND_XY_CONTRACT.md)

## Native verification

The isolated generated fixture used MacBook Pro Speakers via CoreAudio. The stereo device monitored a downmix. The PCM and WAV checks exercised the six-channel master. Native viewport checks used the Mac's Retina display. The Edit window showed Browser and Inspector at 1040×680, the track options remained usable, and opening the lane showed Volume first. The floating window retained square dimensions when resized to 350×400. The motion path and speaker bars use the shared playhead and display target power shares. The document and screenshots use generated audio.

- [Desktop panner and path](verification/surround-xy/native/panner-path.jpg)
- [Compact Edit with Browser and Inspector](verification/surround-xy/native/edit-minimum.jpg)
- [Compact square panner](verification/surround-xy/native/panner-minimum.jpg)

Installed hardware reports two output channels. Physical 5.1 speaker placement, physical MIDI/controller latency, human listening and Resolve receiving-project persistence remain open. Square object output supports 5.1 Side and Rear. Other buses retain their direct, matrix or historical angular policies. Track LFE send/cutoff are deferred, with LFE excluded from the square. The panner is floating and resizable. Docking and a native timeline loop mode remain future work. Accurate restart was checked against uninterrupted PCM. Native rectangle selection and pointer focus-loss during Touch have implementation coverage and remain open as separate interactive checks. This release includes Read and Touch. A pass requires an applied producer receipt. Helper loss before that receipt can discard in-flight motion.

## Release qualification

The Release candidate was built on Apple M5 Pro arm64, macOS 26.3.2 (25D2150), SDK 26.5, Apple Clang 21.0.0, JUCE 8.0.6. Main executable SHA-256 is `78996a2438ebfa4368458e5d75aad6af4ca9ddf0818f8fd21a21c62410e0250e`. The final focused gate passed 6/6 in 25.63 s. [Focused log](verification/surround-xy/stage-bc/ctest.log)

The full gate passed **53/53 in 541.60 s** with installed vendor, hosted vendor, AU and live device branches enabled. It includes the unchanged Audio, MIDI, Perform, Library, native effects, hosted effects and instrument regressions and the advanced-effects sanitizer suite. All 383 source, resource and script hashes and all 17 executable hashes matched before and after. [Release log](verification/surround-xy/final/release/ctest.log), [before fingerprints](verification/surround-xy/final/release/identity-before.json), [after fingerprints](verification/surround-xy/final/release/identity-after.json)

The device test ran at 48 kHz with 512-frame buffers. The producer applied 87 of 89 submitted positions over roughly 2 seconds, coalescing updates that arrived within the same production interval. The helper reported a maximum producer fill time of 0.265 ms, peak resident memory of 51.4 MiB, 0.094 s of CPU and zero underruns. The 1,668-byte receipt was acknowledged, and the Save/Reopen, Undo/Redo and Stop checks passed. A separate maximum-size test verified the receipt envelope stays under 34 KB. [CoreAudio receipt](verification/surround-xy/final/release/surround-device.json)

```sh
python3 scripts/guard_bundle_closed.py
export SOUNDPLUS_VENDOR_QUALIFICATION="$PWD/build/surround-xy/final-vendors"
export SOUNDPLUS_LIVE_DEVICE_QUALIFICATION=1 SOUNDPLUS_QUALIFY_AU=1
export SOUNDPLUS_HOSTED_VENDOR_QUALIFICATION=1
unset SOUNDPLUS_QA_OVERLAY_ONLY SOUNDPLUS_QA_TOUCH_ONLY SOUNDPLUS_QA_LOCAL_AU
python3 scripts/qualify_hosted_release.py before docs/verification/surround-xy/final/release
.tools/cmake/data/bin/ctest --test-dir build -C Release --output-on-failure
python3 scripts/qualify_hosted_release.py after docs/verification/surround-xy/final/release
```

## Working-session preservation

**Workflow demonstration r4499** reopened in Edit with unsaved changes recovered. The first Launch Services start stalled in the preferences-file open. It was stopped, the workspace was checked again, and the Release executable launched directly. The native revision readout matched the saved payload. Audits before qualification, after qualification and after reopen matched all 392 protected original and finalized files, all 197 retained state files and the rows in all 18 catalog tables. The working session remains open. [Workspace audit](verification/surround-xy/final/release/workspace-preservation.json)

## Try it

1. In an isolated Design, choose a **5.1 Side** or **5.1 Rear** Master. Add a mono or stereo Audio Track, set its Track bus to match the source, and add a clip. For MIDI, use an Instrument that accepts the chosen mono/stereo Track bus.
2. Open **Track options → Square Surround** and select **Enable XY**. Drag around the field and watch the role shares. Using the built-in speakers auditions the stereo monitor mix.
3. Double-click the field to add a point at the playhead. Select it and press Enter to enter an exact time. Add another at a different time and choose a shape from the motion menu. Use Shift-drag to select a range of nodes.
4. Switch the existing lane from Volume to **Position X** or **Position Y**, or collapse it. Start playback with Space, arm the panner's Touch, drag, release and stop. The committed pass is one Undo. Save and reopen to see the path.
5. Render with the same bus layout and Mono splits to inspect the six labeled outputs.

On the qualified checkout, run:

```sh
cd /Users/harvest/Documents/Ysew_Project/GIT/Sound-Plus
python3 scripts/guard_bundle_closed.py
.tools/cmake/data/bin/cmake --build build --config Release -j8
build/SoundPlus_artefacts/Release/SoundPlus.app/Contents/MacOS/SoundPlus \
  --data-dir "$PWD/build/my-square-surround-trial" --disable-ai
```

For a fresh checkout, use the repository README's bootstrap and configure commands with `-DBUILD_TESTING=ON -DSOUNDPLUS_BUILD_PLUGIN_ASAN_QA=ON`. Keep the working app closed during rebuild.
