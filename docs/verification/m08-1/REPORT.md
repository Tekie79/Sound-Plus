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

In isolated native macOS windows, a QA operator action through accessibility selected clip-003, requested suggestions, invoked moment preview, rejected RAIN, verified its disappearance, used Undo, and applied the restored suggestion. Both compact and desktop captures show the full review row. Screenshots precede the final near-silence threshold adjustment. Human listening and representative library accuracy review remain open for M09. [Compact](compact.jpg) · [Desktop](desktop.jpg)
