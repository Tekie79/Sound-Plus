# Performance Mixer v1.2 — tasks in progress

Starting HEAD: `97bf7c42f05fec73192193c0061ba174a7d96ab1`; clean checkout. Read the complete user request (attachment `7cc347b8-f6a7-43fd-9afe-fbc2dc0765ed`), supplied review/prompt, current architecture/report/evidence and AGENTS.md.

- ✓ Baseline and supplied documents inspected. R1/R2 remain; preserve completed v1.1 behavior.
- ✓ Protected latest working design r9054 (not older r8567); safe native quit. R1/R2 failing-before logs retained in build/performance-v12/failing-before.log.
- ✓ Request-aware pinning separated from page fetches; stable-ID restoration, explicit removal and selection-follow ownership checks pass actual view/service tests. New queries cannot be pinned by old rows. Later source review also protects annotations when a pin rejects an in-flight reorder.
- ✓ Cooperative cancellation, selected priority, identical-demand no-churn and background progress pass controlled worker tests. All 80 simultaneous sources complete at least once while retention stays bounded at 48; job-based fairness tradeoff documented.
- ✓ Bounded helper diagnostics implemented; intentional SIGKILL, SIGSTOP/watchdog and explicit reconnect tests pass, preserving live/saved document and durable WAV. Original v1.1 cause remains unresolved. First fault run exposed a diagnostics lifetime bug, corrected before qualification.
- ✓ Corrected fixture-qualified has exactly10,000 unique real WAV paths,1,253,073,116 bytes and133 distinct hashes. All10,000 headers/sizes/hashes rechecked after runs. Earlier collision and off-screen-row harness failure retained separately.
- ✓ Final cold/warm120s runs complete80 full viewports each. Actual30-minute48k/512 soak completes1197 full viewports/299 pinned semantic refreshes,with continuous nonzero signal and zero underruns/events/retargets. Separate48k/25660s check passes;512 restored. No30-minute256 claim.
- ✓ All17 final CTest suites pass in59.81s. Native pinning,alias/replacement,internal card/Library drops and Undo,snapshot/reopen,full/primary-layer render/print-back,normal/minimum layout and focus checked;relevant screenshots retained.
- ✓ Bounded helper investigation completed. Historical cause remains unresolved;next-trigger diagnostics documented. Physical Escape/listening/display observations have no human result. Resolve21.1.2 reachable;native receiving-drop/save/exit/reopen gate remains blocked by current drag routing,no inferred acceptance.
- ✓ Latest working Rain glass r9054 reopened unchanged;7 prior QA outputs and3 working-document receipt hashes intact. Recorded-takes brief prepared only.
- ✓ Final report/evidence validation complete; coherent local commit created. No remote publication.
