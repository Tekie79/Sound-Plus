# Performance Mixer v1 live checklist

Prompt: attached `e81db7bf-e104-4327-869e-01fa67611130/Pasted text.txt`. Starting HEAD: `43f5776`. Newer user steering retains fixed corner fade controls and requests explicit direction-retention verification. Checklist convention is documented in [AGENTS.md](../AGENTS.md).

- ✓ Read the attached prompt, architecture/requirements and existing Designer reports; inspect the model, session, live parameters, DSP and render paths.
- ✓ Write [the architecture note](PERFORMANCE_ARCHITECTURE.md) before implementation.
- ✓ Implement shared effective-state resolution, stable mappings, compatibility and persistence; core and native PCM tests pass.
- ✓ Implement Edit/Perform workspace switching, blend pad, assignments and numeric/keyboard alternatives; native normal-size UI inspected; minimum-size checks pending below.
- ✓ Implement named macros and the contextual mapping editor, ownership and unresolved-target feedback; native normal-size UI inspected; minimum-size checks pending below.
- ✓ Save Snapshot, recall and undo/redo pass automated and native checks; named snapshot A/B, saved-document reopen, durable full/primary-layer renders and print-back/Undo verified.
- ✓ Verify fixed corner fade controls retain IN/OUT direction even while dragging across the opposite control; 1,449 canvas checks pass.
- ✓ Final regression: all 13 suites pass in 50.67 s, including effective-inspector and primary-layer print checks. Native performance PCM error is 0.0 before monitor gain.
- ✓ Final serial eight-layer workload completed separately from tests; same helper/graph/playback generation, exact switch recipe/playhead, zero underruns; latency and baseline comparison recorded.
- ✓ Final normal/minimum screenshots, fine keyboard steps, mapping feedback/Undo and both internal-drop paths captured/verified. Restored working revision 6351 unchanged; subsequent user edits retained.
- ✓ README, Performance Mixer v1 report, architecture note and structured evidence updated, including the unresolved native partial-repaint/capture ambiguity.
- ✓ Final source/evidence review complete; implementation, verification record and screenshots committed together.

Resolve physical native-drop qualification remains separate and open. No drag-code rewrite is planned for this slice.
