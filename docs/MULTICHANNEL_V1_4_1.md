# Multichannel correctness and persistence v1.4.1

This patch closes R11–R17 before Source Pools v1.5. Preserve the current Library/Designer workflows and the Lucide/theme system. The full verification/closure report will identify actual source/binary revisions, fresh tests, native actions and remaining qualification gates.

## Processing compatibility decision (before the DSP repair)

- Frozen `stock-takes-v1` stereo recipes remain unchanged.
- Frozen `stock-layout-takes-v1` recipes retain routing version1 and the original panner equation, including its affected negative-wrap PCM. Existing recipe/event hashes and output receipts are not rewritten.
- New editable layout-aware mixes and takes use routing version2 / `stock-layout-takes-v2`. The panner normalizes arbitrary finite accumulated angles periodically; shortest-path 10ms smoothing and speaker geometry remain.
- On user-document adoption, editable layout-aware version1 Live/variation mixes migrate to version2 with a new document revision and retained dirty recovery. This never mutates their frozen takes. Empty-layout legacy Live mixes stay legacy until deliberate layout conversion.
- An explicit corrected-copy action for an old spatial take creates a new ID/processing recipe and provenance linking the original ID/hashes. It retains the original take, motion events and durable outputs. The corrected copy has no falsely inherited Ready output.
- Document version6 (`stock-layout-v2` envelope) gates corrected routing and independent split-output metadata. Recipe serialization dispatch remains version-specific. Output-set metadata is independent of whether the rendered processing is legacy.

A mandatory pre-repair fixture and source/render reference live at `verification/multichannel-v141-spatial-v1*`. It records the reachable 11-target negative-wrap sequence, with 1200 frames between applied targets. Its final512-frame multichannel window is silent under version1. Corrected tests must compare version2 against an independent channel/energy oracle, not just two paths sharing an equation.

## Persistence corrections

Deleted take preferences are removed in the same Undo transaction; receipts/files remain durable. Validation accepts only `live` or current take IDs with at most current-takes+1 preferences. Loading an affected bounded old document prunes dangling preference metadata only; malformed or excessive metadata, payload checksums and 64MiB document bounds remain guarded.

Legacy split rendering persists the split flag, versioned output-set membership and selected finalized-output identity without injecting layout fields into an old frozen recipe. Interleaved Print master and role-named stems retain their actual paths/checksums. A missing member makes an explicitly selected set unavailable rather than substituting another output. Old output receipts/files remain intact.

## Qualification boundaries

Baseline 21-suite logs, PCM and latency records are historical evidence only. The patch requires fresh focused and complete-suite results, native normal/minimum workflows and source/artifact identities. Native receiver drag is separate from Resolve API import/persistence. Physical surround mapping/unplug and human listening remain separate open qualifications. The baseline's six untimestamped below-threshold observations are not diagnosed by this panner correction.
