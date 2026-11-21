# Tasks in progress — Live Insert Editing, Metering and State Synchronization

Authoritative new milestone request: `/Users/harvest/.codex/attachments/b609d7ee-f2a4-467a-9424-d0cd009551be/Pasted text.txt`.
Starting HEAD: `185ed9aac74633e336d24df46a3c40f164af61f9`, initially clean. Early uncommitted separate-instance forwarding work was captured in `build/plugin-editor-live/provisional-bridge.patch` and removed from source because B1 explicitly requires the audible instance. It must NOT be used as the final live path.

- ✓ Read AGENTS.md, the complete new prompt and four referenced reports/contracts.
- ✓ Gate A1: coordinate per-canonical-registry writers; revisioned reconciliation with latest roots/recovery/entries. Deterministic older snapshot → newer publication → older resume test; cancellation/dismissal/failure.
- ✓ Gate A2: coherent worker result.job state/parameters/baseline; consistent defer-to-opaque effective controls through live return/undo/fresh/save/render; owned prepare-time schema mutation rejected in finite and persistent paths.
- ✓ Gate B1: native editor belongs to retained audible isolated processor; explicit processor generation; UI on required message thread; opening/closing does not reset DSP/tails.
- ✓ Gate B2: one authoritative bidirectional native/inline/program/opaque/undo/reset/save/render flow, coherent open-editor save, sequences and gestures.
- ✓ Gate B3: original incarnation/variation/owner/slot/generation lifetime, independent Track/Master windows, copies/moves/removals/reprepare/crash/unresponsive/shutdown; bounded recoverable checkpoints.
- ✓ Gate B4: actual signal-driven Youlean meter checks on Track and Master, known levels/silence/steps/reset; exact identity; no inference from window/transparent PCM.
- ✓ Auto-open/focus new insert editor; float while Designer is used. Hide failed/unsupported/incompatible plugins from normal selection; keep diagnostics retained separately.
- ✓ User steering: icon-based Designer playback/navigation controls plus real persistent timeline markers; avoid a broad redesign.
- ✓ Preserve role layouts, mono-before-surround, per-channel impulses/LFE/latency/output-authority; TAL VST3 duplicate-ID rejection and open VST2/AUv3 requirements.
- ✓ Owned production regressions, actual LV50A native gesture/Save/Undo/reopen/render, timestamped response and raw records.
- ○ OldSkoolVerb native gestures: BLOCKED by unconfirmed custom-control interaction through automation; numerical Track/Master/layout/render/tail cases pass. Do not count a transient display value as accepted DSP state.
- ✓ Final full gate with explicit vendor/AU branches; exact final code/helper/frontend identities; affected final native checks; separate source/PCM/native/meter/listening/physical-surround/receiver results.
- ✓ Restore latest user state in place: Workflow demonstration r36, unsaved, 6.0 s, clip-005, monitor .25, TAL AU retained; no older snapshot copied.
- ✓ Final evidence/report and local delivery commit (no push). Native OldSkoolVerb gesture qualification remains explicitly blocked above.

Protected current user session: Workflow demonstration **r36**, unsaved, playhead **6.0**, selected clip-005, monitor **.25**, newly added TAL-Reverb-3 **AUv2**. Backup/hash/catalog files under `build/plugin-editor-live/protected`. Original app closed through native UI before the provisional build. The provisional bridge was removed and the current bundle uses the retained per-slot service; native vendor verification remains pending. Do not restore the older r34 catalog.

Gate A targeted run: `plugin_gate_a` passed 53 checks in 9.28s. Includes deterministic canonical-alias registry interleavings (older cache, cancellation/dismissal, older helper crash), returned-job baseline round-trip, opaque .75 → edit .6 → Undo/return → live/fresh/finite/save/reopen/export and legacy override, and actual owned VST3 native-ID change on the second setupProcessing (post-prepare-specific rejection on finite and persistent paths). Existing plugin-processing/correctness regressions passed before Gate B architecture work.

Gate A completed: focused 53 assertions (9.28s), existing plugin_processing (60.98s) and plugin_correctness (10.47s), both passed. Evidence under docs/verification/live-inserts/gate-a. No native/PCM/metering Gate B acceptance is inferred from this checkpoint.

Gate B source checkpoint (in progress): same retained slot processor owns native UI and supplies borrowed audition PCM; independent slot services survive selection/other-slot changes. Generation/sequence/delta/restore-operation guards, coherent captures, native gesture history, asynchronous Save/Render/copy/move/transition gates, bounded health/checkpoints, normal-chooser filtering, Designer navigation and persistent markers implemented. Not a vendor/native/metering acceptance yet.

Owned focused evidence: `plugin_live` passed **50 checks / 11.83 s** under ASan (owned fixture instrumented; helper ASan runtime linked). Tests include native-before-close PCM; GenericAudioProcessorEditor bad-free regression after host private-symbol hiding; close/reopen no reset; program/inline/fresh state; actual session open-editor Save, Undo/Redo, Track/Master, duplicate, move, unrelated/owner removal, failure and shutdown; marker persistence/navigation/history. `plugin_correctness` passed **15.88 s** after its chooser expectations were updated to hide failures normally and retain explicit recovery diagnostics. `plugin_gate_a` passed **15.31 s** under the instrumented fixture configuration. No hardware listening, physical surround or receiving-app acceptance is inferred.

Expanded Release real-device checkpoint passed 70 assertions in 24.97 s, including periodic unnotified opaque-state capture/fresh PCM/Undo, separate canvas/native gesture history, explicit bound reconnect and failed-worker shutdown. Current raw root: `/Users/harvest/Documents/Ysew_Project/GIT/Sound-Plus/build/plugin-editor-live/tests/6035dd21fd6f4d58baa07a369535ac39`. Final full gate and vendor/UI/meter acceptance remain pending.

Latest corrective checkpoint: Release `plugin_live` passed **98 checks / 34.10 s**, with `SOUNDPLUS_LIVE_DEVICE_QUALIFICATION=1`. Covers native-to-inline/PCM, actual fixture native slider reflection, notified opaque/program Undo, silent opaque capture/fresh Save restoration, overlapping control timestamps, EOF coherent save without history advance, and actual Duplicate command retaining its clicked owner across asynchronous capture. Silent unnotified opaque changes are observations, not invented user Undo gestures. Final full gate and final vendor/native metering checks remain in progress.

Final source corrections committed through `35fe786`: B1/B2/B3 implemented and owned production paths verified. Native Youlean testing exposed repeated unchanged content-scale notification after view attachment: a generic pinned-JUCE guard now deduplicates it, correcting half-size drawing and displaced native hit tests. Actual Track and Master Stereo meter steps/silence/reset and accepted Rear service with Auto 5.1 native mode pass. LV50A final-helper click/drag, open-editor Save, Undo/Redo, retained close/reopen output and fresh native render pass numerically. OldSkoolVerb opens/plays/closes, but its custom mouse gestures remain unconfirmed through automation and must not be marked passed.

Minimum native window verified at outer 1040×680 (saved content 1040×648 plus title bar), Browser and Inspector visible; controls reachable, inspector scrolls above fixed monitoring. Actual marker Add, Start and Next navigate to the saved marker. First post-scale full run was interrupted by confirmed clamshell sleep/dark wakes; retained separately, not accepted. Awake focused/full reruns in progress. No hardware/listening or receiver acceptance inferred.

Final unchanged source `35fe786`: awake full gate **29/29 / 226.79 s**; explicit vendor, AU and live-device branches. Prior failed post-scale run coincided with documented clamshell sleep/dark wakes and is not accepted evidence. Awake focused retry 3/3 /108.19 s also passes. Final owned run: **98 checks**. Raw control-to-DSP native n20 p50 4.993 ms/p95 10.773 ms; Main state receipt p50 38.962 ms/p95 50.866 ms. One final coalesced inline event is a single observation, not an independent 40-sample latency distribution. Native both-owner final-helper visibility, distinct generations, positive processed frames and open-editor Save repeated. LV fresh app reopen recalls −6 dB, marker and finalized output. Earlier synthetic periodic opaque Undo assertion was superseded: silent opaque changes now remain observations without an invented gesture.

Latest correction pass: [closure/evidence report](LIVE_INSERT_CORRECTIONS_REPORT.md), final source `a6dedbd`; 30/30 gate, retained live 180 assertions, owned scale contract and focused worker/fixture ASan. LV50A/Youlean rerun on final native binaries. OldSkoolVerb custom-control input remains explicitly unconfirmed/open; its native stateful acceptance is not closed by numerical inline tests.

## Ordered-chain follow-up

See [ordered-inserts report](ORDERED_INSERTS_REPORT.md) for the new source/build-specific execution; all earlier native and ASan entries above remain historical evidence. New acceptance keeps mixed Track/Master order, retained processor identity across reorder/neighbor changes, chain bypass, and legacy migration distinct from native vendor controls. OldSkoolVerb custom input remains open unless an actual native change is confirmed through PCM/state/history/recall. VST2, AUv3, physical surround, listening and receiving-app persistence are not inferred.

- ✓ Final ordered UI `d9f62b2`: stopped drag + Option-Down, playing drag with position/service retained, independent chain/slot bypass, open-editor Save and fresh application recall/render.
- ✓ Native minimum 1040×680 with Browser and Inspector, keyboard-focus outline and scrolling Master/output access.
- ✓ LV50A 1.0.0 native/Undo/Redo PCM delta 6 dB; byte-identical finalized fresh-process renders.
- ✓ Youlean 2.5.15 Track/Stereo Master/exact Rear front-pair high/Reset/low/silence; nine selected decoded-source PCM windows exact.
- Open: OldSkoolVerb 2.14 actual custom input. Final correct-window Dry drag and Dry Mute click remain unconfirmed; exact physical handoff in report. No scale-policy cause inferred.
- ✓ Required final Release gate 31/31 /264.24 s; original r3801 user session restored in place with controls/media preserved. Evidence and limitations in ordered-inserts report.
