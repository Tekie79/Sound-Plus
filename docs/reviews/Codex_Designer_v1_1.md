# Codex implementation prompt — SoundPlus Designer v1.1

## Objective

Implement **Designer v1.1: direct editing, transport, and document safety** in the SoundPlus repository. The reference review is SoundPlus_Review_c2fe068.md, based on commit c2fe068d333efa0e45b832ad895c3039da523c36. Inspect the current checkout first and account for later changes; do not revert newer work or assume every finding is still present.

Read the applicable AGENTS.md if one exists, README.md, docs/DESIGNER_V1.md, the architecture/requirements documents, and the review. If AGENTS.md does not exist, proceed using the available project instructions.

Implement the work and verify it. A plan or UI mockup alone does not satisfy this task. Use small, coherent changes and finish the requested acceptance checks that this environment supports. Clearly identify genuinely blocked native checks.

## Product direction

SoundPlus is a general-purpose standalone sound library and creative sound-design application for any user or project, including audio-only work.

Keep the modern Krotos-inspired direction: dark neutral surfaces, clear typography, strong waveform visibility, restrained accents, consistent icons, a collapsible sound browser, contextual inspector and large editing canvas. Give interactive targets visible hover/active feedback and exact values. Use original styling and assets.

The eventual product combines:
- a precision layered Designer;
- a later performance workspace with macros/XY control;
- the same DesignDocument, source references, edits, DSP and render pipeline.

Complete the current layered Designer first. An XY control is not automatically a 360-degree spatial renderer. Keep external plug-in hosting, surround/spatial rendering and independent pitch/time processing in their separately specified milestones.

The current model is one source region per DesignLayer. Preserve it for this milestone. Multiple clips per track, ripple editing, arbitrary automation, curved fades and automatic crossfades are separate scope.

## Phase A — Protect documents and establish reliable transport

Confirm and fix review R01–R05, R09–R10 and R12.

### Document lifecycle

1. Scope asynchronous save/open/import/dialog completions to a document lifetime and operation identity. A completion for design A must never attach its filename, dirty state or status to newly created design B.
2. Give New/Open/Recover/Quit one consistent dirty-document transition. Use Save/Discard/Cancel where needed, or guarantee a fresh durable recovery snapshot before replacement with a usable recovery workflow. Latest changes must never be silently lost because the 500 ms quiet timer has not fired.
3. Capture an immutable final snapshot on the owning thread and make shutdown flush independent of whether the worker is idle or busy. Handle save failures explicitly.
4. Key recovery state by document identity and revision. Preserve the saved-file association, distinguish saved/discarded/recoverable state, and prevent stale autosave from superseding a newer explicit save.
5. Advance the autosave acknowledgment only after successful publication; retry a failed save at the same revision. Open/New must not inherit another document's autosave acknowledgment.
6. Use one authoritative output-folder value, or derive it consistently after Undo/Redo/New/Open/Recover. Render must use the folder represented by the restored document.
7. Keep schema compatibility and existing source identity/half-open sample-range rules. Add migration and recall tests only for schema changes actually needed.

### Transport and live updates

1. Separate the parked UI playhead from the audio engine's active transport position. Use operation/generation acknowledgments to reject stale status.
2. Fix EOF → park → wait → Play, and Stop → immediately seek. A finished stream cannot keep overwriting the parked head.
3. Keep source inspection, decoding, snapshot preparation, graph construction and DSP-state reconstruction off both the realtime audio callback and the helper command/status loop.
4. Preparation must be cancellable and newest-request-wins. Stop must remain responsive while a late seek is preparing. Heartbeats and old-stream service must continue.
5. Propagate cancellation into initial seek/preroll, including offline range-render construction.
6. Use bounded-rate coalesced audio updates during continuous editing. A trailing-only debounce that makes changes audible only after release is insufficient.
7. Preserve a single authoritative transport clock during parameter-only updates; avoid restarting from a stale sampled position. Smooth appropriate live parameter changes and transition graph replacements safely.
8. Establish and measure the strategy for long seeks: cache/checkpoints or another bounded approach with defined effect-state behavior. Do not claim instant arbitrary long seeks without evidence.

## Phase B — Implement direct canvas editing

Introduce explicit viewport, hit-test and gesture responsibilities, keeping pure geometry/timing logic testable independently from JUCE widgets. Use readable project formatting and small named functions.

### Viewport and hit testing

- Store viewport start and pixels-per-second independently of content duration.
- Centralize screen/design/source conversion with explicit rounding, linked rate, reverse and half-open bounds.
- Capture the coordinate mapping and original values at pointer-down.
- Implement cursor-centered zoom, horizontal pan, Fit Design, Fit Selection and predictable edge autoscroll.
- Keep row headers stable while the timeline pans.
- Use distinct targets for playhead, fade handles, trim handles, sync marker, gain line, clip body, row header and empty space. Define target precedence where they overlap.
- Empty space must never accidentally drag the row's clip.
- Show appropriate cursors, hover highlights and live timing/value readouts.

### Required gestures

1. **Move:** Drag a clip body horizontally. Preserve duration and captured source region.
2. **Trim:** Drag each edge. Constrain one accepted delta by source and timeline bounds, then update source trim and placement consistently. Hold the opposite timeline edge fixed within documented sample-rounding tolerance. Test reverse and linked rates.
3. **Fade duration:** Drag visible fade-in/out corner handles, including when the fade starts at zero. Update the existing linear fade durations and display exact time. Preserve the current DSP law and define overlapping-fade behavior. Do not label this fade-curve editing.
4. **Gain:** Drag a clearly identified horizontal constant-gain line; show dB feedback and a reset-to-unity action. This edits layer gain, not gain automation.
5. **Sync:** Drag the existing sync triangle to edit its source sync sample within the captured interval. Moving this marker must not move audio; retain an explicit Align Sync action.
6. **Reorder:** Drag a layer header with a clear insertion indicator while preserving its timeline placement.
7. **Drop:** Show a ghost at the pointer time; add Library ranges and external files as new layers at that time. Preserve IN/OUT, source identity/revision, channel mapping and external/indexed status. For a multi-file drop, use one new layer per file at the common drop time in incoming order. Define Add-button behavior as adding at the parked playhead.
8. **Multi-selection/group move:** Keep a transient selected-ID set and an existing primary selection. Support additive selection and group movement. Apply one common clamped delta to preserve relative timing; single-layer trim is sufficient for this milestone.
9. **Slip:** Provide a clearly documented modifier gesture for the selected layer that changes source IN/OUT together while preserving placement and duration, respecting forward/reverse/rate/source bounds.
10. **Snap and fine adjustment:** Add visible snapping controls for relevant timeline/grid, playhead, clip edges and sync targets, with temporary bypass and fine adjustment. Select modifiers that do not conflict with text editing or other gestures; document the actual map.

### Playhead dragging and audible scrubbing

Implement the two behaviors explicitly:

- Dedicated playhead/ruler dragging provides immediate visual positioning and correctly owned seek requests.
- An enabled audible-scrub mode previews the active audible mix around the pointer using bounded coalesced requests. Define the preview window, reverse-pointer behavior, ramping and effect-state treatment.
- Preserve the pre-gesture transport state: dragging while stopped returns to stopped; dragging while playing resumes from the release position; Stop cancels the gesture/preparation and stays stopped.
- Do not rebuild/render from timeline zero on every mouse event. Use the reliable preparation path established in Phase A.
- Clearly distinguish a forward preview grain at each location from true reverse-speed audio if the initial scrub implementation uses the former.
- If audible scrub cannot be completed or measured, label the shipped control “drag to seek” and report audible scrubbing as incomplete. Never claim it from a moving line alone.

### Transaction behavior

Every completed drag creates one undo transaction. Escape restores the pre-gesture model and selection. A click without a meaningful change creates no edit/history entry. Release outside the component, focus loss and cancelled capture have defined safe behavior. Undo/Redo and reopening a saved design must reproduce the actual resulting audio settings.

## Phase C — UI integration and delivery

Fix R13–R14 and preserve the compact browser workflow.

- Show active source scope and an All sources/clear action in the Designer browser.
- Render loading, empty, unavailable and error messages inside the actual result area at 260 px, 300 px and wider widths.
- Eliminate overlap in full Library with a Find Similar reference at the minimum supported size.
- Validate Designer with browser/inspector expanded and collapsed.
- Show current playhead time and exact selection/gesture values without requiring guesswork from four ruler ticks.
- Keep common layer actions discoverable and keyboard accessible; text/numeric editing must retain expected shortcuts.
- Qualify native output drag separately from file-open import. Drag a finalized WAV to Finder and an installed audio/video receiving app, observe the result, save receiver state, quit SoundPlus, reopen receiver state and verify the media remains available. If the environment blocks this, retain an explicit unverified status and describe the exact blocker.

## Verification

Add meaningful tests for behavior and invariants, not tests that duplicate the implementation.

### Deterministic session tests

Use controlled worker scheduling and a fake/test audio boundary where needed:
- delayed save A → New B → A completion;
- out-of-order/cancelled Open and dialog completion;
- immediate dirty New/Open before autosave;
- close while worker is idle and busy;
- autosave failure and retry without another edit;
- successful save followed by restart without stale recovery takeover;
- output-folder Undo/Redo/New/Open;
- EOF/Stop parked-head ownership and stale status rejection.

### Gesture/timing tests

Exercise pointer target selection and actual edit commands:
- move/trim/fade/gain/sync/slip/drop/reorder/group move;
- one transaction per gesture, no-op click, Escape cancellation;
- source and timeline boundaries;
- 44.1/48/96 kHz sources, forward/reverse, linked rates 0.25/1/4;
- nonempty half-open ranges, fixed opposite trim edge and shared group delta;
- stable time scale while the last layer changes;
- hover/handle precedence and narrow clips.

### Native validation

Run existing relevant tests plus the new tests on the supported Mac when available. Record build commit and environment.

Use the existing eight-layer / 26-insert / 512-frame workload as a defined starting point. Add continuous editing and rapid seeks while search/indexing is active. Measure pointer-to-visual and input-to-audio response, underruns, stale requests, helper restarts and clock continuity. Initial target: p95 live parameter response within 100 ms on that specified workload; report the measured result and any exception.

Test short and substantially longer designs, cancellation during late seek and range-render preroll, natural EOF, minimum-size layout and external drag. Preserve the existing live/offline sample comparison where applicable. A steady-state search benchmark does not establish editing responsiveness.

## Deliverables

- Working code and meaningful targeted tests.
- Updated docs/DESIGNER_V1.md or a clearly linked v1.1 document.
- Actual gesture/shortcut map and screenshots showing handles, time/value feedback, zoom and compact states.
- A review closure table for R01–R14: fixed, intentionally deferred, or blocked, with evidence.
- Test commands/results, native measurements and an honest list of unverified checks.
- A concise description of saved-format changes, if any, and compatibility handling.

Do not mark the milestone complete unless fade dragging, bounded trimming, stable movement, playhead dragging, document retention and the stated transport behavior work. Keep audible scrub and cross-app drag as separate measured acceptance items. Continue through implementation and verification; ask the user only for an actual unresolved product decision or access blocker.
