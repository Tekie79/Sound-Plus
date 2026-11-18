# SoundPlus repository instructions

Read this file first at the start of every task in this repository. Preserve explicit user instructions and newer steering throughout ongoing work.

## Main instruction: show a live task checklist

When the user supplies a prompt or attaches files for work:

1. Read the complete prompt and all relevant attached instruction files before implementation. Inspect applicable repository guidance and the current HEAD. Account for later changes when a review names an older baseline.
2. Create a concrete checklist of the work to do. Show it to the user in a commentary update before implementation. For substantial work, also maintain a task checklist in `docs/` so it survives context compaction and handoffs.
3. Keep the checklist live while working. Use a modern, compact **Tasks in progress** display with neutral round status markers: `◉` in progress, `○` pending, and `✓` completed. Show blocked items with their actual reason. The latest user preference is monochrome: do not use colored emojis or markers. Avoid a large repeated basic checkbox list in chat. Update the user when meaningful items finish, not only in the final response. Update the saved checklist at the same milestones.
4. Mark an item complete only after the work and its relevant verification are complete. Distinguish implemented code, automated tests, native UI checks, measurements and human listening. Do not turn a tool attempt into a verified pass.
5. Continue through implementation and verification. Preserve the user's latest design and source media. Do not replace newer edits with an older QA snapshot. Retain unrelated working changes.
6. Finish with the outcome, actual checks, remaining limitations and commit identifier when committing is requested. The user has asked to commit completed SoundPlus work at the end.

Keep updates concise and easy to follow. Do not leave the user without a meaningful progress update for more than about a minute during sustained work. Carry the original task forward when the user adds a correction or asks for status.

## Existing product constraints

- Preserve the JUCE/C++ shared Library, Designer, session and audio architecture and the current Lucide/theme system.
- Fade controls stay pinned to clip top corners, use subtle two-tone accent fills, and keep their IN/OUT directions during dragging. The latest requested size is 10 logical points with subdued default opacity.
- Protect the latest working session before rebuilding; close all processes running from the application bundle before rebuilding that bundle.
- Use isolated QA catalogs/documents and generated fixtures for tests. Keep source media in place and durable renders intact.
- Keep parsing, allocations, file/database I/O and graph preparation out of the real-time callback.
- Cross-application delivery is qualified only by actual receiver import, placement and save/quit/reopen persistence. Native drag initiation alone does not pass. Keep human listening separate from PCM checks.
