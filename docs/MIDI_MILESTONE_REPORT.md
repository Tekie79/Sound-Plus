# MIDI sequencing and instruments — development report

Starting/reviewed HEAD:949dbc5619c9921cd52f5588a5b91a94d9523884. No later commits or unrelated changes at start. Read the current recording, automation, take, hosting and format contracts. Preserve all existing catalog/media/recording prefixes and finalized/Resolve-referenced WAVs. No push.

## Stage A prerequisite checkpoint

| Review finding | Correction | Evidence |
| --- | --- | --- |
| A1 Previously active monitor survives rejection | Disable and clear matrix before validation; epoch-tag queued frames, discard stale backlog, reset peaks only at the safe device boundary. Output-device changes disable the raw monitor. Separate monitor-command serial reconciles actual state/error with current UI intent. Raw capture remains independent. | Real service/session tests enable48k, reject44.1k, inspect actual silent monitor output, safely re-enable, restart/reconfigure, reject bad mapping, and repeat during capture with identical unprocessed samples. |
| A2 Acknowledged raw PCM not content-validated | Version2 per-owner SHA-256 prefixes, byte counts and owner binding; incremental worker hashing with all-owner commit. Validate all prefixes/finite PCM before derived writes, then verify the copy digest. | Real stores test independent SHA oracle, same-size raw mutation, nonfinite data with a matching mutated digest, mismatched owner prefixes, first-owner-only append failure, retained checkpoint/raw bytes and interrupted WAV finalization. |
| Legacy policy | Default v1 partial recovery refuses. Explicit legacy action stays labelled `legacy-unverified` and Recovered (unverified); finite/size checks still apply. Old finalized WAV hashes and old files are retained. | Clean v1 recovery requires opt-in and retains the unverified receipt marker. |

Source: [InputRecording](../src/InputRecording.cpp), [session/UI acknowledgments](../src/DesignInputSession.cpp), [helper device changes](../src/AudioEngine.cpp), [real-store/service tests](../tests/InputCorrectionChecks.inc). The [recording contract](AUDIO_INPUT_RECORDING_CONTRACT.md) now specifies this version/compatibility policy.

Stage A final relevant gate: **7/7 passed in34.37s**, with **195 input checks**. [Gate](verification/midi/stage-a/gate.log), [source/build identity](verification/midi/stage-a/identity.json), [controlled input evidence](verification/midi/stage-a/controlled-input.json). This stage changes no MIDI model/engine/UI and will be committed separately before MIDI implementation. Existing microphone/device/listening/receiver qualifications are not expanded by controlled regressions.
