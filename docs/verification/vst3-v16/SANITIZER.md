# Plugin component/worker sanitizer check

The pre-fix AddressSanitizer log identifies an out-of-bounds impulse write in the owned `wave()` test generator when `frames` was 513/1027. The original fixed-position second pulse at 2048 exceeded those buffers. It corrupted the test process and later surfaced in publication cloning. Both impulse writes now check the generated buffer length; the release test fixture and independent short-boundary assertions use the corrected bytes.

The first diagnostic driver linked the legacy uninstrumented core and produced a libc++ deque container-annotation mismatch. It was not counted as a pass. The retained successful driver links a consistently instrumented dedicated core, JUCE and production UI/session components, with ordinary release fixture/worker processes across the real process boundary. No container-overflow suppression is used. `ASAN_OPTIONS=detect_leaks=0` excludes framework/global leak reporting; this is memory-access qualification, not a leak result.

The optional `SOUNDPLUS_BUILD_PLUGIN_ASAN_QA` CMake switch builds `SoundPlusPublicationDebug` and its dedicated core without changing the release application's flags or bundling that diagnostic executable. The pass log identifies the generated per-run root. It is not used for release performance numbers.

[Final sanitizer executable identity](sanitizer-artifact.json) and [passing log](asan-component-after.log) accompany the [original fixture failure](asan-fixture-overflow-before.log). Close executing app/helper bundles before rebuilding. Reproduce from the repository root:

```sh
.tools/cmake/data/bin/cmake -S . -B build -DSOUNDPLUS_BUILD_PLUGIN_ASAN_QA=ON
.tools/cmake/data/bin/cmake --build build --target SoundPlusPublicationDebug --parallel 6
ASAN_OPTIONS=detect_leaks=0 \
  build/SoundPlusPublicationDebug_artefacts/Release/SoundPlusPublicationDebug \
  "$PWD/build/plugin-asan-qa" \
  "$PWD/build/SoundPlus_artefacts/Release/SoundPlus.app/Contents/MacOS/SoundPlus" \
  "$PWD/build/SoundPlusFixture_artefacts/Release/VST3/SoundPlus QA Effect.vst3" \
  "$PWD/build/SoundPlusStereoFixture_artefacts/Release/VST3/SoundPlus QA Stereo.vst3"
.tools/cmake/data/bin/cmake -S . -B build -DSOUNDPLUS_BUILD_PLUGIN_ASAN_QA=OFF
```

Retained text logs normalize line endings/trailing whitespace only; original tool logs remain under the local `build/vst3-v16` QA directory.
