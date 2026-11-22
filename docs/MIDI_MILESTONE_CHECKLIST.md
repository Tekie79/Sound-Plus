# MIDI sequencing and instruments — tasks in progress

Reviewed/starting HEAD: 949dbc5619c9921cd52f5588a5b91a94d9523884. No newer commits at start; working tree clean. Preserve current catalog, media, all old recording journals/prefixes, and finalized outputs. No push.

✓ A1 Monitor rejection/reconfiguration: fail closed, discard old buffered samples, reset meters at a safe boundary; active-recording/device regressions.

✓ A2 Incremental acknowledged-prefix integrity, versioned journals, explicit legacy policy, corruption/nonfinite/multi-owner/interrupted-finalization tests.

✓ Stage A relevant final gate: 7/7 in 34.37 s,195 input checks; source/build identity captured. Stage A code checkpoint committed before MIDI implementation.

✓ B Separate MIDI Tracks, stable typed clips/notes/controllers, tempo/tick/sample conversion, edit commands and persistence/historical compatibility.

✓ B Contextual piano roll and CoreMIDI recording/monitoring with timestamped bounded queues, shared transport and note-off safety.

✓ C Native polyphonic referenced-sample instrument, sustain/envelopes/pitch/loops/voice stealing and independent PCM oracle.

✓ C Dedicated isolated VST3/AUv2 instrument processing/state/editor, exact bus/MIDI packets/PDC; qualify an installed instrument if available.

✓ C One collapsed automation lane per track, native/hosted target registry, shared live/offline rendering, snapshots/frozen recipes and receipts.

✓ Final native1040×680/normal workflow, complete unchanged-build vendor/AU/device gate, preservation audit, report and checkpoint commits.

Keep simulated fixtures, actual CoreMIDI devices, installed instruments, human listening, physical surround and receiver drag evidence separate. VST2/AUv3/universal vendor compatibility are not qualified by this increment.

Current relevant gates: 8/8 in 38.07 s; subsequent correction gate5/5 in 32.17 s; installed-AU fresh-process persistence1/1 in 23.26 s. Real AU branch required when AUMIDISynth is registered. Native editor Create/transpose/copy/paste/Undo, real owned CoreMIDI recording126 packets, Stop/Undo/Redo/Save/quit/reopen, actual reopened output, VST3 GUI/Gain0.50 recall, installed AU GUI/Gain-6dB editing and minimum Browser+Inspector checks are executed. Final unchanged-build gate passed 37/37 in 345.23 s, all 296 source/resource and 8 binary hashes unchanged. Original r4478 reopened in place; payload/preferences,1 root/43 assets and 37 protected hashes match. No old backup restored. Report/evidence complete; final documentation commit follows. Physical keyboard, human listening, physical surround and receiver drag remain separate/open.
