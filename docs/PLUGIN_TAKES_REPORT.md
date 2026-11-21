# Plugin-aware recorded Perform takes v1

Request: attachment 0747e22a-0c25-410e-b5d3-a35f8f64fda0. Starting HEAD **a502a69**, exactly the reviewed commit; ordered implementation **d9f62b2**. No newer commits or unrelated changes were present. Stage A must pass and be committed before Stage B implementation.

## Stage A correction checkpoint

| Finding | Correction and regression |
|---|---|
| Valid v7 pooled stock designs rejected | Historical DSP tag is validated from historical processing semantics, independently of the current writer's format promotion. Canonical insert metadata and forged/incompatible tags cannot masquerade as v7. Genuine fixtures were saved/reloaded/rendered using unmodified core at **5b81430** in an isolated checkout; Track, Master and variation-only stock cases include a selected alternate, routing/fades/colour/locks/bypass and historical frozen takes. Model state, baseline static/take PCM, frozen hashes, recovery loading and save/reopen as v9 are tested. |
| Seeded variations changed after round trip | Draws follow canonical typed references, resolving stock payloads by stable ID. Reordered Gain/Filter/Delay, fixed seed, effect/parameter locks and macro-owned parameters compare by ID. A baseline-generated historical RNG oracle verifies unchanged canonical order retains old behavior. |
| Reorder destroyed keyboard focus | Ordered rows are reused by stable InsertRef, with indices/bounds refreshed in place. The focused row scrolls into its own viewport; refresh never grabs external focus. Actual peer-delivery tests exercise consecutive Down/Down/Up/Return, 16 rows, focused subcontrols and background refresh; production Designer commands exercise stopped/playing and Undo/Redo using peer dispatch rather than direct row calls. Native evidence follows. |

Baseline fixture generator/source/producer hashes and original emitted manifests/PCM are retained in [legacy-v7](../tests/fixtures/legacy-v7/README.md). The current writer was not used to invent these v7 fixtures. Relocation uses refreshed live source stamps and explicit identical-content take overrides; historical recipes/hashes are unchanged.

Targeted correction pass: **2/2 /20.80 s** after related source-pool/take/performance/view suites passed. The first new Return assertion assumed the controls were still collapsed on its second invocation. Test setup now closes them before the no-extra-click keyboard sequence and still requires Return to open the correct slot. Raw failed evidence is retained. Full gate and native results are recorded below once executed.

## Preserved boundaries and open requirements

The user's current **Workflow demonstration r3801**, unsaved Perform state (Blend .608/.760, Macro 1 .491, monitor .25), catalog, sources and durable outputs were protected before normal Quit. This is a new protection snapshot, not the older milestone backup. The pre-quit UI also reported an already-restarted audio helper; this observation is not attributed to the new implementation. The original catalog remains in place throughout isolated QA.

OldSkoolVerb native custom input remains unconfirmed until an accepted native gesture changes state/PCM. VST2 dependency/adapter and AUv3 runtime qualification remain open. Expanded Perform groups/additional pads and Resolve product integration remain separate milestones. Human listening, physical surround, numerical PCM, native execution and receiving-project persistence are reported separately.

Stage A required full gate: **32/32 /268.55 s**, Release arm64 / ASan OFF, actual vendor/AUv2/device branches enabled. No processing/UI source changed afterward. Native Track and Master stopped/playing **Down, Down, Up, Return** keep the same slot with no intermediate clicks. Native Undo/Redo retains focus; playing Track Return opens its same hosted editor. [Gate](verification/plugin-takes/stage-a/full-gate.log), [source/helper identity](verification/plugin-takes/stage-a/identity.json), [stopped focus](verification/plugin-takes/stage-a/track-stopped-focus.png), [playing Master](verification/plugin-takes/stage-a/master-playing-focus.png), [same-slot editor](verification/plugin-takes/stage-a/track-playing-editor.png).

Full sixteen-row reachability, viewport scrolling, focused subcontrols and no background focus stealing are actual peer-delivery automated checks. Native checks use three-slot owner chains. Edge-drag auto-scroll and insertion-line feedback are **not implemented**; drag reorder within the visible list and keyboard navigation through all rows remain usable. No broader redesign is included.
