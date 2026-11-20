# Tasks in progress — Live Insert Editing, Metering and State Synchronization

Authoritative new milestone request: `/Users/harvest/.codex/attachments/b609d7ee-f2a4-467a-9424-d0cd009551be/Pasted text.txt`.
Starting HEAD: `185ed9aac74633e336d24df46a3c40f164af61f9`, initially clean. Early uncommitted separate-instance forwarding work was captured in `build/plugin-editor-live/provisional-bridge.patch` and removed from source because B1 explicitly requires the audible instance. It must NOT be used as the final live path.

- ✓ Read AGENTS.md, the complete new prompt and four referenced reports/contracts.
- ✓ Gate A1: coordinate per-canonical-registry writers; revisioned reconciliation with latest roots/recovery/entries. Deterministic older snapshot → newer publication → older resume test; cancellation/dismissal/failure.
- ✓ Gate A2: coherent worker result.job state/parameters/baseline; consistent defer-to-opaque effective controls through live return/undo/fresh/save/render; owned prepare-time schema mutation rejected in finite and persistent paths.
- ◉ Gate B1: native editor belongs to retained audible isolated processor; explicit processor generation; UI on required message thread; opening/closing does not reset DSP/tails.
- ○ Gate B2: one authoritative bidirectional native/inline/program/opaque/undo/reset/save/render flow, coherent open-editor save, sequences and gestures.
- ○ Gate B3: original incarnation/variation/owner/slot/generation lifetime, independent Track/Master windows, copies/moves/removals/reprepare/crash/unresponsive/shutdown; bounded recoverable checkpoints.
- ○ Gate B4: actual signal-driven Youlean meter checks on Track and Master, known levels/silence/steps/reset; exact identity; no inference from window/transparent PCM.
- ○ Auto-open/focus new insert editor; float while Designer is used. Hide failed/unsupported/incompatible plugins from normal selection; keep diagnostics retained separately.
- ○ User steering: icon-based Designer playback/navigation controls plus real persistent timeline markers; avoid a broad redesign.
- ○ Preserve role layouts, mono-before-surround, per-channel impulses/LFE/latency/output-authority; TAL VST3 duplicate-ID rejection and open VST2/AUv3 requirements.
- ○ Owned production regressions and actual LV50A/OldSkoolVerb native gestures during playback; save open/undo/reopen/no-reset; measured timestamped live response p50/p95 and raw workload records.
- ○ Final full gate with explicit vendor/AU branches; exact final code/helper/frontend identities; affected final native checks; separate source/PCM/native/meter/listening/physical-surround/receiver results.
- ○ Restore latest user state in place, report and local commit.

Protected current user session: Workflow demonstration **r36**, unsaved, playhead **6.0**, selected clip-005, monitor **.25**, newly added TAL-Reverb-3 **AUv2**. Backup/hash/catalog files under `build/plugin-editor-live/protected`. Original app closed through native UI before the provisional build. Current source has returned to HEAD except this checklist; the last built bundle contains a provisional bridge and MUST be rebuilt after Gate A before native delivery. Do not restore the older r34 catalog.

Gate A targeted run: `plugin_gate_a` passed 53 checks in 9.28s. Includes deterministic canonical-alias registry interleavings (older cache, cancellation/dismissal, older helper crash), returned-job baseline round-trip, opaque .75 → edit .6 → Undo/return → live/fresh/finite/save/reopen/export and legacy override, and actual owned VST3 native-ID change on the second setupProcessing (post-prepare-specific rejection on finite and persistent paths). Existing plugin-processing/correctness regressions are running before Gate B architecture work.

Gate A completed: focused 53 assertions (9.28s), existing plugin_processing (60.98s) and plugin_correctness (10.47s), both passed. Evidence under docs/verification/live-inserts/gate-a. No native/PCM/metering Gate B acceptance is inferred from this checkpoint.
