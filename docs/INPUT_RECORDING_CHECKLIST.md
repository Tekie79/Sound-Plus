# Multitrack corrections and Audio Track recording — tasks in progress

Starting HEAD: `370ba95`. Stage A committed as `1800bc6`. Reviewed baseline: `d14e23bd3808af67c4d409e0b9295c8bedd86774`.
Preserve the later Perform axes, equal sidebar widths and bounded pad sizing.

## Stage A (commit before Stage B)

✓ Preserve ordinary multitrack output provenance, including hosted raw-pass authority and save/reopen.

✓ Sound-preserving split: independent processor histories, copied stable automation bindings, tails, persisted continuation, historical hashes.

✓ Retain unaffected automation editors by owner identity; finish/cancel gestures safely and reject stale callbacks.

✓ Targeted multitrack/view/session/hosted regressions. Lifecycle publication race reproduced and corrected; ten repeated lifecycle runs pass.

✓ Complete vendor/AU/device gate:34/34 in313.14s; source and six binaries unchanged. Stage A report and commit.

## Stage B

✓ Extend the supervised audio-device helper with bounded, timestamped raw input capture and optional monitoring (default off).

✓ Durable recording journal, atomic WAV finalization, cancellation and explicit partial/failure recovery.

✓ Audio-only track arm/input selection/meters and shared Record/Stop; normal editable clips and one Undo per completed capture.

✓ Controlled mono/stereo/multiple-track, timing/latency/rate, PCM, persistence and fault tests.154 input checks; seven focused suites pass in23.05s. Ruler-drag isolation and one-hour boundary regressions reproduced before correction.

✓ Real built-in Mono mic, empty/populated Audio Tracks, raw/WAV equality, saved/reopened clips, native Undo/Redo, R/Space/Escape and actual helper/app interruption recovery.1040×680 Browser+Inspector inspection. Physical Stereo/separate-input/surround hardware is unavailable.

✓ Complete final unchanged-build gate:35/35 in305.68s;282 source/resource and six executable hashes unchanged. Requirements, contracts, report and native evidence retained.

✓ Reopened the original in-place Workflow demonstration r4478; full payload,1 root/43 assets and37 media/output hashes match after launch. Perform, Browser, current XY/Macro/monitor settings restored.

✓ Committed completed source and verification evidence after Stage A; no push.

Evidence will distinguish controlled tests, actual hardware, human listening and receiving-app acceptance.

Current evidence: native microphone sources remain local in the isolated QA catalog. Normal Stop receipts measured zero input overruns/output underruns, with max native input callback10.75µs. Actual SIGKILL helper/application prefixes recovered as labelled clips; cancellation remained partial. Latest original Workflow demonstration r4478, preferences,1 root/43 assets and37 source/output hashes compare unchanged before relaunch. Final corrected full vendor/AU/device gate passed35/35 in305.68s. No human listening or new receiving-app qualification is claimed.
