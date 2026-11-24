# M03 development verification

Built on M01 `417e062`. In Find Similar, select a recording with a match, then choose **Refine** above its waveform. Review the suggested event or choose another. **Audition suggested** and **Audition original window** use the same channel interpretation. **Apply suggested range** sets the ordinary editable IN and exclusive OUT. **Use full match** restores the original interval.

Analysis uses native-rate 5 ms frames, maximum physical-channel energy, frequency-weighted positive spectral flux, an adaptive floor and release grouping. An isolated reference event can rank hypotheses by duration and envelope alignment. Up to eight hypotheses survive. Ambience and unclear events retain the full window. These suggestions describe transient boundaries. Semantic ranking remains owned by Find Similar.

Each request analyzes one candidate and an optional reference, capped at 32 seconds and 16 channels per window. Encoded source material stays in place. The worker verifies revision and file stamp, hashes decoded window content before reusing a cached result and bounds the derived cache to 128 entries. Request cancellation also invalidates outstanding menu callbacks.

The seven focused Release tests passed on Apple Silicon. Generated fixtures check exact boundaries at 44.1, 48 and 96 kHz, a rear-channel-only impact, opposite polarities, two events, steady and quiet audio, cache repair, cancellation, and stale/offline sources. A JUCE view-command test checks a second hypothesis, bookmarking, transfer, restoring the coarse window and pin preservation. Existing search, selection playback and performance regression suites passed. [Execution log](release.log)

Human judgments of event quality, physical auditions and the consolidated large-library acceptance matrix remain open for M09.
