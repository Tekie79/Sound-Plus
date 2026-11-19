# Source Pools v1.5

A Designer layer can keep up to **64 alternatives**, including its initial source. A normal single-source layer needs no pool. The Library remains the full searchable catalog; alternatives reference original files in place.

## Use alternatives

1. In **Edit**, select a layer and expand **Alternatives** in its source inspector. In **Perform**, right-click an assigned source card or focus it and press **Shift-Return** to open the same controls. The four Blend cards and the XY pad retain their existing meanings.
2. Use **Add alternatives**, drop files, or drag a Library/Browser sound or marked region into the disclosure. Adding does not change the active source. Different ranges of the same recording have distinct entry IDs.
3. Click a row or focus it with the arrow keys and press **Return**. Previous/Next selects the neighboring entry. A first use opens **Review input and mapping**. Confirm physical channels, ordered interpretation and routes to the current design. Unknown six-channel files require explicit roles; channel count alone does not mean surround.
4. Selection verifies content in the background and updates the source name, waveform, range and routing together. Successful selection stops owned Live Designer playback. Press Play to audition the newly prepared design. Failed selection retains the previous source and playback intent.
5. **Options** reviews an entry, enables/disables it for variations, changes row order, or relocates identical content. Remove of the active entry explicitly asks which reviewed source to use instead. These edits support Undo/Redo.

The selected checkmark identifies the active source. Keyboard focus can address another row without changing that source until Return. Previews show the real source range and layer reverse state before gain, fades or effects; they are not post-effects renders. Loading, silence, offline, changed and decode errors are distinct. Only visible and focused previews request work. Per-channel inspection remains beside Routing in the layer inspector.

Placement, rate, reverse, gain, pan, fades and effects belong to the layer. Entry range and the complete reviewed routing belong to the source, including spatial mode/position/width/divergence and LFE policy. Selecting an entry recalls those routing settings. Relative sync time converts between source rates and clamps into a shorter range; existing valid fades, including overlaps, remain intact. One Undo restores the complete previous selection.

## Source variations and capture

Enable **Choose alternate source** in variation options to opt in. The independent source-choice lock prevents seeded source changes while allowing ordinary gain/timing/effect variations. Whole-layer locks still apply. Disabled entries are excluded from seeded draws but remain manually selectable. Missing entries are never silently excluded or rerolled; a missing resolved choice fails explicitly.

Variations persist a resolved source, policy, seed and ordinal. Playback, render and Record do not draw again. Record verifies the current choice and freezes its exact source, fingerprint, original-frame range, interpretation, physical inputs and mapping. The take retains versioned pool-origin provenance, not a mutable pool. Source switching, pool edits and rerolls are unavailable during Arming, Recording and Finalizing. Return to Live Design to edit alternatives after capture.

Live seeks retain the existing approximate 250 ms effect history. **Accurate audition** rebuilds history from timeline zero for judging long tails. Neither path introduces sample-trigger events. Capture/replay and offline take rendering retain complete history and the existing typed XY/macro/spatial event contract.

## Channels, unavailable files and compatibility

The source-input limit is 64; the processing/design bus limit is 16. Wider sources need an explicit supported physical subset or the routing-v2 all-channel arithmetic Mono mean. The review displays physical input numbers, ordered roles, nonzero route coefficients and LFE policy. It does not infer a new layout from the previous alternative. A bus change invalidates alternate reviews for the old destination; review before using them again. Legacy physical pairs, odd final channels and Analysis Mix preserve their existing behavior.

Offline entries remain in the document. Reconnect the original unchanged source and select again. For a new location, use **Relocate identical content**; content must match the verified fingerprint. Changed/replaced recordings must be added as new alternatives. Take relocation remains separate and preserves the original frozen identity. No drive is required to remain mounted merely to view its inactive alternative.

Interleaved, split export, explicit downmix and Print operate on the resolved Live choice or selected frozen take. Output-set roles, paths, hashes and current-context selection remain independent of export presets. A missing selected split member makes that selected output unavailable; another receipt is not substituted. Durable renders are retained.

Manifest **7** adds `sourcePoolMetadataVersion: 1`. Existing manifests 1–6 remain supported. No-pool recipes receive no pool fields. Pool metadata does not rename DSP: `stock-takes-v1`, frozen `stock-layout-takes-v1` and `stock-layout-takes-v2` retain their hash meanings and dispatch. See [the ownership and deterministic algorithm contract](SOURCE_POOLS_V1_5_CONTRACT.md) and [executed evidence](SOURCE_POOLS_V1_5_REPORT.md).

## Repeatable local checks

From the repository root:

```sh
.tools/cmake/data/bin/cmake --build build --parallel 6
.tools/cmake/data/bin/ctest --test-dir build --output-on-failure
open build/SoundPlus_artefacts/Release/SoundPlus.app
```

Use an isolated generated QA catalog without changing your ordinary library:

```sh
build/SoundPlusTests_artefacts/Release/SoundPlusTests --prepare-source-pool-ui "$PWD/build/source-pool-demo"
build/SoundPlus_artefacts/Release/SoundPlus.app/Contents/MacOS/SoundPlus \
  --data-dir "$PWD/build/source-pool-demo/catalog" \
  --add-folder "$PWD/build/source-pool-demo/media" --disable-ai
```

Close SoundPlus before rebuilding its bundle. Generate the demo once per fresh directory; repeated preparation replaces only that explicitly owned demo fixture.

For a five-minute check with your library: select an existing layer, add two alternatives (one can be a marked Library region), choose/review them, Play, switch while playing and confirm it stops, then restart. Undo/Redo, lock source choice, generate source variations, save/reopen and confirm selected sources/ranges. Record a short take, return to Live and change its pool; return to the take and render it. Its original chosen source must remain. Unmounting/reconnecting a real external volume and physical surround listening are additional manual qualifications, distinct from generated PCM and controlled missing-file tests.
