# Multitrack correctness — Stage A

Starting HEAD `370ba95`, reviewed baseline `d14e23bd3808af67c4d409e0b9295c8bedd86774`. The later Perform axes, equal sidebars and pad limits are retained. No audio-input implementation is included in this Stage A commit.

| Finding | Correction and evidence |
| --- | --- |
| A01 — output provenance | Ordinary multitrack stock and hosted renders retain `multitrack-automation-v1`; hosted raw-pass authority, graph and SHA-256 remain separate. Nonempty processing labels survive document save/reopen. Existing receipt labels are not upgraded. `DesignAudio.cpp`, `DesignModel.cpp`; generated WAV/receipt checks in `MultitrackTests.cpp` and `OrderedInsertTests.inc`. |
| A02 — split automation/history | Right fragments have independent insert/rack identities and cloned source-effect curves. Each independently reconstructs the dry source history; audible gates hand off at the exact project frame and only the final fragment owns the tail. Forward/reverse and nonintegral-rate tests, exact-cut keyframes, Gain/Filter/Delay, owned hosted latency, repeated splits, saved/reopened PCM and actual Split/Undo/Redo are covered. Existing historical frozen hashes remain under the existing compatibility suites. |
| A03 — automation editor lifetime | Headers/lanes retained by stable owner within the owning document incarnation/variation. Unrelated disclosure preserves component identity, native keyboard focus and active Touch. Hiding/removing an owner ends its gesture. Replaced contexts reject stale callbacks; numerical completions bind target/selection. Tests exercise Touch across disclosure, one Undo, hide/next edit, document replacement and actual view commands. |

The explicit split contract and its boundary-edit policy are in [MULTITRACK_AUTOMATION_CONTRACT.md](MULTITRACK_AUTOMATION_CONTRACT.md). New continuation-bearing documents require manifest12; documents without continuations and historical take hash encodings retain their prior envelope/bytes. This prevents an older build from silently ignoring the continuation.

Native stock filtering in the legacy-stereo multitrack path now uses the same project-anchored coefficient scheduling already used by layout-aware/frozen multitrack playback. The new split comparison exposed the earlier block-partition difference. Legacy non-multitrack processing remains unchanged.

The complete gate exposed an existing lifecycle race under load. A controlled repetition reproduced an Incomplete take whose first diagnostic was a placeholder while the callback was still publishing the real fault. The worker-only diagnostic reader now waits (bounded250ms) for the release publication; no waiting or new work is added to the device callback. A deterministic held-publication regression and failure-only observation logging cover it. The initial failed/interrupted gate and reproduction are retained separately; they are not reported as passes.

## Verification

Targeted gate: multitrack automation, Designer view commands, Designer session, historical ordered corrections and ordered inserts. [Log](verification/input-recording/stage-a/targeted.log).

Final unchanged build: **34/34 suites passed in313.14s**, with `SOUNDPLUS_VENDOR_QUALIFICATION`, `SOUNDPLUS_QUALIFY_AU=1` and `SOUNDPLUS_LIVE_DEVICE_QUALIFICATION=1`. [Complete log](verification/input-recording/stage-a/full-gate.log), [before identity](verification/input-recording/stage-a/identity-before.json), [unchanged-source/binary proof and compiler](verification/input-recording/stage-a/identity-after.json). The multitrack suite passed212 checks; the corrected lifecycle suite also passed ten consecutive runs in47.16s. The initial gate was stopped after its lifecycle failure and diagnosis; its failed log and the subsequent reproduction remain in the same evidence directory. No sanitizer or new physical-input claim is made.

The user's current Workflow demonstration r4478, catalog, recovery data and all37 referenced/media output hashes were protected before a normal Quit. No older snapshot was restored over it. QA uses separate generated media and catalogs.

This stage does not qualify new microphone capture, human listening, physical surround monitoring, VST2/AUv3, OldSkoolVerb custom gestures, MIDI instruments or native receiver drag. Stage B adds Audio Track input capture separately from Perform control recording.
