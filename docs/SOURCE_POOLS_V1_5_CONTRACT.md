# Source Pools v1.5 — compatibility and ownership contract

Written before changing document serialization. Starting source is 6c3e18a; its Library loops, threshold search and conditional range selection are retained.

## Optional metadata; existing engine

A layer still resolves to the existing source, original-frame region, physical selection and routing consumed by DesignGraph. A pool is optional, at most 64 entries including the initial source. Pool/entry/source UUIDs are identities; paths, names and indexes are not. Different ranges of one source are distinct entries. Creating a pool snapshots the current source as its first entry. No catalog enumeration or copying of thousands of assets enters the document.

Entry owns source ID/revision/stamps/fingerprint, source rate/count, ordered detected layout/confidence, explicit interpretation, original-frame range, physical inputs/pair/qualified arithmetic mean and reviewed mapping with its destination bus. Layer owns placement, linked playback rate, reverse, gain/pan, fades, effects and existing locks. The complete reviewed routing belongs to the entry, including its spatial mode/position/width/divergence and LFE policy; selecting an entry recalls those routing settings. Layer pan remains layer-owned. Selection copies only entry-owned fields into the ordinary resolved layer. A default filename label follows the source; a custom layer alias remains. Sync retains its elapsed source time relative to the old range start, converted to the new source rate and clamped into the new range. Existing legal fades are preserved, including overlaps on a shorter source; existing envelope/DSP behavior applies. These adjustments form one Undo transaction.

Document manifest 7 adds `sourcePoolMetadataVersion: 1` while retaining the appropriate existing `dsp` tag. Versions 1–6, including manifest 4/5/6 and empty-layout legacy, retain their dispatch. Absent pool metadata inserts no fields into legacy mix/recipe serialization and changes no canonical hashes. Pool metadata is independent of routing/DSP version. The three frozen take processing identities remain unchanged.

## Validation, availability and concurrency

Add and activation inspect/hash on the session worker and verify stamps before/after. Fingerprints belong to the accepted revision; changed bytes require deliberate replacement, never a filename match. Unavailable inactive alternatives do not block a valid active source. Activation is atomic: incompatible/unreviewed/offline/changed entries leave the prior selected entry, display and source unchanged and report the error. A selected source later becoming unavailable stays selected and visibly unavailable; it never silently falls back.

Each alternate has its own physical selection and mapping review for the current bus. Known ordered layouts can propose a mapping, but the user confirms it before first activation. Unknown six-channel sources require explicit ordered interpretation (Side, Rear or discrete); channel count is not evidence of surround order. Wider-than16 sources require an explicit <=16 physical subset or routing-v2 all-channel arithmetic Mono mean; DSP buses remain <=16. Bus changes invalidate inactive entry reviews for another destination. Active source edits synchronize only the selected entry. Capture readiness validates the active mapping and fingerprint.

Normal Live activation stops the owned Designer audition and leaves it stopped. Play is an explicit restart with a freshly prepared graph; this is not a hot swap or source trigger. It never starts a Library/reference audition. Capture Arming/Recording/Finalizing and immutable take context reject source/pool mutation. Worker completions bind document lifetime, mix/variation, layer, pool/entry identity, source revision and a per-layer request generation. A→B→C keeps only C's activation; independent accepted Add operations may each complete once. Context changes/Undo invalidate obsolete work.

Remove of an active entry requires an explicit replacement choice; removing the last entry is rejected. Reorder and enable/disable preserve entry IDs. The active selected snapshot must agree with its entry; mutations and Undo/Redo/save/reopen validate this invariant.

## Determinism and independent lock

Source choice is opt-in through the variation Source parameter. Explicit selection and Record do not implicitly reroll. Before generating each variation, `pool-fnv1a64-smix-v1` hashes UTF-8 fields each followed by NUL (including the last): policy, unsigned decimal seed, layer ID, pool ID, unsigned decimal zero-based variation ordinal. FNV-1a uses offset 14695981039346656037, prime 1099511628211 and unsigned 64-bit wrap. The unsigned SplitMix64 finalizer follows: xor >>30, multiply 0xbf58476d1ce4e5b9, xor >>27, multiply 0x94d049bb133111eb, xor >>31. Enabled entry IDs are sorted lexicographically; finalized hash modulo candidate count selects an entry. Availability is not a candidate filter: a missing deterministic choice fails explicitly rather than rerolling. Fixed externally computed expected vectors qualify this version.

Resolved policy, seed, ordinal and entry ID persist with the variation/pool. Playback and export use that result without another draw. Record verifies and freezes the currently resolved explicit/variation choice before Arming completes. Source-choice lock is independent of gain/pan/rate/etc.; existing whole-layer lock also prevents source variation. Pool edits affect future generation only, never a resolved variation or take.

## Frozen takes and delivery

Freeze strips mutable pools from the recipe and retains one immutable, versioned origin record per pooled layer: pool/entry/layer/source IDs, revision/fingerprint, exact source range/rate, interpretation/physical mapping and choice policy/seed/ordinal. Origin metadata is saved outside the old canonical processing recipe/event identity under the document metadata gate. Recipe/event hashing keeps its existing meaning and dispatch; no old take is relabeled. Receipts may carry the origin record alongside existing verified source provenance. Corrected-copy lineage, current-context output preferences, split-member roles/paths/hashes and explicit output-set identity remain unchanged.

Relinking a pool entry requires identical verified content; it updates only that entry's location/stamps and the active resolved layer when applicable. Take relinking remains the existing separate content-verified override. Later pool edits, bus edits, catalog updates or entry deletion cannot mutate frozen take recipes, origin records, events or durable receipts. Inactive pools are not scanned by take render/replay. Source triggers/overlapping voices are deferred to a separate typed-event contract.

## Preview and UI bounds

Alternatives is a compact contextual disclosure in the existing source inspector, available from Edit/Perform without replacing the four Blend cards. Rows show actual source-range/reverse overviews via WaveformPool/shared compact painting, selection, duration, layout and distinct loading/silent/offline/changed/decode-error states. Only visible/selected rows request work; reuse existing 48 full/160 summary/96 geometry cache limits and bounded worker admission. Painting performs no decoding, hashing, filesystem/database work or SVG parsing. Selected preview bindings include document/context/layer/entry/revision and reject obsolete results.

Physical surround listening, actual external-drive removal and native Resolve receiving-project persistence remain separate qualifications; controlled offline/relocation, PCM or drag initiation cannot stand in for them.
