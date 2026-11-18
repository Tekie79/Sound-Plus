# SoundPlus

A standalone macOS sound library built with C++20, JUCE 8.0.6, CMake and SQLite FTS5. Add existing folders, search filenames or describe a sound, and audition original files or matching regions. No project, account or DAW is needed. Sound-description search runs locally and works offline after installing its optional model.

The product reference is [the architecture and requirements specification](docs/Sound_Design_Workstation_Architecture_and_Requirements.docx). The current milestone adds real paired CLAP embeddings, progressive sound analysis, fused search, cached waveforms and editable region audition. See [local AI search, measurements and limits](docs/LOCAL_AI_SEARCH.md) and the preceding [library performance report](docs/LIBRARY_PERFORMANCE.md). Designer, plugin hosting, search by example and Resolve placement remain future work.

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

1. **Minute 1:** Launch, click **Add Folder**, and select your sound-effects folder. Search a known filename while names and headers appear. Confirm semantic coverage begins increasing. Originals remain in place.
2. **Minute 2:** Enter an everyday description of a sound you know is present, then press Return. For example, describe a door creaking or water pouring. Look for **Sound similarity** and a time range. Coverage is partial until analysis finishes; similarity is a ranking signal, not a confidence percentage. Select a result and press **Play Match**.
3. **Minute 3:** Choose a longer recording with an event away from its start. Inspect **Matching moments**, click the waveform to seek, drag to adjust the range or edit **IN/OUT (seconds)**. **+/-0.25 s context** adds 250 ms on both sides; **Play Whole** starts the complete recording. While playing, use Command-F to change the query; the pinned audition must continue.
4. **Minute 4:** Click **Pause Indexing**, wait for **Paused**, and search/play again. Resume, then quit with Command-Q. Reopen the same catalog: filename search is immediate; sound search becomes ready after local model loading and reuses completed embeddings. Unfinished work resumes. Use **Refresh Results** when a selected/paged result order is held.
5. **Minute 5:** Disconnect networking and repeat a sound-description query. If using an external drive, stop playback, eject normally, and confirm its results stay visible as **Offline**; reconnect and audition again. Space types spaces in text fields; with results focused it plays/stops. Down leaves the search field for results.

**Rescan** reconciles all roots and retries failed headers. Output device and listening volume are in the bottom transport. There is no delete-original action.

## Repeatable tests

```sh
.tools/cmake/data/bin/ctest --test-dir build --output-on-failure
```

CTest generates small audio fixtures under its three test-output directories, checks identity, interrupted indexing, 4,096-result paging, cancellation, pause/resume, reuse, source ranges at 44.1/48/96 kHz, waveform caching, revision invalidation and embedding-space separation. It launches the real audio helper, requiring an available Core Audio output and a logged-in macOS session. It recreates only its own output directories. See the AI report for the additional real-model workflow and native benchmark commands.

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
