# Collections and MIDI correction — tasks in progress

Starting/reviewed HEAD: f3c5b89e6e77a6666233a695f27760b672054840. Working tree clean; no later commits at start. Read attached review/request and repository guidance. Preserve current in-place user catalog/session, media, embeddings, raw/journal prefixes, takes and durable outputs. No push.

✓ A Reproduce/fix interleaved MIDI continuation with two/three fragments, edits/separation, rates, session history/recall and native/hosted PCM.

✓ A Measure live MIDI acquisition→applied p50/p95/p99/max at44.1/48/96k, callback sizes/load; investigate bounded low-lookahead safely, retain clock/PDC/panic/integrity distinctions.

✓ A Relevant regression gate and independent Stage A commit before collection implementation.

◉ B Transactional Catalog v7 migration; stable nested collections/order/cycles/delete policy, many-to-many members, Favorites/Recent, undoable bulk metadata, unresolved/reviewed exact region references.

○ C Shared Library/Designer navigation and search/root/collection scope; exact stored-region audition/drag, offline/paused metadata and live synchronization without audio/embedding invalidation.

○ D Small/100k/500k scoped regression/performance, legacy/migration/integrity and historical hashes/PCM; native normal/minimum Browser+Inspector and preserved Perform.

○ D Complete unchanged-build Release vendor/AUv2/device gate, source/binary identities, current-workspace audit/reopen, report/evidence and commits.

Physical controller/loopback, listening, physical surround and actual receiver drag remain separate open qualification. Future pads/overdubs, new effects and external AI generation are outside this increment.
