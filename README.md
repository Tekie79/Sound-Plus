# SoundPlus

A standalone macOS sound library built with C++20, JUCE 8.0.6, CMake and SQLite FTS5. Add existing folders, search their filenames while indexing continues, and audition original files with Space. No project, account, DAW or network connection is needed at runtime.

The product reference is [the architecture and requirements specification](docs/Sound_Design_Workstation_Architecture_and_Requirements.docx). This is the Library and audition slice requested for the first working milestone, rather than the specification's complete M1 (which also includes semantic search and region audition).

## Build and launch

Run on an Apple Silicon Mac with Xcode Command Line Tools, Python 3 and internet access for the initial dependency download. CMake 3.31.6, Ninja 1.11.1.3 and the checksum-verified JUCE source are installed inside this checkout. No Homebrew packages or administrator install are required.

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

The app is already built at `build/SoundPlus_artefacts/Release/SoundPlus.app`. macOS 12.0 is the compilation minimum; the hardware/OS actually tested is recorded in [milestone verification](docs/MILESTONE_1.md). This is a local development build; distribution signing and notarization are deferred.

The normal catalog is `~/Library/Application Support/SoundPlus/library.sqlite`. Keep it on local storage. For an isolated test catalog or scripted folder import:

```sh
open -n "$PWD/build/SoundPlus_artefacts/Release/SoundPlus.app" --args \
  --data-dir "$PWD/build/manual-catalog" \
  --add-folder "/absolute/path/to/sound-effects"
```

Only one app window may write a given catalog. Different test catalogs can run independently.

## Five-minute test with your library

1. **Minute 1:** Launch, click **Add Folder**, and select a sound-effects folder with nested folders. Watch the source's count increase. Search for a known filename prefix before scanning finishes. Names appear before duration/channels/rate finish loading.
2. **Minute 2:** Select a result and press **Space**. Check filename, path, duration, channels and sample rate. Press Space again to stop. Try two files with the same name in different folders: their paths and displayed asset IDs must differ.
3. **Minute 3:** Play a longer recording. Press **Command-F** and search for another filename. The bottom transport must continue naming and playing the original recording even if it leaves the result list. Use Down to focus results. Space stops the pinned audition when no result is selected, or starts the newly selected result. Spaces typed in the search field remain search text.
4. **Minute 4:** Quit with Command-Q and reopen. Search immediately. If the initial header pass finished, unchanged sources should show **0 headers read this session** after reconciliation. An interrupted pass resumes its unfinished jobs. File enumeration and stat checks still run; unchanged completed recordings are not decoded or reindexed in FTS.
5. **Minute 5:** If you indexed a removable drive, stop playback and eject it normally in Finder. Within a few seconds its source and results should say **Offline**, while searches still find them. Reconnect it: the source should become Online and the same asset IDs should play again. With only local storage, temporarily rename a disposable fixture folder and restore its name to test availability handling.

**Rescan** reconciles all roots and retries failed headers. Output device and listening volume are in the bottom transport. There is no delete-original action.

## Repeatable tests

```sh
.tools/cmake/data/bin/ctest --test-dir build --output-on-failure
```

CTest generates WAV, AIFF and FLAC recordings under `build/test-output`, exercises the real catalog services, interrupts a 2,400-file import, and launches the app's audio helper against the current Core Audio output. It requires an available output device and a normal logged-in macOS session. It recreates its own test-output directory on each run.

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

See [architecture, coverage and verification](docs/MILESTONE_1.md) and [dependency versions and licensing](docs/DEPENDENCIES.md).
