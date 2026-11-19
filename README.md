# SoundPlus

A standalone macOS sound library built with C++20, JUCE 8.0.6, CMake and SQLite FTS5. Add existing folders, search filenames or describe a sound, and audition original files or matching regions. No project, account or DAW is needed. Sound-description search runs locally and works offline after installing its optional model.

The product reference is [the architecture and requirements specification](docs/Sound_Design_Workstation_Architecture_and_Requirements.docx). The current milestone is **Performance Mixer v1.3.1 — Take Integrity and Output Correctness**: record the applied XY/four-macro performance, keep/name it, audition its frozen recipe, render a permanent WAV and print it back as an ordinary layer. See [v1.3.1 corrections and hardware evidence](docs/PERFORMANCE_MIXER_V1_3_1.md), [the recorded-take workflow](docs/PERFORMANCE_MIXER_V1_3.md) and [the take clock, persistence and effect-history contract](docs/PERFORMANCE_TAKES_CONTRACT.md).

This retains Designer v1.1.1 layered editing, stock processing, variations, referenced designs and durable WAV delivery. Library and Find Similar remain independently usable. The [Lucide icon/theme system](docs/ICON_SYSTEM.md), [button semantics and Resolve delivery status](docs/BUTTON_SEMANTICS_AND_RESOLVE_DELIVERY.md), [v1.2 reliability qualification](docs/PERFORMANCE_MIXER_V1_2.md), [v1.1 waveform report](docs/PERFORMANCE_MIXER_V1_1.md), [Designer corrections](docs/DESIGNER_V1_1_1.md), [direct-editing report](docs/DESIGNER_V1_1.md), [Find Similar](docs/FIND_SIMILAR.md), [local sound-description search](docs/LOCAL_AI_SEARCH.md) and [library performance](docs/LIBRARY_PERFORMANCE.md) remain applicable. Sampler pools, MIDI, hosted plugins, surround and Resolve marker placement remain future work.

## Build and launch

Run on an Apple Silicon Mac with Xcode Command Line Tools, Python 3.9+ and internet access for initial dependency downloads. Local AI requires macOS 13.3+. The Library/audition application retains its macOS 12.0 compilation minimum. Only macOS 26.3.2 was hardware tested. CMake, Ninja, JUCE and the native ONNX Runtime are installed inside this checkout. No Homebrew packages or administrator install are required.

```sh
cd /Users/harvest/Documents/Ysew_Project/GIT/Sound-Plus
./scripts/bootstrap.sh
.tools/cmake/data/bin/cmake -S . -B build -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$PWD/.tools/bin/ninja" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0
.tools/cmake/data/bin/cmake --build build --parallel 6
open "$PWD/build/SoundPlus_artefacts/Release/SoundPlus.app"
```

The app is already built at `build/SoundPlus_artefacts/Release/SoundPlus.app`. Its audio and AI helpers and native runtime library are bundled inside the application. Python is used for build/test tooling, never app inference. This is a local development build; distribution signing and notarization are deferred.

Click **Install AI Model → Download verified CLAP model** in the app (about 163 MB). Alternatively install the identical pinned pack from the checkout:

```sh
python3 scripts/install_model.py
```

The pack is verified by SHA-256 and reused at `~/Library/Application Support/SoundPlus/Models/clap-htsat-unfused-q8-c28f2883575e`. For an offline installation choose **Import offline model folder**, or run:

```sh
python3 scripts/install_model.py --from-pack /absolute/path/to/verified-model-pack
```

The offline folder must contain the eight files listed in `resources/clap-model.json`; importing a custom manifest cannot replace the app's pinned checksums. An optional `--model-dir /absolute/path/to/pack` selects a separate pack for isolated tests. `--disable-ai` runs just the Library and audition workflow.

The normal catalog is `~/Library/Application Support/SoundPlus/library.sqlite`. Keep it on local storage. For an isolated test catalog or scripted folder import:

```sh
soundplus_manual_catalog=$(mktemp -d "${TMPDIR%/}/soundplus-manual.XXXXXX")
open -n "$PWD/build/SoundPlus_artefacts/Release/SoundPlus.app" --args \
  --data-dir "$soundplus_manual_catalog" \
  --add-folder "/absolute/path/to/sound-effects"
```

Only one app window may write a given catalog. Different test catalogs can run independently. Prefer a temporary catalog for scripted checks: macOS privacy controls can block direct catalog access under Documents. Use **Add Folder** to grant access to your chosen sound folders.

## Five-minute test with your library

Start this timed test after installing the model. Use a small representative subfolder first; a whole library may need longer than five minutes to analyze.

1. **Minute 1:** Launch and use the **folder-plus icon** to add a representative subfolder. Search a known filename or describe a sound. Hover icons to see their names and shortcuts. Originals remain in place.
2. **Minute 2:** Select a promising recording. Audition with the centered transport. Press **I** to mark IN and **O** to mark OUT at the playhead (case-insensitive), click the waveform's **[ / ]** tools, or drag its bracket handles. The playhead stays visible after stopping. Exact numeric edits are available on right-click or ⌘E; press Return to apply. Click **Find Similar**. The reference stays above the alternatives with its exact range; its source recording is excluded by default.
3. **Minute 3:** Play the reference using its triangle icon, then select and audition candidates using the bracketed-play icon. Reference and candidate waveforms are independent. Edit the reference range and click its **Find Similar** again. Try **Include source recording** to search other moments in the same take.
4. **Minute 4:** Use the **audio-file icon** or drop one local audio file onto the app. It is a temporary reference; its separate folder-plus action explicitly indexes its **containing folder**, including other audio files. For multichannel audio, audition **Analysis Mix** or select a stereo pair. Analysis always uses the all-channel arithmetic average; opposing polarity can cancel. Long references show window progress.
5. **Minute 5:** Pause/resume indexing using the sidebar icon while searching and playing. Quit and reopen the same catalog: completed embeddings and a valid search index are reused. Choose the same reference/range again to reuse its vector cache. Disconnect networking and repeat. Space remains text input in editors and toggles audition with results focused.

Reference selections are session state; their exact-range embeddings persist. A long selection uses at most twelve model windows spread across it, averages normalized embeddings from nonsilent windows, then normalizes the result. Narrow the reference to one event when a long take contains unrelated sounds. Semantic similarity ranks neighbors; it does not prove a convincing match exists.

In both the full Library and Designer’s Library browser, selecting another result switches audition only while the selected recording is playing (or preparing to play). Selection while stopped remains silent; browsing does not take playback away from a Designer mix or a reference preview.

In Designer, click a header to select a track or a clip to highlight only that clip. Drag a track’s lower edge to resize it; double-click the edge to reset. Use the vertical-height icons or Command-Option-wheel for vertical zoom, Command-wheel for horizontal zoom, and each header’s colored circle for a saved track color. Header mute, solo and variation-lock controls mirror the inspector. Heights and zoom are workspace preferences. The check-in-circle transport action runs **Accurate audition** with full effect-history pre-roll; ordinary live seeking uses an approximate 250 ms history and audible grains use 50 ms. Use accurate audition after editing long delay or reverb tails.

Double-click a fade curve to add a small point, then drag it to reshape the fade. Option-click removes a point; Control-drag adjusts finely; Shift selects OUT when IN/OUT points coincide. Right-click a curve/handle, double-click its duration handle, or use its inspector curve button to choose Linear, Equal power, Slow, Fast, S curve or Custom. Switching presets retains the custom points. Both fades use the same theme accent and the same curve in playback and export. Undo/Redo and document recovery include the points. See [controls, screenshots and compatibility](docs/TRACK_CONTROLS_AND_FADES.md).

Click a stopped waveform to position its playhead silently; Play or Space starts from that parked position. Clicking the waveform that is currently playing seeks while keeping playback active. Clicking an inactive waveform leaves the other audition uninterrupted. Play Whole and Play Match explicitly start their full or selected range. Icon buttons use colour feedback without focus borders. Rescan, Retry Analysis and Reconnect Audio have distinct folder-scan, waveform-retry and plug icons. The transport shows actionable playback errors instead of a duplicate Playing/Stopped filename line.

**Rescan** reconciles all roots and retries failed headers. Output device and listening volume are in the bottom transport. There is no delete-original action.

## Repeatable tests

```sh
.tools/cmake/data/bin/ctest --test-dir build --output-on-failure
```

The twenty CTest suites generate small audio fixtures under isolated test-output directories, including fresh per-run Designer fixtures. The Library selection regression exercises both workspaces against the native audio helper, including pending playback, stopped/EOF/Stop races and playback ownership; see [selection-audition verification](docs/verification/selection-audition.json). It checks identity, interrupted indexing, 4,096-result paging, cancellation, pause/resume, reuse, source ranges at 44.1/48/96 kHz, waveform caching, revision invalidation and embedding-space separation. The new suite checks exact cached retrieval, live deltas, source exclusion, dense-window grouping, binary/metadata checksums, cache recovery, background-index pause and multichannel transport. It launches the real audio helper, requiring an available Core Audio output and a logged-in macOS session. The take-integrity and device-lifecycle suites check immutable native PCM during metadata edits, context-matched ready-file/Print-back binding, removed-original verified relocation, atomic source-change rejection, same-rate restart/error handling and interrupted Arming/finalization. The reliability suites additionally check request-aware loaded-row pinning, stable selection/ranges across background changes, cooperative waveform cancellation and fairness beyond48 simultaneous demands, and intentional helper crash/watchdog/reconnect diagnostics with real WAV preservation. The performance suite checks 0–4 assignments over the whole blend surface, stable target ownership, effective inspector edits, gesture undo/cancel, compatibility gates, primary-layer print weighting and exact native pre-monitor/offline PCM equality. The Designer suite checks mixed-rate sample timing, reverse/linked rate, source mapping, locked variations, exact document recall, cancellation and abrupt process interruption before publication, copy checksums and native eight-layer live/offline parity before monitor gain. See [Find Similar verification](docs/FIND_SIMILAR.md) for real-model workflows, 10k/100k/500k-region benchmarks and accuracy/recovery commands.

## 100,000-file native benchmark

```sh
python3 scripts/benchmark.py prepare --files 100000 --output build/benchmark
python3 scripts/benchmark.py run \
  --app "$PWD/build/SoundPlus_artefacts/Release/SoundPlus.app/Contents/MacOS/SoundPlus" \
  --fixture build/benchmark/fixture.json \
  --output build/benchmark/my-run --label current
```

Use a fresh `--output` directory for each run and run one benchmark at a time. Fixture construction is separate. The fixture lives under `/private/tmp/soundplus-benchmark-*`; each run uses dedicated catalogs and records JSON and logs in its output directory. Keep the app visible and avoid other disk-heavy work. The run measures native dispatch to first paint for 200 queries during indexing, then three runs of 200 warm queries and three while auditioning. It also checks empty states, source filters, paging, pause/resume, selection stability and a 35-second reopen. A separate six-file, 405-second corpus exercises full-file decoding and cached device-callback start. See [measurement definitions, results and limitations](docs/LIBRARY_PERFORMANCE.md).

Generate a separate neutral demo library, including a 30-second audition and duplicate filenames:

```sh
python3 scripts/make_fixture.py build/demo-library
```

The fixture generator refuses to overwrite a nonempty directory and records SHA-256 hashes in its manifest. Add the generated folder through the app.

The optional volume test creates and ejects only its own disposable HFS+ disk image. It checks offline visibility, volume identity, stable asset IDs, and remounting at a different path:

```sh
python3 scripts/test_volume.py \
  "$PWD/build/SoundPlus_artefacts/Release/SoundPlus.app/Contents/MacOS/SoundPlus" \
  build/volume-test
```

See [current performance and coverage](docs/LIBRARY_PERFORMANCE.md), [the historical first milestone](docs/MILESTONE_1.md) and [dependency versions and licensing](docs/DEPENDENCIES.md).


## Editable designs and delivered audio

Open Designer using the layers icon. Add a selected Library recording or its marked range with the layer-plus icon, drag a Library row onto the canvas, or use Designer's add-source icon for an external file. External sources do not index their containing folder. Library monitoring's Analysis Mix preference does not choose Designer channels: layers start with the original mono or first stereo pair.

The optional browser stays beside the canvas. Drag its divider to resize it; use the **Browser** text button and inspector icon to hide panels. The **Library** text button opens the full Library. Timing, fades, inserts and master settings are collapsible. Tooltips and accessible names identify actions. Space plays/stops; I/O mark source IN/OUT; Command-F searches; Command-Z/Shift-Command-Z undo/redo; Command-D duplicates; Command-O opens and Command-S saves. Panel choices, browser width, workspace, monitor volume and window bounds persist per catalog.

Designer’s **Edit / Perform** switch keeps the same design, transport and playhead. Perform offers an equal-power blend pad, four named stable-layer assignments and four named macros. Assign with the slot picker, drag the selected design-layer handle onto a slot, or drag a Library recording onto a slot to create an ordinary layer. Assignment/mapping starts bypassed: activate the blend or macro explicitly. Unassigned layers keep their levels; coherent layers may still clip. Pad arrows move by 0.025, Shift-arrows by 0.005, Home centres, and Escape cancels a drag. Numeric X/Y fields and macro values provide keyboard alternatives.

Perform source cards show one title, a choose/replace/clear arrow, a real 26-point source waveform and duration at the linked rate. Double-click the title to set an alias; replacing a source retains a custom alias. Drag an assigned card to move/swap corners in one Undo; slot pickers include all existing layers. A Library drop adds an ordinary layer. Return or double-click the card waveform opens its layer in Edit, without starting playback. The passive head follows Designer transport only within the source interval. Waveforms combine extrema across **all source channels**, before fades/gain/inserts/macros; trimmed edge bins are conservative at cache resolution. They are source overviews, not output meters. Loading, silence, missing layers, offline sources and decoder errors have distinct states. Visible full Library/Browser rows reuse these peaks and the renderer; the detailed audition panel remains available. One shared serial worker and bounded source/geometry caches keep thumbnail work off painting and the audio callback.

New sparse pads use a continuous geometric blend law: line coordinates for two occupied corners, triangle coordinates for three, after projecting onto their convex hull, then power normalization. Four corners keep the previous bilinear behavior. Saved v1 performance mixes retain their **legacy** law until the active mix’s explicit **Update law** action, which is undoable. Other saved snapshots keep their own law and delivered WAVs stay untouched. Escape during a pad/slider drag restores the original values and ignores the remainder of that held gesture until release.

Double-click assignment or macro names to rename. Each macro’s mapping button opens its target/range/response editor. Supported targets are layer gain/balance, deliberate master gain, stock filter cutoffs, and delay/reverb wet. One macro owns a target; blend-assigned layer gain cannot also be mapped. Missing targets remain visibly unresolved. The Edit inspector shows effective values; deliberately editing a mapped field unlinks only that target. Bypass restores base values, and reset returns a control to its neutral/reset position.

**Save Snapshot** creates a named kept, editable variation containing the base and its control settings. The selector recalls it; Compare switches to Original and back. Later edits while it is selected change that candidate. Save the document to retain snapshots and assignments. Full rendering and primary-selected-layer rendering use the same resolved state; print-back adds a normal layer. Selection rendering resolves weights against the complete design before isolating the primary layer, rather than renormalizing it to full level. Monitor gain remains excluded.

**Recorded Perform Takes:** choose **Live Design**, then **Record**. Preparation verifies source fingerprints and starts fresh DSP at frame zero; move XY and the four macros, then Stop. Choose the completed take, Audition, Keep/name, and **Render Take**. The selected context drives the shared transport and render; take replay always rebuilds history from frame zero. Takes are read-only performances; choose Live Design to edit the recipe again. Recording controls restore the untouched live values at Stop/Cancel. One Undo removes the finished take; keep/name/delete are undoable. Take options expose Cancel, Delete and verified identical-content relink. Incomplete recovered/faulted takes remain visible but cannot audition or render. Limits are 120 seconds, 8192 applied events and 16 takes per document. Saving takes requires manifest **4 / stock-takes-v1**; older readers reject it. Static documents keep their previous versions. Monitor volume stays excluded from WAVs. Print-back adds the finalized take WAV as an ordinary layer.

Generate a fresh four-source native recording fixture (impact, stepped texture, decaying tail and long ambience):

```sh
build/SoundPlusTests_artefacts/Release/SoundPlusTests --prepare-take-ui "$PWD/build/my-take-fixture"
```

Open its `workload.soundplus` through Designer. The generator refuses an existing destination. See [the five-minute take check and evidence](docs/PERFORMANCE_MIXER_V1_3.md).

A `.soundplus` file is a versioned, checksummed JSON **referenced design**. It stores source identities/revisions, original integer ranges, mappings, a fixed sample clock, ordered effects, variation candidates and base-plus-control snapshots, render settings and output receipts. Existing neutral designs keep manifest version 1. Legacy performance state keeps manifest 2 / `stock-perform-v1`. New-law state uses manifest **3 / `stock-perform-v2`**; older builds reject it rather than silently ignoring the new interpolation. Existing manifest-1 designs remain neutral. It does not contain source audio. Keep the original files and originating catalog, or explicitly relink sources on another machine. Collected media packages are not implemented. Saves use staging and a previous `.bak`; unsaved edits recover from the catalog's `designer-autosave` folder. `designer-preview` contains rebuildable playback snapshots and is separate from documents and exports.

The render icon writes a uniquely named verified WAV to `~/Music/SoundPlus/Renders` by default. The folder icon chooses another permanent folder, saved in the design. Default output is 48 kHz, 24-bit stereo; render settings expose mono/stereo, 44.1/48/96 kHz, 16/24-bit PCM or 32-bit float WAV, a range, a fixed tail (default one second, maximum 30), explicit normalization and optional TPDF dither. Mono output averages the stereo mix `(L+R)/2`; mono layer pan uses a constant-power law, while a centered stereo layer preserves its channel levels. Monitor volume is excluded from renders. Cutoffs are limited to the saved design rate's usable band.

After finalization, drag the ready output icon into Finder or another audio application. Outputs remain in the chosen folder after restart or preview-cache cleanup. The print-back icon adds that file as a new external layer. Drag Original transfers an applicable complete source URL; a destination may reference that original path. Copy Original to Project makes a byte-identical verified copy in a folder you choose. Rendering always uses an immutable revision with independent DSP state, so editing during a render affects subsequent results.
