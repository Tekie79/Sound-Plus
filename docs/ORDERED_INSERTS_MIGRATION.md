# Ordered insert persistence and processing contract

Stage A checkpoint: `5b81430`, required full gate 30/30, 250.93 s. Stage B source is the subsequent ordered-insert checkpoint identified in the milestone report.

## One owner chain

Each Track and Master owns `InsertChain`. Its typed `order` references are the sole signal-order authority. Stock and hosted payload tables store each slot's settings once and are looked up by stable kind/ID, never by table position for processing. UI reorder changes only `order`; no independent stock/plugin order is editable in the product. Transient legacy/programmatic builders append payloads through one normalization boundary, which assigns missing initial positions and removes explicitly deleted references. Saved canonical chains contain the complete ordered slots inline.

Manifest **9**, `ordered-inserts-v1`, stores `insertChain` with version 1, chain bypass, hosted service-rack identity when needed, and an ordered `slots` array. Each record has an explicit `stock`/`hosted` kind, stable ID, per-slot bypass and settings. Stock settings retain type, parameters, slot lock and parameter locks. Hosted settings retain the coherent opaque state, program, identities, parameters and baseline. Runtime endpoints, clocks and failures are not serialized.

A canonical chain is not saved alongside old `effects`/`masterEffects`/`hostedRack` order fields. Conflicting nonempty representations are rejected. Unknown kinds, duplicate typed references, missing payloads and unsupported bounds fail explicitly. Limits preserve all valid legacy capacity: **eight stock plus eight hosted slots per owner**, with existing whole-mix hosted state/service bounds unchanged. No valid legacy slots are truncated.

Versions 1–8 remain readable. Their existing stock order is followed by their existing hosted order, with IDs, values, flags, locks, mappings, sources, variations and outputs retained. Saving an insert-containing live document writes manifest 9. Documents without insert features retain their existing format selection.

A legacy external-group bypass is retained independently as `legacyHostedBypass`; migration does not flatten or rewrite the slots' original flags. Affected rows identify the group bypass and Effect actions can enable that legacy group. New whole-chain controls operate solely on the new chain bypass flag. Ordinary new workflows do not create the legacy group state.

## Frozen takes

Historical frozen take recipes keep their original legacy insert encoding and checksum. They are not silently rehashed to the new live-document layout. Existing stock-only take behavior remains supported. A new stock recipe that needs ordered/bypass semantics has explicit `recipeVersion: 2`; its full ordered chain is part of its checksum. That manifest-9 checkpoint rejected hosted take capture. The subsequent plugin-aware milestone adds explicit manifest `10 / plugin-takes-v1`, recipe version 3 and `plugin-ordered-takes-v1`; see [the current take contract](PERFORMANCE_TAKES_CONTRACT.md). Stock-only take hashes and processing remain unchanged. Expanded Perform beyond four assignments remains a later milestone.

## DSP order and structural changes

The same prepared `OrderedRack` drives audition, Accurate audition, finite render and selected-stem processing. For layout-aware Tracks it remains after source edits/fades/gain and before spatial/matrix routing. For legacy stereo it retains the existing gain/pan position. Master remains after summation and before master gain. No insert moves across a routing stage.

Each hosted slot still owns its retained isolated native/audible instance. Selection and neighboring slot edits do not retarget it. New copies/duplicates receive independent IDs/services; a move to another owner retains slot identity but rebinds its service under the new owner. Reorder retains slot/service identities. All chain preparation, state work, allocation and file/JSON/DB I/O remain off the device callback.

Structural reorder uses the existing adoption policy: retire the old PCM borrower, reset the retained DSP for the new graph, and rebuild from the bounded live-seek pre-roll (normally 250 ms). This may produce a brief preparation transition; it is not a promise of seamless state-history continuity across a topology change. Accurate audition and offline render rebuild from frame zero for judging long tails. Opening/closing a native view alone does not reset DSP.

## Nondestructive rack bypass

Track and Master each have a separate chain bypass state. Toggling it never rewrites slot bypass flags, locks, settings or order. The prepared dry ring uses the **sum of hosted slot latency**, including per-slot bypass, so compensation remains constant. A live bypass toggle crossfades wet/dry over **10 ms** (480 frames at 48 kHz). Initial static bypass starts directly on the aligned dry path; settled full-wet samples bypass extra interpolation arithmetic to preserve historical PCM bits.

Active wet processors continue processing during whole-chain bypass, including source-end zeros, so their tails evolve. Individual slots retain their existing bypass/history policy. Unbypassing reveals the current evolving wet history, not a frozen earlier tail. Output tail caps remain unchanged. Missing, crashing or incompatible plugins still fail explicitly even if the whole chain is bypassed; bypass is not a qualification workaround.

## UI and ownership

Track Inserts and Master Inserts remain distinct inspector sections. Rows display actual mixed order, with per-slot enable, plugin editor access, drag reorder and Option-Up/Down keyboard reorder. Return opens the selected slot's controls/editor. Add effect remains explicit; advanced parameters expand on demand. The section's chain enable control owns only whole-chain bypass. Existing Lucide icons, theme, compact waveforms, fades and Library behavior are preserved.

Copy/move commands share one context-bound coherent capture boundary. They resolve the originally requested owner/slot, reject changed or read-only take contexts, preserve state/locks, and create a new slot ID for copies. A moved stock macro target follows its new owner. Source media and finalized output files are never rewritten by these commands.
