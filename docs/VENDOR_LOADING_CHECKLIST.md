# Tasks in progress — real vendor loading

Baseline `056ff389025f40e895e1f7891cfe47eaeca0e2c4`, preserving unified/contextual insert UI after reviewed `3e09c9d`.

- ✓ Read complete task, guidance, hosting contract and matrix; identify current app and protect user catalog.
- ✓ Baseline installed-vendor inventory and actual failing-stage reproduction.
- ✓ R26 optional MIDI / declared auxiliary-bus negotiation and negative fixtures.
- ✓ R27 class-level retry and adapter-policy cache validity.
- ✓ R28 searchable failure/layout diagnostics and targeted recovery UI.
- ✓ R29 per-editor interactive lifetime, independent cancellation and recoverable state.
- ✓ R30 height-change relayout across stock/hosted selections.
- ✓ Two real-vendor Track/Master native insert, parameter/editor/state/render workflows.
- ✓ Targeted + full regression gate, sanitizer and portable audits, final identities/report, restore latest user session.
- ✓ Final review, closure report and retained evidence prepared.

VST2 dependency/distribution and AUv3 runtime qualification remain distinct open requirements. No Expanded Perform work while loading is unqualified.

TAL-Reverb-3 loads after the MIDI/parameter-surface corrections but inline PCM response remains unqualified despite confirmed advertised-ID queue delivery. Qualification uses installed LV50A and newly observed Voxengo OldSkoolVerb; no vendor was installed or activated by this task.

Native editor opening/return/recall is verified separately from direct custom-editor gestures; Voxengo custom gestures remain unverified. Full CTest 26/26, ASan 157,697 checks, portable 505 checks. See REAL_VENDOR_LOADING_REPORT.md.
