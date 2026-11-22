# Multitrack corrections and Audio Track recording — tasks in progress

Starting HEAD: `370ba95`. Reviewed baseline: `d14e23bd3808af67c4d409e0b9295c8bedd86774`.
Preserve the later Perform axes, equal sidebar widths and bounded pad sizing.

## Stage A (commit before Stage B)

✓ Preserve ordinary multitrack output provenance, including hosted raw-pass authority and save/reopen.

✓ Sound-preserving split: independent processor histories, copied stable automation bindings, tails, persisted continuation, historical hashes.

✓ Retain unaffected automation editors by owner identity; finish/cancel gestures safely and reject stale callbacks.

✓ Targeted multitrack/view/session/hosted regressions. Lifecycle publication race reproduced and corrected; ten repeated lifecycle runs pass.

✓ Complete vendor/AU/device gate:34/34 in313.14s; source and six binaries unchanged. Stage A report and commit.

## Stage B

○ Extend the supervised audio-device helper with bounded, timestamped raw input capture and optional monitoring (default off).

○ Durable recording journal, atomic WAV finalization, cancellation and explicit partial/failure recovery.

○ Audio-only track arm/input selection/meters and shared Record/Stop; normal editable clips and one Undo per completed capture.

○ Controlled mono/stereo/multiple-track, timing/latency/rate, PCM, persistence and fault tests.

○ Real supported Mac input capture and minimum-size native UI checks; preserve current user session/media.

○ Complete final unchanged-build gate, build/source identities, requirements/report, final commit. No push.

Evidence will distinguish controlled tests, actual hardware, human listening and receiving-app acceptance.
