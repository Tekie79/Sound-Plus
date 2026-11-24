# M08.1 local acoustic category review

Select one recording in Library, open **Edit catalog metadata** in Details and click **Suggest from audio**. The menu gives up to five categories and their raw similarity scores. Preview auditions the strongest original sample interval. Apply sets a searchable user-owned category. Reject suppresses that category for this recording revision and classifier version. Both actions use catalog Undo/Redo. Existing embedded, derived and user categories take precedence. Filename suggestions remain available when audio analysis is unavailable.

## Processing contract

The optional local CLAP helper produced text prototypes for 82 UCS categories from two prompts per category using the pinned taxonomy and aliases. The bundled resource includes both prompts, the original taxonomy digest, model/preprocessing identity, normalized vectors and an encoded silence reference. ARCHIVED and UNCATEGORIZED are organizational labels excluded from ranking. Prototype generation is reproducible with `scripts/build_acoustic_prototypes.py`. Querying runs lazily on a cancellable background reader and reuses the existing all-channel analysis mix.

At most 32 indexed seeks span the recording. A qualifying window must have a finite normalized vector and cosine below 0.97 against digital silence. Each category scores 65% strongest-window cosine and 35% mean of its three strongest available windows. Return Unknown if the best score is below 0.32 or leads the runner-up by less than 0.03. Keep scores within 0.10 of the winner, with a 0.32 floor. These are heuristic operating thresholds. Scores have no probabilistic calibration. Long recordings can contain events between sampled windows. Opposite-polarity channels can cancel in the existing analysis mix.

Reads and review writes check the asset ID, revision, path and file stamp. Rejections live in the migrated SQLite v9 catalog. Writes on a newly assigned category or a changed source fail. The UI drops results for cancelled requests. Reviewing audio creates no source-file changes.

## Development observations

Five focused Release tests passed in 11.98 s, including ranking, bounded reads, stale/offline cases, catalog edits, explicit rejection and undo, similarity and collections. The playback test ran an embedding write and acoustic query during Repeat One and a Library rescan and retained the real audio helper's active playback generation.

All 43 fixture files matched the source hashes in the local ESC-50 evaluation manifest. 27 returned suggestions and 16 abstained. Two rain files ranked RAIN first. Both sea-wave recordings ranked NATURAL DISASTER first. One door creak also ranked RAIN. These errors make explicit operator review important. The JSON records every result, score, source hash and preview interval. No fixture audio is redistributed. [Fixture observations](fixture.json)

On the installed local model, the same generated decaying transient placed in channel 1 or channel 6 yielded equivalent embeddings and RMS. The rear-only signal scored 0.236 against digital silence. A near-silent 16-bit stimulus scored 0.979 and met the near-silence abstention threshold. [Encoder measurements](channel-check.json)

In isolated native macOS windows, a QA operator action through accessibility selected clip-003, requested suggestions, invoked moment preview, rejected RAIN, verified its disappearance, used Undo, and applied the restored suggestion. Both compact and desktop captures show the full review row. The frozen candidate reopened the editor and returned RAIN at 0.38 in both the 1040 × 680 and 1480 × 920 logical-point QA configurations. Both screenshots are from that candidate. Human listening and representative library accuracy review remain open for M09. [Compact](compact.jpg) · [Desktop](desktop.jpg)

### Repeating the development measurements

After indexing the isolated fixture with the pinned model, run the Release test executable against a disposable copy of that catalog. It reports each ranked category, the unrounded score and the matching half-open sample interval. The local evaluation corpus consists of 40 attributed ESC-50 excerpts and three derived composites.

```sh
"$build/SoundPlusTests_artefacts/Release/SoundPlusTests" --inspect-acoustic-fixture "$qa_catalog"
ctest --test-dir "$build" --output-on-failure -R 'acoustic_metadata|catalog_metadata|library_collections|library_playback_search|library_similar_integrity'
```

For the separate encoder measurement, generate 48,000 signed 16-bit PCM frames at 48 kHz with six interleaved channels. In the single active channel use `round(A * exp(-(i % 6000)/260) * sin(i * 0.31))` at frame `i`. Use amplitude 29,000 for front and rear comparisons, zero for digital silence, and one for the near-silent rear stimulus. Encode the full interval with the same model and preprocessing IDs as the prototype resource. Read the helper's analysis RMS and compare the normalized vectors by dot product. This measures the encoder's channel policy. Physical surround monitoring and human audibility require separate observations.
