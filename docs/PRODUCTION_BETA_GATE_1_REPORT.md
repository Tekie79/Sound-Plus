# Production Beta Gate 1

Candidate work began from reviewed HEAD `2de9e19bd6b440b3d229ac1e879e962e2ff72292`. The release scope is an invitation-only Apple Silicon macOS beta. Larger feature work stayed frozen. Checks use generated audio and isolated catalogs. Verified stages were pushed to `origin/main` as `fabd6b3` for motion hardening, `521ba05` for workflow and Resolve, and `292f094` for stress and take reliability.

## Reliability findings

| Finding | Result |
|---|---|
| P2 · Clipboard schema and point budgets | Validates the row before accessing fields, rejects nonfinite, coerced or out-of-range values, detects collisions after sample-rate conversion and checks both axis budgets. Invalid paste leaves the document intact. |
| P3 · Numeric motion range | Exact time resolves against the current design's playable body frames. Invalid time leaves the point unchanged. A valid move is one Undo. |
| P2 · Touch interruptions | Clears held local gestures on focus or pointer loss. Stop, seek and graph changes stop accepting motion. Only matched receipts can change saved axes. Forced helper loss during active Touch retained the previous curves and restarted the helper. The unfinished pass was reported as interrupted. |
| P1 · Take-limit finalization | The first 120-second 5.1 soak finished playback with zero underruns and marked the take Incomplete. The deterministic regression reproduced Stop overwriting a naturally finalizing take. The fix preserves consumed frames and the durable checkpoint on late or repeated Stop for that generation. |

Native validation exercised malformed and valid paste, out-of-range numeric time, one Undo after a valid move, and an applied Touch pass with matching X/Y frames and a 100 ms return. Shift selection, rectangle cancellation, focus loss and close passed through the panner's component event handlers. A real modifier-held rectangle and focus switch during a still-held Touch gesture remain open. [Stage A evidence](verification/beta-gate-1/stage-a/README.md)

## Workflow evidence

- **Library and native Designer.** Marked 2.000–6.000 s in the 48 kHz Library recording and inserted source frames 96,000–288,000 in the existing Audio Track. The same motion path survived restart. Native render created both 5.1 layouts, each with 912,000 frames at 48 kHz and 24-bit PCM. WAV masks were `0x60f` for Side and `0x3f` for Rear. Twelve mono splits matched their master channels byte-for-byte and both positional LFE tracks were silent. [Marked range](verification/beta-gate-1/native/marked-library-range.jpg), [WAV audit](verification/beta-gate-1/native-render-pcm.json)
- **Routed processing.** Indexed source → marked range → Audio Track with owned VST3 gain → Square Read → named 5.1 Group and Master native gain. Independent PCM checked the front-center and rear roles, with both layouts saved and read back. Missing source and IR were reported with their identities retained. Explicit source and identical IR relink preserved the original finalized render. [Workflow CTest](verification/beta-gate-1/workflow-ctest.log)
- **Resolve Studio 21.1.2.10.** The receiving Media Pool imported the two independent masters and placed them on tracks 2 and 3. Each is six-channel, 48 kHz and 456 timeline frames at 24 fps. Saved the QA project, quit SoundPlus, quit Resolve, relaunched Resolve and reopened the project. Resolve returned both items Online with the same IDs, track placements, duration and six-channel mapping. Each item references the original finalized test WAV path. The previous Resolve project was reopened. Resolve Media Pool import and native drag remain distinct delivery paths. [Receiver evidence](verification/beta-gate-1/resolve-receiver.json)
- **MIDI and Perform.** The software sampler test schedules a MIDI note at frame 275 and verifies the same compensated 5.1 PCM through a lookahead Master insert. Native Pad A–D controls appeared in Perform on the isolated project. [Perform view](verification/beta-gate-1/native/perform-four-pads.jpg)

## Failure and distribution evidence

The focused recovery gate passed 8/8 after correcting a test fixture's MIDI microtick count. The permission test makes a generated project directory read-only. The save cancellation test stops after staging and sync. The disk test fills a dedicated HFS image until the operating system returns ENOSPC. All three retain the prior save. Actual helper crash and watchdog checks, controlled device stop/reconnect and sample-rate changes, and VST3 availability errors run through their owned harnesses. Physical interface disconnect remains open.

The rerun used a 40-minute project source, eight layers, 1,024 generated indexed recordings, a 5.1 Side design bus, and stereo CoreAudio at 48 kHz and 512 frames. It retained 5,760,000 consumed frames and 4,652 events and produced a finalized WAV. During capture there were zero underruns, 80 whole waveform viewports and 4,493 control updates overlapping indexing. Playback generation, graph epoch and helper process stayed fixed. At 110 s the same graph had 10,320 producer fill samples, a 1.00 ms p95 upper bound and a 2.092 ms maximum. Helper peak RSS at completion was 248 MiB, and the helper accumulated 85.86 CPU seconds. App peak RSS was 269 MiB. The metric histogram runs on the producer and uses 0.05 ms buckets. This sustained case exercised Perform Blend, macro and angular spatial controls with concurrent Library work, followed by take rendering. Square Touch, hosted editor Touch, MIDI and input recording used dedicated tests. A single all-mode endurance run remains open. [Before/after soak](verification/beta-gate-1/stress/soak.json), [late-Stop reproduction](verification/beta-gate-1/stress/late-stop-repro.log), [focused retest](verification/beta-gate-1/stress/ctest.log), [ENOSPC](verification/beta-gate-1/stress/enospc.log)

The local Apple Silicon bundle contains the application, audio, AI, and plugin helpers, the nested editor app and ONNX Runtime. The AI helper resolves its dylib relative to the bundle. Bundled notices cover the inspected runtime, models and icon set. The current checkout records JUCE 8.0.6's AGPLv3/commercial license options and leaves the distribution license choice open. The host has no valid signing identity. Gatekeeper rejected the development bundle. A signed installer, real notarization response and clean-account install/upgrade/uninstall/rollback remain unqualified. [Dependency inventory](DEPENDENCIES.md), [Apple notarization requirements](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution/)

The copied bundle had the same 18 file hashes, indexed a generated recording in a new catalog, and reopened an isolated catalog from the earlier beta stage. Removing and reinstalling the local copy preserved the catalog bytes, and the app reopened with the recording indexed. This exercise used the existing macOS account. [Bundle audit](verification/beta-gate-1/stress/package-candidate.json)

The candidate opened Edit at 1040×680 from its isolated window preferences with Browser and Inspector visible. The Audio and Master lanes opened collapsed, and expanding the Audio lane showed Volume. Resizing the floating panner to 350×400 kept the field square. Perform showed A–D at the minimum size. [Edit](verification/beta-gate-1/native/candidate-edit-minimum.jpg), [Volume lane](verification/beta-gate-1/native/candidate-edit-volume.jpg), [panner](verification/beta-gate-1/native/candidate-panner-minimum.jpg), [Perform](verification/beta-gate-1/native/candidate-perform-four.jpg)

## Release decision

**No-Go for private beta distribution.** The software, native layout and Resolve receiving checks passed in their measured scope. Gatekeeper rejected the development bundle. Developer ID signing, notarization, clean-account upgrade and rollback, physical six-speaker mapping, and human listening remain open. No release waiver or human acceptance is on record.

| Gate | Status and evidence |
|---|---|
| Candidate and Release tests | **Passed 54/54 in 545.43 seconds** on Apple M5 Pro arm64, macOS 26.3.2 (25D2150), SDK 26.5, Apple Clang 21 and JUCE 8.0.6. Source `292f094f64bdf7a5165dbc5214c2baa641541c86`. Main executable SHA-256 `ed6a1b473718312ef320606a11974d075809655c4d36b1365790872f5288e43d`. The complete Release gate matched 383 source/resource/script hashes, 17 executable hashes and 18 bundled files before and after. [CTest](verification/beta-gate-1/final/release/ctest.log), [before](verification/beta-gate-1/final/release/identity-before.json), [after](verification/beta-gate-1/final/release/identity-after.json) |
| Numerical PCM and native UI | Passed named-role, gain, mask, mono split, PDC, timing, saved state and minimum window checks. Native held-modifier and focus-interruption acceptance has controlled component coverage. A separate physical pointer repeat remains open. [Native candidate evidence](verification/beta-gate-1/native) |
| CoreAudio and human review | Stereo device passed at 48 kHz and 512 frames. Square Touch applied 87 of 89 submitted positions with zero underruns. Forced helper loss preserved previously acknowledged curves. Six-output playback and human listening are blocked by the available two-channel interface and the lack of a human session. Physical MIDI and microphone loopback remain separate. [Native device receipt](verification/beta-gate-1/final/release/surround-device.json) |
| Installed plugins | Passed the enabled VST3 and AU branches, including owned fixtures, OldSkoolVerb, LV50A, and AUDelay on this host. Hardware and plugin breadth remains limited to the recorded matrix. [Hosted vendor results](verification/beta-gate-1/final/release/hosted-vendors.json) |
| Resolve | Passed real Media Pool import, placement, save, quit, reopen and online persistence for both 5.1 masters. Both remain source-path references. Native cross-app drag is separately unverified. [Resolve evidence](verification/beta-gate-1/resolve-receiver.json) |
| Distribution and licensing | Blocked. The development bundle passed local copy and isolated catalog launch. Strict signature and Gatekeeper checks returned exit 1. A distribution license choice and authorized signing identity are pending. [Local bundle audit](verification/beta-gate-1/stress/package-candidate.json) |
| Original workspace | **Workflow demonstration r4499** reopened in Edit with unsaved changes recovered. The payload, 392 protected files, 197 retained state files, and all 18 catalog tables matched the baseline after reopen. The original session remains open. [Preservation audit](verification/beta-gate-1/final/release/workspace-preservation.json) |

## Reproduce the checks

Use the Release configuration with `-DBUILD_TESTING=ON -DSOUNDPLUS_BUILD_PLUGIN_ASAN_QA=ON` on Apple Silicon. The repository README gives the bootstrap and configure commands. Close any process from the candidate bundle before rebuilding.

```sh
python3 scripts/guard_bundle_closed.py
.tools/cmake/data/bin/cmake --build build --config Release --parallel 8
export SOUNDPLUS_VENDOR_QUALIFICATION="$PWD/build/beta-gate-1/final-vendors"
export SOUNDPLUS_LIVE_DEVICE_QUALIFICATION=1 SOUNDPLUS_QUALIFY_AU=1
export SOUNDPLUS_HOSTED_VENDOR_QUALIFICATION=1
unset SOUNDPLUS_QA_OVERLAY_ONLY SOUNDPLUS_QA_TOUCH_ONLY SOUNDPLUS_QA_LOCAL_AU SOUNDPLUS_BETA_FULL_VOLUME
python3 scripts/qualify_hosted_release.py before docs/verification/beta-gate-1/final/release
.tools/cmake/data/bin/ctest --test-dir build -C Release --output-on-failure
python3 scripts/qualify_hosted_release.py after docs/verification/beta-gate-1/final/release
```

The 120-second workload uses the existing reliability tool, with both destinations new and isolated:

```sh
python3 scripts/benchmark_reliability.py prepare --files 1024 --output build/beta-gate-1/fresh-soak-fixture
python3 scripts/benchmark_reliability.py run --fixture build/beta-gate-1/fresh-soak-fixture \
  --output build/beta-gate-1/fresh-soak --seconds 120 --cycle-ms 1500 --buffer-frames 512 \
  --design-layout 5.1-side-v1 --record-take
```

To reproduce the ENOSPC check, run the workflow test with `SOUNDPLUS_BETA_FULL_VOLUME` pointing to a new mounted disk image no larger than 64 MiB. It fills the image and checks that the authoritative saved design remains byte-identical. The concurrent full gate leaves that optional branch unset because the independently qualified image is detached.

For a native QA session on this Mac, start the built executable with a separate catalog:

```sh
build/SoundPlus_artefacts/Release/SoundPlus.app/Contents/MacOS/SoundPlus \
  --data-dir "$PWD/build/beta-gate-1/manual-catalog" --disable-ai
```

Use **Add Folder** with a copy of test media, mark a range and choose **Add marked region to Designer**. Set the Design bus to **5.1 Side** or **5.1 Rear** and open **Track options → Square Surround** on the Mono or Stereo Audio Track. Enable XY, add two points at different playhead times, then audition Read and Touch. Choose an isolated **Permanent render folder** and turn on **Split mono set + multichannel Print master**. Keep the finalized folder at its original path when testing Resolve reopen. A receiver may reference that path.
