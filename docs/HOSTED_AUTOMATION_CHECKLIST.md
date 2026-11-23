# Hosted VST3/AUv2 Parameter Automation — tasks in progress

Start/reviewed HEAD `3da199dfe3e48db19e08daec88c420962ccfb8bc`, clean and equal origin/main. Complete attached review and current native/advanced effects, MIDI, multitrack automation, hosting and take contracts read. Latest observed original workspace: Workflow demonstration revision4498 (new microphone recording), `build/gain-hover-native`; protect this newer state, never restore revision4478.

✓ A Protected r4498, full stable binding identity, bounded sequence/clock/offset event scheduling and Read/base precedence verified. Foundation commit2914805 pushed; Touch precedence completes in C.

Stage A checkpoint: SPR5 bounded timestamped event transport, lease sequence/offset/native-ID and exact-instance token validation, message-thread sub-block processing with rebased MIDI and retained history implemented. Owned VST3 independent normalized/step PCM oracle passes 15 rate/partition combinations; stateful delay and duplicate-named instance isolation pass. Final targeted gate4/4 (47.90s; see committed log); prior hosting/AUv2 regression5/5 (109.58s). At that checkpoint registry remained read-only; Stage B below enables it only with validated scheduling. Original r4498 remains protected/closed.

✓ B Hosted Read for Clip/Track/Group/Master and MIDI Instruments implemented and tested with owned VST3 and process-local AUv2 fixtures. All15 rate/block combinations, hidden Hold/continuous curves, exact boundaries, program/base overlays, original stable IDs, split/seek, Side/Rear channel roles and explicit rejection of unavailable bus formats. Gate10/10 in149.84s; final expanded hosted gate4/4 in82.31s. VST3 owned Discrete16 is rejected without conversion; AUv2 admits and verifies all16 distinct channels.

◉ C Actual SoundPlus controls and qualified native VST3/AUv2 gesture Touch capture, target fixed at gesture start, Read return, stop/focus/window/crash/late events, one Undo pass; searchable instance-aware selector; exactly one collapsed Volume-first lane on each processable owner. Latch/Write only if independently qualified; no inert modes. Commit/push.

◉ D Manifest17/recipe8 activated only by nonempty bound hosted curves. Full base state/identities/curves/routing/source provenance, retained-take hash round trips, consumed pre-monitor PCM equality at original and converted rates, live-mix mutation isolation and finalized render receipts verified in B. Remaining transport/failure/device checks complete with C/E. Historical suites remain green.

○ E Owned VST3/AUv2 effect+Instrument oracles and installed vendor qualification, actual native editors/control capture/Undo/Save/Reopen, minimum1040×680 and desktop with both sidebars, resource/traffic/device measurements, final full unchanged Release gate/hash identities, original-workspace audit/reopen, contract/report/closure/roadmap and commit/push.

○ Follow-on after hosted qualification: four-tab Pad A–D Perform with independent four-source XY/macros/level/mute/solo, one shared clock, non-destructive overdub of B while A replays, combined/per-pad render and old single-pad compatibility. Preserve milestone boundaries and separate its verification/commits.

Human listening, physical multichannel/controller/DAC loopback, receiving-app import/place/save/quit/reopen and signed/notarized distribution remain separate. Native drag initiation is not receiver acceptance. Use isolated QA and generated fixtures; keep all user media, IRs, journals and finalized outputs in place. Device callback cannot load/process plugins, allocate, parse JSON, wait on services or perform I/O.
