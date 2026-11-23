# Four-pad Perform — tasks in progress

Follows hosted-automation stage C at d4d113d. Original r4498 remains protected and closed.

✓ Versioned value-owned Pad A–D state, one shared graph/clock, stable targets in the existing Master automation lane; preserve legacy single-pad encodings/PCM.
✓ Working tabs/add, independent source assignments/macros/level/mute/solo; tab selection only edits, never solos or restarts transport. Preserve responsive bounded XY/sidebar sizing.
✓ Non-destructive selected-pad recording while other pad curves replay; retained immutable takes plus editable independent curves, Undo/Redo and Save/Reopen.
✓ Combined and per-pad durable rendering; independent PCM/partition/transport tests, actual native CoreAudio recording A then B and Render Selection comparison.
✓ Final native UI retest: Command-S/Space from pad tabs; recorded Read off/on preserves points; Pad B/D waveforms ready; Master lane navigation; desktop and1040×680 screenshots.
◉ Full unchanged Release gate.
○ Final reports, protected original audit/reopen, commit and push.

Bounded v1 policy: at most four pads; audio layer ownership is exclusive across explicit pad sources/mappings. Pad A retains previously unassigned audio layers. MIDI tracks remain on the shared timeline, outside four-source audio slots. Per-pad stems contain that pad's sources through the existing common routing/Master; shared MIDI is excluded from pad stems. Combined playback preserves shared MIDI. No extra automation lane is introduced.

Native retest found and corrected Command-S/Space propagation from pad tabs, a missing Read-menu handler, and waveform worker demand incorrectly remaining on Pad A. Each has an actual command/selection regression; pointer inspection now passed; full gate pending. Hosted vendor scheduling reference passes six installed mono/stereo cases.
