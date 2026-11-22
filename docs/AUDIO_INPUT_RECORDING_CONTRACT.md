# Audio Track input recording v1

This extends the existing supervised `SoundPlusAudio` device process. Library audition, the layered/Perform project model, retained insert services, playback PCM consumer and source renderer remain shared. Perform control-event takes and microphone/interface source recordings are distinct operations.

## Input and transport

Only Audio Tracks have physical input settings and arm controls. Track options choose a registered CoreAudio device UID, explicit physical input channel(s) and interpretation. Mono, Stereo pairs and exact reported named layouts are supported. Channel count alone cannot invent surround roles; CoreAudio tag/bitmap descriptions are resolved to ordered labels and matched against the existing typed channel contract. Unknown/incompatible layouts are unavailable. MIDI remains a separate typed Instrument-slot contract with no new capture/instrument controls.

Up to eight armed tracks may capture independent channel selections from one device simultaneously. Separate devices/aggregate-device creation are outside v1; an existing compatible aggregate device can be selected as one CoreAudio device. A recording appends a new normal clip per owner; populated tracks and original files are retained. One Undo removes the inserted clips without deleting the recorded WAVs. Input settings persist; arms and monitoring do not automatically reopen on launch or document replacement.

`R` starts/stops Audio Track recording in Edit; shared Stop/Space stops and finalizes, Escape cancels. Recording options expose cancellation and retained-source recovery. Incompatible structural audio edits, Undo and document replacement require Stop. Harmless metadata/disclosure edits remain available and completion appends to the current document rather than restoring its starting snapshot. Retained native-plugin state observations keep their existing owner/generation/capture boundary. A topology or processing failure interrupts capture explicitly.

The existing playback graph supplies the recording transport, including silent time after current clips end and empty-track sessions. Its temporary audition snapshot has a one-hour clock extent; this does not change the editable design duration. Ruler, marker-navigation and playhead-drag seeks cannot move or finalize an accepted input recording. Capture stops incomplete before a new block would exceed the one-hour project envelope, retaining only its valid prefix; a recording started near that limit has less available time. Existing PCM callback underruns, device loss and processing failure invalidate the recording rather than silently certifying an uninterrupted take.

## Clock, channels and monitoring

The input-only HAL IOProc uses `inInputTime.mSampleTime` and `mHostTime`, identifying the first acquired hardware sample (Apple SDK AudioHardware.h). Every output callback publishes its actual PCM-consumer project frame, generation and presentation host timestamp. The writer maps acquisition time into that project clock and trims pre-start input. HAL acquisition timestamps already account for input acquisition timing: reported input latency/safety offset are retained as provenance and are not subtracted a second time. Frame placement rounds once. Physical acoustic/electrical round-trip calibration is a separate qualification.

Source WAVs use the actual configured input rate; a different project rate remains explicit and the existing source renderer resamples during audition/render. No source is relabelled to the project rate. Named WAV ordering uses the existing role-to-WAV mapping. Capture supports native float32 CoreAudio input streams, at most16 physical channels and4096 frames per callback. Unsupported formats/bounds fail explicitly.

Monitoring starts **off**. The optional path is raw input through the existing Stereo monitor output, with the existing explicit named-bus conversion and monitor gain; it bypasses design inserts and never alters recorded source PCM. It requires matching input/output nominal rates. It is not a separate playback device/engine. Monitor ring drops are counted; no asynchronous multi-device monitor resampling or long-duration clock-drift qualification is claimed in v1.

## Real-time and durability boundaries

Input callbacks validate/copy samples into a fixed32-slot SPSC ring, update atomic meters/faults/timing and optionally fill a separate bounded Stereo monitor ring. Output callbacks consume their existing preallocated PCM plus the monitor ring. Neither callback performs file/database I/O, JSON, locks, allocations, graph construction or file finalization. Timestamp/sample discontinuities, unavailable timestamps, nonfinite/missing PCM, oversize blocks and ring overruns fail closed. Callback work and underrun/overrun counts are exposed separately from writer/finalization time.

The disk worker writes unprocessed interleaved float32 source files under the catalog's permanent `recorded-audio` directory. `.operation.partial/journal.json` acknowledges a common frame prefix only after all source files are fsynced. Checkpoints occur at most every250ms while the writer is healthy; a stall beyond buffer capacity causes an explicit incomplete prefix. A normal Stop drains accepted blocks, creates role-correct float WAVs, syncs files/receipt and atomically renames the source-set directory within its parent. No incomplete operation is called Complete/Ready. The safety envelope is one hour and less than1.75GiB per raw source file, whichever is reached first; reaching a safety bound retains an incomplete prefix.

Cancel, denial, I/O failure, helper/app interruption and device loss retain their actual status. Cancelled/interrupted prefixes are never silently inserted as successful captures. Deliberate recovery publishes a labelled `recovered` receipt and only the acknowledged durable prefix. Finalized but unclaimed source sets can also be added deliberately after an app interruption. Checksums/header checks precede timeline insertion; stale owner/document incarnations cannot replace current edits. Duplicate operation IDs never overwrite sources. No automatic cleanup deletes recorded originals or durable outputs.

Documents containing input settings or recording provenance require manifest13. Documents without those fields and historical take recipe encodings retain their previous envelopes and hashes. App/audio IPC is version11. The existing plugin format/identity, isolated DSP and single-pass output-authority contracts remain in effect.

## Journal v2 prefix integrity and monitor correction

New journals/receipts use version2 with `sha256-raw-prefix-v1`. Each source retains its stable owner, acknowledged byte count and SHA-256 of exactly that prefix. The writer advances incremental hash state only after every owner's append succeeds; fsync precedes journal publication. Checkpoint hashing is bounded by owner count and does not reread growing files. Hashing never runs on an audio callback.

Before recovering/publishing, verify every owner's acknowledged byte count, digest and finite float32 data, before touching any derived WAV or receipt. Longer unacknowledged suffixes are ignored. A second digest during WAV staging detects changes between validation and copying. Any mismatch/nonfinite source refuses publication and retains raw files/journal/staging. Interrupted WAV finalization rebuilds from verified raw authority.

Version1 partials have no historical prefix digest. Default recovery refuses with an explicit reason. The separate **Recover unverified legacy** action validates available size/finite PCM and labels both receipt (`legacy-unverified`) and clip (**Recovered (unverified)**); it never invents a verified historical checksum. Previously finalized v1 WAVs remain governed by their retained output SHA-256; no old journal, receipt or source is automatically rewritten/upgraded.

Rejected monitor requests disable monitoring, clear the old matrix, and discard prior ring epochs before re-enable. Epoch-tagged source frames reject late old-configuration publication without resetting producer/consumer indices from a control thread. A safe input-device reconfiguration resets peak meters. Output-device changes turn monitoring off explicitly. Monitor acknowledgments carry a separate serial so stale status/error cannot undo a newer UI action. These changes do not alter recorded source PCM.

## Qualification

See the milestone report for actual controlled, native, physical-device and failure evidence. Simulated stereo/multiple-input/role tests do not qualify unavailable physical interfaces. PCM verification is separate from human listening. No new VST2, AUv3, OldSkoolVerb custom-gesture, physical surround, MIDI-instrument or receiving-application drag qualification is implied.
