# SoundPlus — Repository Agent Instructions

Read this file **before starting any task** in this repository. Follow the user's latest explicit instructions, preserve newer decisions, and reconcile older prompts against the current repository state.

## 1. Required: live task progress

Show a **Tasks in progress** panel in a commentary update **before implementation**. Keep it current throughout the task; it is a working status display, not a one-time plan.

### Display style

- Use a clean, monochrome, compact layout. No colored emoji, decorative badges, large tables, or repeated checkbox lists.
- Title the display **Tasks in progress** and, when useful, show an accurate completion count (`2/5 complete`).
- **Indent each task line with exactly two tab characters**. Put the task lines in a fenced `text` block so Markdown preserves the indentation and alignment.
- Use these status markers consistently: **`✓` complete**, **`◉` in progress**, **`○` pending**. For a blocked item, keep `○` and state **`Blocked: <specific reason>`**.
- Keep each line short and action-oriented. Prefer **4–7 milestone-level tasks**. Put detailed subtasks in the saved checklist rather than crowding the chat.
- Show the current active task (`◉`) clearly. Do not mark more than one item active unless the work is genuinely parallel.

**Example (replace with actual task state):**

**Tasks in progress** · 2/5 complete

```text
		✓  Review      Read instructions and inspect current HEAD
		✓  Plan        Confirm scope, compatibility and test gates
		◉  Implement   Build the active milestone
		○  Verify      Run focused tests and native UI checks
		○  Deliver     Final audit, commit, push and report
```

If blocked, show the reason on that same line, for example:

```text
		○  Verify      Blocked: no physical 5.1 interface available
```

### Progress updates

1. **Before implementation:** Read the full prompt and attachments, applicable contracts, this file, and the current HEAD. Identify later changes and protect the latest user state. Then post the initial progress panel.
2. **During work:** Update the panel when meaningful milestones finish, verification changes the status, or a blocker appears. Keep user-visible updates concise; avoid more than about a minute without a meaningful update during extended work.
3. **For substantial work:** Maintain a matching task checklist in `docs/`. Update it whenever the visible milestone statuses change so progress survives handoffs and context compaction.
4. **For interruptions or new directions:** Reconcile the new instruction with the original goal, revise the task list, and continue from current work. Do not restart completed work unnecessarily.
5. **For completion:** Show the final verified status, actual checks, remaining limitations, and relevant commit identifiers.

**Status integrity:** Mark `✓` only when the task **and its required verification** are complete. Distinguish source changes, automated test results, native UI checks, benchmark measurements, human listening, physical-device validation, and cross-application acceptance. A tool invocation, an attempted test, or a retained log from an older build is not proof that the current build passed.

## 2. Execution and delivery

- Follow the newest user instructions over superseded prompts or older checklists. Never discard unrelated working-tree changes or overwrite newer work with an older snapshot.
- Inspect current HEAD, the working tree, applicable repository guidance, and relevant contracts before editing. State the actual baseline and keep source/build identities in the report.
- Implement in reviewable stages with focused regressions. Run the required complete qualification gate on an unchanged final build when the milestone calls for it.
- Preserve the **latest** working document, unsaved edits, catalog, preferences, recording journals, source media, impulse responses, take recipes, and finalized outputs. Use isolated QA data and generated fixtures.
- Before rebuilding the application bundle, protect the latest working session and close all processes running from that bundle.
- Finish with a concise outcome report: implemented features, actual verification, known gaps, preserved state, and commit IDs.
- **Commit and push completed, verified SoundPlus work to the configured remote** under the latest explicit user instruction. Verify the push result and report the destination/commit. Never describe uncommitted or unpushed changes as published.

## 3. Product and technical constraints

- Preserve the native **JUCE/C++** architecture, shared Library/Designer/session/audio model, current Lucide icons, and theme system.
- Keep **Audio** and **MIDI Instrument** tracks distinct. Organizational folders must not silently change Group Bus audio routing.
- Each processable track has **exactly one collapsible automation lane**, collapsed by default and initially showing **Volume**. Selecting another parameter changes the **displayed** curve only; other saved, read-enabled curves continue to play. Do not create separate visible lanes for individual effects.
- Keep the Edit square surround-panner direction distinct from the Perform source-blending pads. Preserve existing multichannel layouts, channel identities, routing, latency compensation, and historical DSP/take compatibility. Do not present proposed features as implemented before they pass verification.
- Preserve the existing Perform Pad A–D shared-clock model, independent pad controls and overdubs, as qualified in its current contract.
- Keep fade controls pinned to the upper corners of clips, with subtle two-tone accents, correct IN/OUT dragging directions, **10-logical-point** size and subdued default opacity.
- Keep parsing, allocation, file/database I/O, plugin construction, graph preparation, and blocking waits **out of the real-time device callback**.
- Preserve source-file identity and original audio. Virtual collections must not move or duplicate source media. Missing or changed references must fail safely without silent reassignment.
- Qualify cross-application delivery only after the receiving app actually imports, places, saves, quits, and reopens the material successfully. Native drag initiation or internal drop alone is insufficient.
- Report **human listening**, physical surround/MIDI/controller/DAC tests, installed vendor compatibility, numerical PCM checks, and native UI tests as separate evidence categories.

## 4. Final response standard

At the end, report the verified outcome in brief prose, followed by a compact final **Tasks in progress** panel if useful. State the exact passing/failing gate counts, any qualification still open, current source/build identity, preservation result, and commit/push status. Never claim production readiness solely from passing automated tests.
