# SoundPlus v1.4 — tasks in progress

Starting HEAD `3609486` matched the supplied review; clean checkout. The attached user request defines scope; review/prompt/audit documents are reference material. Latest actual working session: Rain glass design r9731, protected before rebuild with catalog backup, recovery/settings copies and media/render hashes. Never restore an older snapshot over newer work.

- ✓ R10 symmetric active/pending Live/Take context restoration; controlled Undo/Redo and actual helper stream ownership; required first commit `4431786`.
- ✓ Versioned source/layout/routing/export contracts; deliberate legacy conversion; mandatory historical canonical hash and original WAV parity.
- ✓ Full 1–16-channel graph/stock DSP/FIFO/resampling/metering; independent monitor mapping/downmix; source admission 1–64 with explicit wide subsets.
- ✓ Role beds, source matrix, Spatial Panner/LFE policy, bounded typed spatial capture/replay.
- ✓ Interleaved/split WAV, actual metadata/order, transactional publication, durable output-set receipts/selection/Print identity.
- ✓ Compact Design/Monitor/Export UI, source badges, labelled per-channel details through shared bounded waveform scheduling; normal/minimum native workflows and 16-channel meters.
- ✓ Existing 20 + new multichannel suite, generated identity/anti-phase fixtures, all 21 suites passed after the startup migration correction.
- ✓ IPC backpressure diagnosed in a new failed pressure run; partial-write/Unicode/timeout tests and focused fix `25f6e98`.
- ✓ Native restoration exposed a schema migration race; backward-compatible read-only projection, controlled schema-4/5 startup recovery and native copied-old-catalog recovery passed; correction `2951a00`.
- ✓ Refreshed final-artifact 60s capture /45s replay measurements, PCM audit, native normal/minimum screenshots and report. Actual latest r9731 session reopened with AI enabled; document/settings bytes, catalog IDs and protected media/render hashes unchanged.
- ✓ Final evidence/report committed locally; working tree verified clean. No push.

Receiving-app gates: Resolve 21.1.2.10 API import, 5.1 track placement, save, SoundPlus exit and receiving project reopen passed for original/full/primary-layer selection. **Native cross-application drag remains blocked/unverified**: native start/end observed, but automation did not establish a confirmed Resolve-targeted drop. Finder/internal drops are not qualification. Physical mapped surround, physical unplug and human listening remain unverified. Historical unexplained restart/zero-peak observations are not retroactively closed.

Final full run: 21/21 passed in 84.22 s. Test-only portability correction `b4fac3d` regenerates/fingerprint-checks historical inputs and explicitly relinks the test copy without rewriting canonical recipe/event hashes. Final pressure capture retained 2316 events; 0 underruns/restarts, 22792/22798 nonzero status observations (6 initial/below-threshold observations qualified in the report). Replay: 12790/12790 nonzero, 0 underruns/restarts, 180 metadata edits.
