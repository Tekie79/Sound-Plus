# R25 — saved completed-pass recovery

Starting revision: `53f4fab`. Source inspection confirmed saved-job Open trusted the normalized presentation hash and did not establish raw authority. It also restored an unchecked delivery receipt.

Open now uses the same worker-side `verifiedFallback` policy as operation recovery: raw is verified first, an intact normalized representation is retained, and a missing/corrupt derivative recovers the same pass as raw. Requested normalization stays separate. A missing/corrupt raw pass retains input/settings and reports an authority reason without Ready. No plugin is invoked by recall, fallback, normalization retry or copying. Hashing remains off the UI/audio callback. Existing epoch/context gates reject cancellation and stale Open completions.

Saved delivery readiness is deliberately cleared on every Open. Historical files remain untouched. Create Copy performs fresh verification and atomic publication before index/Print/drag become enabled; no saved receipt is implicitly trusted.

## Verification

Production plugin suite: **Passed, 38.05 seconds**, [log](verification/r25/targeted.log). New tests actually process a nondeterministic fixture and normalize/save it before reopening through fresh sessions with a nonexistent helper, across Mono, Stereo, 5.1 Side and 5.1 Rear. All six intact/missing/corrupt combinations, retained generation/input/frames/rate/roles/raw hash, same-pass normalization retry and split copying pass without an additional worker request. Tests make the historical delivery stale and confirm it is never adopted. Cancellation and rapid completed-A → prepared-B Open do not revive A. Existing legacy unnormalized and prepared-only binding checks pass. The production panel also Opens a missing-derivative job, exposes Raw with normalization requested, auditions/stops, retries normalization and finalizes a copy.

This is agent-run production component/session verification, not human listening or native receiving-app qualification. Native inspection and the full regression gate will be recorded with the subsequent feature build. [Source/app identities](verification/r25/identity.json) bind this separate recovery build. Only owned generated fixtures were damaged for these tests.
