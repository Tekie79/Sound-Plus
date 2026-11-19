# Performance Mixer v1.3 prerequisite corrections

R3: `LibraryView::updateSelection(bool)` separates explicit user pinning/playback-follow from programmatic detail restoration. Actual Refresh and typed-query handlers run through the AI-enabled view with a controlled delayed semantic completion at the inference boundary; real catalog, reader, fusion and publication run normally. The retained ID and audio generation stay stable while ordering is adopted; later deliberate selection pins. Clicking an already selected cell also pins. Production uses the normal inference helper; the optional constructor test mode creates no model worker.

R4: four bounded demand surfaces retain at most40 entries each.48 full-resolution sources and96 geometries remain unchanged. Evicted demanded sources retain conservative all-channel display summaries of at most2048 peak pairs each (at most160 summaries /2.5MiB). A selected/card source promotes to full peaks. No immediate reload is needed solely because a still-visible source was evicted. Stamp-only validation does not reread a derivative when the source is unchanged; errors retry decode on recovery. Geometry is invalidated on validation adoption.

Oldest overdue validation receives a protected service admission after at most3 ordinary admissions, also when missing work remains. With N bounded demands, an overdue retained source has at most4N admissions ahead of it (plus the currently executing job). This is a scheduler service bound, not a wall-clock promise for blocked OS I/O. Abandonment still cancels; selected priority and existing background fairness remain.

The pressure regression now continues after all80 row/layer sources have completed once: unchanged demands settle,1000 further ticks add no work, a retained selected source goes Offline/reconnects,changes/restores,and a final validation sweep settles again. Full peaks/display summaries/geometry are independently bounded. Ordinary Perform remains at most45 demands; combined Edit is the relevant over-capacity product case.

The helper notice regression now waits for a different helper PID and a subsequent healthy idle heartbeat before asserting notice persistence. The historical intermittent restart cause remains unresolved.

[Executed evidence](verification/performance-v13-corrections.json):4 targeted suites pass in10.15s. Full feature regression follows in the recording milestone.
