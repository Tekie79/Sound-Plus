# SoundPlus v1.4.1 — tasks in progress

Starting HEAD `6364dfd` exactly matches the reviewed artifact; no later application changes or pre-existing working changes. The attached pasted user request defines implementation scope; the ZIP review, correction brief and evidence audit are references. Preserve R5–R10 and the current shared architecture/UI.

- ✓ Read AGENTS, complete attached request/review/correction brief, v1.4 contract/report and applicable architecture requirements; inspect HEAD/source tree.
- ✓ Protected actual r9731 working session/catalog/settings and six source/render hashes; app/helpers closed. Isolated correction suite added.
- ✓ R11 implementation and focused automated checks:23 lifetime IDs /16 current takes; Live+take preferences; Delete/Undo/Redo/save/autosave/reopen; bounded dangling metadata repair; injected write failure. Native QA found an additional temporary recording-graph projection bug with valid retained preferences; temporary graph now clears only its removed-context preferences; actual native session/helper replacement capture, render and 17-preference save/reopen regression passed.
- ✓ R12 implementation/focused verification: routing v2 / stock-layout-takes-v2 and IPC7; archived v1 WAV/hash behavior preserved; corrected-copy lineage/receipt; independent mono/stereo/divergence/revolution oracles; real graph adoption; controlled and native capture/replay/render; startup migration/stale completion guards. Six relevant suites passed. Native UI workflow remains in QA gate.
- ✓ R13 implementation and focused production-control/PCM checks: selected route binding; precise one-field edits; implicit/absent no-op; preserved drafts; layer/document stale-context guards; Undo/Redo/reopen. Native workflow remains in QA gate.
- ✓ R14 implementation/focused actual-service tests: revision-aware replacement override invalidation through actual metadata/indexing/Library/Add paths.
- ✓ R15 implementation/focused production-callback, native-helper adoption and independent PCM tests: actual-owner audition specification across reference/candidate mode changes, revisions, pending and stopped states.
- ✓ R16 implementation/focused verification: transactional legacy pair/analysis conversion, actual input indices/PCM, persistence and immutable takes.
- ✓ R17 implementation/focused automated checks: mandatory historical fixture, actual render-panel callbacks, stereo/mono split and interleaved, exact legacy WAV hash, membership/selection/save/reopen/Print/Undo/Redo/missing stem. Native workflow remains in QA gate.
- ◉ Native normal/minimum workflows, focused indexing/waveform pressure, all21 existing suites plus correction coverage, fresh source/artifact/PCM/UI evidence.
- ○ Closure report, corrected Source Pools v1.5 brief, actual latest user design reopened, coherent local commits and clean tree; no push.

Compatibility decision before R12 DSP edits: preserve existing `stock-layout-takes-v1` recipe/receipt processing. New live/new-take processing uses an explicit corrected version. A deliberate corrected copy retains the original take and its outputs. Legacy stereo remains unchanged. Implement exact version identifiers and document admission/migration when inspected.

Open qualification gates remain separate: native SoundPlus→Resolve drag, physical surround mapping/unplug, human listening. Prior API persistence and six untimestamped low-peak observations are historical baseline evidence, not patch verification or a diagnosis.
