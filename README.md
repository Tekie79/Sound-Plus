# SoundPlus

A standalone macOS sound library built with C++20, JUCE 8.0.6, CMake and SQLite FTS5. Add existing folders, search their filenames while indexing continues, and audition original files with Space. No project, account, DAW or network connection is needed at runtime.

The product reference is [the architecture and requirements specification](docs/Sound_Design_Workstation_Architecture_and_Requirements.docx). The current milestone adds large-library paging, cancellable search, incremental indexing and Pause/Resume to the standalone Library and audition workflow. See [performance measurements and verification](docs/LIBRARY_PERFORMANCE.md). The specification's complete M1 also requires semantic search and region audition, which remain future work.

## Build and launch

Run on an Apple Silicon Mac with Xcode Command Line Tools, Python 3.9+ and internet access for the initial dependency download. CMake 3.31.6, Ninja 1.11.1.3 and the checksum-verified JUCE source are installed inside this checkout. No Homebrew packages or administrator install are required.

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

The app is already built at `build/SoundPlus_artefacts/Release/SoundPlus.app`. macOS 12.0 is the compilation minimum; the hardware/OS actually tested is recorded in [current verification](docs/LIBRARY_PERFORMANCE.md). This is a local development build; distribution signing and notarization are deferred.

The normal catalog is `~/Library/Application Support/SoundPlus/library.sqlite`. Keep it on local storage. For an isolated test catalog or scripted folder import:

```sh
soundplus_manual_catalog=$(mktemp -d "${TMPDIR%/}/soundplus-manual.XXXXXX")
open -n "$PWD/build/SoundPlus_artefacts/Release/SoundPlus.app" --args \
  --data-dir "$soundplus_manual_catalog" \
  --add-folder "/absolute/path/to/sound-effects"
```

Only one app window may write a given catalog. Different test catalogs can run independently. Prefer a temporary catalog for scripted checks: macOS privacy controls can block direct catalog access under Documents. Use **Add Folder** to grant access to your chosen sound folders.

## Five-minute test with your library

1. **Minute 1:** Launch, click **Add Folder**, and select a sound-effects folder with nested folders. Watch the source's count increase. Search for a known filename prefix before scanning finishes. Names appear before duration/channels/rate finish loading.
2. **Minute 2:** Click **Pause Indexing**. Wait for **Paused**, then search and audition with **Down**, **Space** and **Command-F**. Resume and confirm counts continue. With more than 1,000 matches, scroll beyond them. Selecting or paging holds the result order; **Refresh Results** includes newly discovered files while retaining an eligible selected asset ID.
3. **Minute 3:** Play a longer recording. Press **Command-F** and search for another filename. The bottom transport must continue naming and playing the original recording even if it leaves the result list. Use Down to focus results. Space stops the pinned audition when no result is selected, or starts the newly selected result. Spaces typed in the search field remain search text.
4. **Minute 4:** Copy one spare audio file into an indexed subfolder, give it a distinctive name, then search for it. Use **Refresh Results** if the current result order is held. Quit with Command-Q and reopen the same catalog. Search immediately. Completed unchanged sources use directory fingerprints without enumerating every recording; unfinished header work resumes.
5. **Minute 5:** Stop playback. If using a removable drive, eject it normally in Finder and confirm its results remain searchable as **Offline**; reconnect and audition the same asset again. Otherwise, rename the spare file from minute 4 and search its new name. **Rescan** performs explicit recovery and retries failed headers.

**Rescan** reconciles all roots and retries failed headers. Output device and listening volume are in the bottom transport. There is no delete-original action.

## Repeatable tests

```sh
.tools/cmake/data/bin/ctest --test-dir build --output-on-failure
```

CTest generates audio under `build/test-output` and `build/performance-test-output`, exercises the real catalog services, interrupts a 2,400-file import, checks paging across 4,096 matches during concurrent import, rapid query cancellation, pause/resume, changed-directory work and unchanged reopen. It launches the audio helper against the current Core Audio output, so it requires an available output device and a logged-in macOS session. It recreates only its own test-output directories on each run.

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
