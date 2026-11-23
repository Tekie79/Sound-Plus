# Library zoom and organization follow-up

Baseline `6d42aa8` on `ft-ui-redesign`.

| Work | Status |
| --- | --- |
| Diagnose blocky waveforms | Confirmed the zoom was enlarging whole-file peak bins. |
| Decode high-resolution viewport peaks | Verified the final trace on an 85 ms saved range. Tests checked exact sample positions, both channel polarities, and transients outside the range. |
| Organize Details and repair menu punctuation | Verified grouped audio fields and custom iXML tags. Native menu displays “New Collection…” correctly. |
| Simplify adding and nesting collections | Verified an existing destination and a new nested destination from a marked range. Confirmed parent-first ordering, correct Unicode names, and retention after reopening. Tested atomic create, undo and redo. |
| Preserve and reopen the working session | Reopened the original Library. The audit matched design revision 4506, all 18 catalog tables, and 425 media files. |

The final arm64 Release build passed 12 focused tests on macOS. Native UI checks used two generated WAVs in an isolated catalog.
