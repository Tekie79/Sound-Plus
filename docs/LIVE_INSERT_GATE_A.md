# Live insert milestone — Gate A correctness checkpoint

Starting commit: `185ed9aac74633e336d24df46a3c40f164af61f9`, initially clean. The user's subsequently edited r36 catalog, new TAL AU insert and original media are protected in place; no older catalog is restored.

A1 uses per-canonical-registry thread/process coordination for snapshot capture and publication, with operation-versioned reconciliation across the unlocked background probes. Root lists merge with the latest configured roots at capture/publication. A completed newer candidate/recovery wins over an older cached or failing result. Cancellation never publishes the older partial snapshot. This is not merely a lock around the final write.

The controlled production test pauses an older Track operation after snapshot capture, publishes a Master add-root and recovery through the same registry's canonical alias, then resumes Track. It repeats with a cancellation/dismissal token and an actual older child crash. New roots, recovery and unrelated entries survive; cancellation preserves the newer published file byte-for-byte.

A2 publishes matching `state`, `stateHash`, `parameters`, `parameterBaseline` and program in the worker-produced `result.job`; the standalone result also exposes its matching baseline. The actual returned-job regression edits Gain .5→.25, processes, reuses that job and requests .5; PCM returns to the requested value.

Effective-control policy: a request equal to its saved baseline defers to the opaque capture. Explicit changed requests override it. A persistent instance retains the native values read immediately after opaque restoration, so .75 opaque/.5 request-baseline → .6 explicit edit → Undo/return to .5 restores effective .75, matching fresh restoration, finite processing and saved/reopened full-design render. No full opaque replay is required on every slider update. Legacy jobs without a baseline retain explicit normalized override semantics. This fixes a returned-Process-job/capture consistency issue; it does **not** establish corruption of ordinary saved Designer documents.

An owned VST3 delegate changes its advertised native parameter ID on the second `setupProcessing` and requests the normal SDK parameter-info refresh. Both finite and persistent worker paths reject it specifically at the post-prepare identity check, before Ready/output publication. Duplicate-native-ID rejection remains intact. No vendor bundle or pinned adapter source was changed.

Verification on native arm64 Mac:

- New `plugin_gate_a`: **53 assertions**, passed in **9.28s**.
- Existing `plugin_processing`: passed in **60.98s**, with its CTest `SOUNDPLUS_QUALIFY_AU=1` environment. Optional `SOUNDPLUS_VENDOR_QUALIFICATION` was **unset** for this intermediate Gate A run; no new vendor branch claim.
- Existing `plugin_correctness`: passed in **10.47s**.
- [Raw summary](verification/live-inserts/gate-a/summary.json), [focused log](verification/live-inserts/gate-a/focused.log), [existing regressions](verification/live-inserts/gate-a/existing-regressions.log), [source hashes](verification/live-inserts/gate-a/source-hashes.json).

Gate B remains in progress. These results do not qualify same-instance native editing, metering, human listening, physical surround or receiving-app acceptance.
