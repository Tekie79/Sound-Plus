# Tasks in progress — Plugin Correctness and Discovery Recovery

Starting HEAD: `ac6a4805a74d35e62614b356c121fb77e0d86b61`. No later checkout changes or user working-tree edits at start. Preserve R26–R30 and minimal names, adding restrained disambiguation only when needed.

- ✓ Read the complete attached request, applicable guidance and four required reports/contracts.
- ✓ Reproduce TAL with exact installed identity; trace restoration/controller/queue/process/PCM; compare an independent host where available.
- ✓ Restore full Rescan discovery; keep Retry selected targeted.
- ✓ Refresh selected AU registration/bundle/architecture/fingerprint; retain atomic publication and unrelated entries/roots.
- ✓ Preserve compound class selection across cached/fresh/search changes; no substitution when a selected class disappears.
- ✓ Clear obsolete current-failure fields after successful recovery; retain history separately.
- ✓ Add high-index/large-schema stable-ID, legacy-ID, program/state and same-count reordered-schema regressions.
- ✓ Preserve separate owner panels, meaningful unavailable/bypass states and compact real waveforms at minimum size.
- ✓ Rerun optional MIDI/auxiliary, exact layouts/LFE, four vendor control cases and eight vendor/layout/owner renders with explicit qualification environment.
- ✓ Native Rescan/Retry/multi-class/ownership/bypass/recall/export checks; full gate, portable audits and exact final identities.
- ✓ Closure report complete; latest user state restored in place; local commit complete (see git log).

TAL remains an open acceptance item until an audio oracle passes. VST2 dependency/adapter and AUv3 runtime qualification remain open requirements. Separate-instance editors apply on close; same-instance editing, meter qualification, interleaving, hosted takes and Expanded Perform are outside this milestone.

Diagnosis: read-only inspection of the installed TAL binary confirms 2,092 declared parameters and ten duplicate native IDs 48–57 shared by audio controls and MIDI aliases. Its process dispatcher routes that range to MIDI before audio lookup. General duplicate-native-ID rejection implemented; TAL audio acceptance remains open. Initial focused suite passed 39 assertions; expanded production chooser/real multi-class and widget regressions are in verification.

Final evidence: 27/27 CTest suites passed in 185.48 s with SOUNDPLUS_VENDOR_QUALIFICATION explicitly set to build/plugin-correctness/vendors-delivery. Focused suite: 71 assertions. Vendor records contain four persistent control cases and eight owner/layout renders. Portable audit: 505 checks, zero PCM error. Native Rescan/Retry/class selection/insertion/owners/bypass/save/quit/reopen/render and minimum/larger layouts/waveforms verified. Native final WAV: Stereo 48k, 24bit, 96,000 frames; max raw quantization error 5.96046448e-8. Human listening, physical surround and receiving-app acceptance not tested. TAL remains an external open audio acceptance item; its ambiguous native IDs now fail closed. User Workflow demonstration r34 restored with latest unsaved edits, playhead 2.112, monitor .25; 191/191 protected files unchanged.
