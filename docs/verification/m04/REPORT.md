# Creative native effects

The tested source is `9238098e3c6b9b04a6e6ebcfe1aed8e6f1bb13b9`. The isolated macOS arm64 Release executable SHA-256 is `cec720adf3235b4f305baa31d7f330e5c9d6eec08b2874b0614824d104857e15`.

Granular Texture uses two seconds of prepared history per channel and a bounded pool of 32 grains. Even the 400 ms and 60 grains/s extremes fit the pool. Its sequence derives from the saved instance ID and seed. Freeze Buffer stops writes to the ring. Mix normalization caps the sum of overlapping windows. The named right channel can read from a separate offset without folding channels together.

Spectral Freeze and Spectral Blur use prepared 256–2048-frame FFTs, sine windows and normalized 50% or 75% overlap-add. PDC equals one window, including on bypass. Freeze captures the latest full window at the applied Hold event, including between hops. Release rejoins the current input through the ordinary overlapping windows. Held effects can sustain throughout the requested export tail. Stutter records the first slice in each group and replays it with a short gate ramp. A live seek into an incomplete group resumes capture at the next complete group. Accurate audition rebuilds source history. All four use the existing native rack, parameter IDs, automation, presets and frozen mixes. The default LFE path carries compensated dry audio.

Independent numeric checks compared an isolated grain with its analytic Hann amplitude and per-channel impulses with their expected delayed samples under both overlaps. Coverage includes 44.1, 48 and 96 kHz, 1/32/61/257/512-frame partitions, changed instance seed, off-hop freeze and release, clocked eighth-note repeats, both named 5.1 layouts, stable automation targets, Save/Reopen and frozen recipe round-trips. [Initial DSP gate](granular-spectral.log) · [Stutter gate](stutter.log)

The native Edit inspector reopened a generated design with two Spectral Freeze inserts. Expanding the parameters displayed Window at 1024 frames and Overlap at 75%. Human listening, multi-speaker evaluation, creative-effects-specific 24-strip soak and extended operator scenarios remain in M09.

The complete Release regression ran unchanged after the final build. All 60 tests passed in 503.96 seconds, including the AddressSanitizer effects target. [Full Release output](release.log)
