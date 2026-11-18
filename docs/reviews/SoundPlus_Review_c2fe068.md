# SoundPlus — latest-commit review and Designer correction plan

**Review date:** November 18, 2026<br>
**Repository:** https://github.com/Tekie79/Sound-Plus<br>
**Reviewed commit:** c2fe068d333efa0e45b832ad895c3039da523c36<br>
**Commit title:** Add Designer v1 layer editing, variations and durable WAV rendering<br>
**Commit time:** November 18, 2026, 6:54 a.m. America/New_York<br>
**Compared with:** aca882119a58dd678d09afe15311b28756ad42de

| Episode | Scene | Department | Priority | Next action |
|---|---|---|---|---|
| Any project | Any scene or audio-only work | SoundPlus / sound-design software | P1 | Complete Designer v1.1: document safety, transport, and direct editing |

## Review decision

The user's observation is correct. The canvas paints fade lines and a playhead, but exposes only move and edge-trim drag modes. Fades are adjustable through inspector sliders; the playhead supports click positioning and arrow nudges. Direct fade dragging and playhead dragging/audible scrubbing are absent.

The next milestone should be **Designer v1.1 — direct editing, transport, and document safety**. Preserve the current standalone architecture and modern dark visual direction. Complete the precision layer editor before adding the Krotos-inspired Performance workspace.

The underlying design should support both eventual modes: a layered editor for precise placement and a performance surface for exploratory gestures, both editing the same saved design and audio graph. An XY surface is a controller; it does not by itself implement a 360-degree spatial renderer or surround output.

## Evidence and limits

This review inspected the actual repository at the pinned commit, the commit changes and surrounding Designer/session/audio/model code, Library integration, tests, and the committed normal/compact screenshots. It also recalculated the canvas drag and trim formulas for concrete numerical examples.

The review environment is Linux. The project explicitly requires macOS Apple Silicon and Objective-C++, so the native application, CoreAudio output, CTest suites, external-app drag, and human listening were not rerun here. Native successes in docs/DESIGNER_V1.md remain the project's reported evidence.

Sources: [build requirements](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/CMakeLists.txt#L1-L17), [implementation and verification report](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/docs/DESIGNER_V1.md), [normal Designer screenshot](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/docs/verification/designer-v1.jpg), [compact Designer screenshot](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/docs/verification/designer-compact.jpg).

**Severity meaning:** P1 should be fixed before accepting the next Designer milestone because it risks lost work or control/transport failure. P2 is a concrete editing, state, or layout defect. Missing planned features are listed separately from bugs.

## What is already useful

- DesignLayer stores referenced source regions, source-clock bounds, placement, sync, fades, gain, pan, linked rate, reverse and inserts.
- DesignDocument stores original and variation mixes. The same graph is used for playback preparation and rendering.
- Rendering captures a frozen document, stages output, verifies it, and publishes a permanent WAV with a receipt.
- The Library and Designer share audition ownership. Search can continue without intentionally taking over the Designer transport.
- The screenshots show a coherent visual direction: dark panels, a compact browser, a contextual inspector and restrained controls.

Sources: [design model](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignModel.h#L16-L74), [render path](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignAudio.cpp#L142-L169), [shared ownership](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/Workstation.h#L10-L24).

## Confirmed and high-confidence findings

### R01 — P1: An old save completion can assign its filename to a new design

**Where:** DesignSession.cpp 48–51; DesignerView.cpp 156, 195–197.

The background save snapshots document A, but its completion assigns savedFile unconditionally. Only clearing dirty is guarded by document identity/revision. New is available while saving.

**Reproduction:** Delay A's save; create B; allow A's save to finish; edit B and press Command-S. B now uses A's destination and can overwrite it. A backup provides limited recovery but does not correct the destination.

**Fix:** Give each document lifetime and asynchronous operation an identity/token. A completed operation must update only the matching active document. Apply equivalent protection to Open and asynchronous dialogs.

**Acceptance:** A controlled worker barrier must prove that A's completion cannot alter B's path, dirty state, output, selection or status.

Sources: [save completion](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignSession.cpp#L42-L51), [enabled controls](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignerView.cpp#L156-L157), [Save destination](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignerView.cpp#L195-L197).

### R02 — P1: New/Open can discard changes before autosave captures them

**Where:** DesignSession.cpp 42–46, 95.

New immediately replaces the document and clears histories. Open completion also replaces the current document. Neither protects the latest dirty revision. Autosave waits for roughly 500 ms without editing.

**Reproduction:** Edit and invoke New immediately, or finish a continuous gesture and immediately open another design. Changes since the last quiet autosave can disappear without undo.

Per-document UUID recovery files do exist; it would be inaccurate to claim there is only one recovery file. They cannot contain changes that have not yet been saved.

**Fix:** Use one document-transition procedure for New, Open, Recover and Quit. Offer Save/Discard/Cancel or guarantee a current recoverable snapshot before replacement, with an accessible recovery workflow.

Sources: [replacement and autosave](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignSession.cpp#L42-L46), [autosave scheduling](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignSession.cpp#L91-L95).

### R03 — P1: Quit can skip the final dirty snapshot when the worker is busy

**Where:** DesignSession.cpp 18–29.

The destructor sets quitting before joining. The worker's final dirty save is inside its condition-variable wake branch, but its outer loop is while-not-quitting. A worker already executing a job can return or throw, fail the outer loop condition, and exit without entering the final-save branch. An in-flight autosave can also cancel when quitting becomes true.

**Fix:** Capture an immutable shutdown document on the owning thread and explicitly flush it through an unconditional shutdown path. Surface a real write failure instead of treating cancellation as successful retention.

**Acceptance:** Close with the worker idle, preparing a preview, writing autosave, and failing a save. Verify the intended last retained revision in every case.

Source: [worker lifecycle](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignSession.cpp#L11-L29).

### R04 — P2: Recovery can restore an older revision after a successful explicit save

**Where:** DesignSession.cpp 15–16, 48–51, 90–95.

Startup loads recovery.soundplus automatically. Successful explicit Save does not retire or synchronize that recovery record.

**Reproduction:** Let revision N autosave; make another edit and immediately Save N+1 before the quiet autosave; quit; restart. Startup can revive N as dirty even though N+1 exists at the user's saved path.

Additional lifecycle inconsistencies: toolbar Recover uses Open and marks recovery clean; autoRevision is not tied to document identity or reset by Open; it is advanced before write success, preventing retry of a failed autosave at the same revision.

**Fix:** Track recovery by document ID, revision, saved path and clean/discarded status. Acknowledge an autosave only after successful publication; retry failures. Avoid reviving stale recovery.

Sources: [startup recovery](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignSession.cpp#L11-L16), [save](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignSession.cpp#L48-L51), [recovery and timer](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignSession.cpp#L90-L95).

### R05 — P1: Long seeks block the helper control/status loop

**Where:** AudioEngine.cpp 98–103, 176–195; DesignAudio.cpp 114–125; AudioIPC.cpp 218–238.

A play-design command synchronously constructs and seeks a graph. Seek renders all preceding frames from zero to reconstruct effects. This runs on the same loop that handles commands, refills playback and emits status. The constructor does not pass through seek cancellation.

**Consequence:** Preparation can prevent Stop/new seek processing and starve old playback refill. If it takes more than five seconds, the existing watchdog considers the helper unhealthy and terminates/restarts it. The native duration threshold needs measurement; the blocking control path is visible in the source.

The same constructor can delay Cancel during a selected-range render before its cancellable read loop begins.

**Fix:** Use cancellable preparation away from the control/status loop, generation-aware delivery and a bounded seek strategy. Keep an authoritative transport clock. Scrubbing needs this work before it can safely generate frequent requests.

**Acceptance:** Late seeks on short and long designs; repeated newest-wins seeks; Stop and Cancel during preparation; continuous heartbeats; no helper restart or stale result takeover.

Sources: [synchronous command](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/AudioEngine.cpp#L98-L103), [service loop](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/AudioEngine.cpp#L176-L195), [seek reconstruction](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignAudio.cpp#L111-L136), [watchdog](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/AudioIPC.cpp#L218-L238).

### R06 — P2: Left-edge trim changes placement beyond the actual available source trim

**Where:** DesignerView.cpp 50–53.

The source boundary is clamped, but placement receives the full pointer delta. When the source cannot extend, placement continues moving.

**Calculated example:** A full three-second source starts at timeline 5 s and ends at 8 s. Drag the left trim two seconds left. Source IN remains zero, but placement becomes 3 s and the right edge becomes 6 s. The operation moves the entire sound instead of holding the opposite edge.

A similar mismatch occurs when placement reaches timeline zero; reverse and linked-rate cases need equivalent bounds.

**Fix:** Derive one admissible trim delta from source bounds and timeline bounds, then use that accepted delta to update both fields. Preserve the opposite timeline edge, accounting for documented sample rounding.

**Acceptance:** Forward/reverse; 44.1/48/96 kHz sources; linked rates 0.25/1/4; source boundaries, zero time, one-sample regions and repeated drags.

Source: [trim implementation](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignerView.cpp#L48-L55).

### R07 — P2: The time-to-pixel scale changes while dragging

**Where:** DesignerView.cpp 24–26, 50–53.

extent() depends on the current design end, and mouseDrag uses that changing extent against the original pointer anchor. Moving or trimming the last layer changes the scale during the same gesture.

**Calculated example:** For a 10-second layer, one-second tail and 820 usable pixels, the current formulas map a 100 px move to 1.40854 s. The next event at 101 px maps to 1.60479 s because the preceding move expanded the timeline. That one-pixel change produces about 196 ms of additional movement.

**Fix:** Persist viewport start and pixels-per-second separately from content duration. Capture the conversion at gesture start. Make Fit an explicit action; use predictable autoscroll at viewport edges.

Source: [extent and pointer conversion](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignerView.cpp#L23-L26), [drag conversion](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignerView.cpp#L48-L53).

### R08 — P2: Empty row space and painted controls are treated as clip body

**Where:** DesignerView.cpp 41–55.

Any horizontal position beyond the row label becomes a trim or move gesture. There is no test that the point is inside the actual clip rectangle. Fade lines, sync triangles and the playhead also have no distinct drag targets.

**Reproduction:** Drag in empty timeline space before or after a layer's clip. It moves that layer. Drag near a visible fade line or the playhead over a clip and the clip can move instead of the expected control.

**Fix:** Introduce explicit hit testing with target priority and distinct hover/cursor feedback. Empty space should seek/select a range according to the selected tool. Playhead, fade and trim targets must take precedence over body move.

Source: [canvas event handlers](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignerView.cpp#L41-L55).

### R09 — P2: A parked playhead snaps back after natural EOF

**Where:** DesignSession.cpp 70, 93; AudioStream.h 73–76, 92.

After natural EOF, the audio stream still identifies itself as a design, though playing is false. The session timer keeps copying its old ending sourceSample into the document.

**Reproduction:** Play to the end, click halfway through the timeline, wait more than 100 ms, then press Play. The parked head is overwritten; the play-start logic can restart at zero instead of the chosen position.

**Fix:** Let active acknowledged transport own the moving head. On completion, transfer ownership back to the parked UI head and ignore stale generations/status.

Sources: [seek and timer](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignSession.cpp#L66-L70), [timer overwrite](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignSession.cpp#L91-L95), [finished stream status](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/AudioStream.h#L73-L92).

### R10 — P2: Continuous edits are delayed and graph replacement uses an old playback position

**Where:** DesignSession.cpp 25–31, 67, 94; AudioStream.cpp 117–120.

Every edit moves previewDue 100 ms into the future. A continuously moving pointer can keep deferring audible changes until it pauses. When replacement eventually runs, it starts from a previously sampled source position after snapshot writing and graph preparation. The old graph advances in the meantime, so replacement can repeat elapsed audio.

The scheduling and stale-position paths are source findings; their audible size and latency were not measured in this review.

**Fix:** Separate gesture history from bounded-rate audio updates. Apply suitable parameter changes smoothly and preserve transport time. Treat intentional seek and a parameter-only update as different operations.

**Acceptance:** Drag gain/pan/fade for several seconds during playback. Measure input-to-audio latency and capture a ramp/timecoded source to detect repeated sections.

Sources: [edit scheduling](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignSession.cpp#L25-L35), [position capture](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignSession.cpp#L66-L70), [preview timer](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignSession.cpp#L91-L95), [graph swap](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/AudioStream.cpp#L117-L120).

### R11 — P2: Dropping a sound ignores its timeline drop position

**Where:** DesignerView.cpp 185–189; DesignSession.cpp 53–55; DesignModel.h 22.

Internal and external drop handlers add sources without using the drop coordinates. A newly constructed layer has placement zero.

**Fix:** Convert the canvas drop point through the viewport transform, preserve captured source IN/OUT and mapping, and show a placement preview. Define Add-button behavior separately, such as adding at the parked playhead.

Sources: [drop handlers](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignerView.cpp#L185-L193), [layer addition](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignSession.cpp#L53-L55), [default placement](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignModel.h#L16-L25).

### R12 — P2: Undo can restore one output folder while rendering uses another

**Where:** DesignerView.cpp 193; DesignSession.cpp 39–42, 84–87.

Output folder exists both in the document and the session. The chooser updates both; Undo restores only the document; Render uses the session variable.

**Reproduction:** Select folder A, select B, Undo, Render. The restored document says A, but the output goes to B. New also retains the old session folder while clearing the document field.

**Fix:** Establish one source of truth or consistently derive the session value after document replacement/undo/redo. Define and serialize any deliberate folder inheritance.

Sources: [folder edit](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignerView.cpp#L190-L197), [undo/new](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignSession.cpp#L37-L42), [render destination](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignSession.cpp#L84-L89).

### R13 — P2: Compact browser hides search scope and cannot display empty states correctly

**Where:** LibraryView.cpp 276–294, 365–378, 577–595; Workstation.h 15, 33.

The compact browser retains the source-folder filter but hides the source controls and scope indicators. Its empty-state painting still uses x=270 and width=componentWidth-310. At the default 300 px width, that text area has negative width.

**Reproduction:** Filter to folder A in Library, enter Designer and search for a file in B. It is excluded with no visible scope/clear action, and the empty result explanation may not render.

**Fix:** Show an active-source chip with All sources/clear, and lay out wrapped empty/loading/error messages inside the actual result bounds.

Sources: [empty-state geometry](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/LibraryView.cpp#L276-L294), [hidden controls](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/LibraryView.cpp#L365-L378), [query scope](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/LibraryView.cpp#L577-L595), [browser width](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/Workstation.h#L28-L34).

### R14 — P2: Find Similar layout overlaps at the newly supported minimum window size

**Where:** Main.cpp 94; LibraryView.cpp 324–344.

The minimum outer window is 1040 × 680. With a reference visible, results begin at y=373 with minimum height 90. At a 680 px component height, selected-recording controls begin at y=360 and its waveform at y=412. These areas overlap; a native title bar can reduce available content height further.

**Fix:** Make the reference/results/selected-sound regions adapt or scroll, preserving non-overlapping usable bounds.

**Acceptance:** Full Library with/without a reference and Designer with panels open/collapsed at minimum and comfortable desktop sizes.

Sources: [minimum window](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/Main.cpp#L90-L97), [Library layout](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/LibraryView.cpp#L324-L344).

## Missing direct interactions

These are product gaps, not claims that existing DSP is absent.

| Interaction | Current state | Next requirement |
|---|---|---|
| Move clip body | Implemented with hit-test/scale defects | Stable direct movement with snap bypass and fine control |
| Trim clip edges | Implemented with clamp mismatch | Opposite edge stays fixed at bounds and across rates/reverse |
| Fade in/out duration | Inspector sliders; painted canvas lines | Visible corner handles with drag and exact duration feedback |
| Fade shape | Linear DSP only | Separate future schema/DSP requirement; do not imply curve editing now |
| Playhead | Click seek and arrow nudge | Dedicated grab handle and ruler drag |
| Audible scrub | Absent | Explicit bounded audio-preview behavior with play/pause/Stop semantics |
| Horizontal zoom/pan | Automatically fitted extent | Persistent viewport, cursor-centered zoom, pan and Fit actions |
| Gain line | Inspector gain | Constant layer-gain line; distinguish from automation |
| Source slip | Absent | Optional modifier gesture preserving placement and region length |
| Sync point | Painted triangle; inspector actions | Direct marker drag with source-bound constraints |
| Drop placement | Coordinates ignored | Ghost placement and insertion at drop time |
| Reorder layers | Up/down buttons | Header drag with insertion target |
| Multi-select | Single primary layer | Transient selected-ID set and atomic group move |
| Gesture cancellation | No explicit Escape rollback | Escape restores the entire pre-gesture state |
| Range/loop/split/crossfade | No complete canvas workflow | Design separately; avoid a hidden expansion into a full DAW |

Sources: [canvas](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignerView.cpp#L11-L56), [inspector](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignerView.cpp#L75-L98), [keyboard commands](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignerView.cpp#L216-L219), [linear envelope](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/DesignAudio.cpp#L82-L92).

## Recommended milestone boundary

### v1.1A — Document and transport reliability

Close R01–R05, R09–R10 and R12. Establish deterministic session tests with a controllable executor/audio interface. Fix these before increasing the frequency of edit/seek requests.

### v1.1B — Direct editing

Close R06–R08 and R11. Add fade duration handles, playhead drag, zoom/pan/Fit, constant gain line, sync-point drag, correct drop placement and layer-header reorder. Add transient multi-selection/group movement and a well-defined optional slip gesture after the single-layer operations pass.

Use explicit hit-test/gesture/viewport code with pure geometry/timing logic that can be tested independently. Keep integer source/design clocks and a single shared edit path so inspector, keyboard and canvas produce the same result.

### v1.1C — UI and delivery qualification

Close R13–R14. Add hover targets/cursors, readable timing/value feedback, a visible active search scope and reliable minimum-size layout. Qualify external file drag on the actual Mac.

### Subsequent milestones

Add the Performance workspace, macros/XY control and performance recording against the same design model. External plug-in hosting, independent pitch/time processing, automation, richer clip/track editing and spatial output deserve separate requirements and tests. The current one-region-per-layer model is sufficient for the immediate editing milestone.

## Acceptance tests that matter

1. **Gesture correctness:** drag each real handle, inspect resulting model values, Undo/Redo once, Escape cancel, and verify unrelated fields remain correct.
2. **Trim invariants:** opposite edge fixed, nonempty half-open source range, valid sync point, zero/source bounds, reverse, mixed sample rates and linked rate.
3. **Viewport invariants:** changing a layer does not change the time scale under the pointer; explicit zoom/pan/autoscroll are predictable.
4. **Transport ownership:** EOF → park → wait → Play; Stop → seek; repeated latest-wins seek; interrupted preparation; parameter changes preserve clock continuity.
5. **Live interaction:** continuous gain/fade/pan changes during an eight-layer playing mix while search/index work runs. Proposed target: p95 audible response within 100 ms for this measured workload, with no helper restart and no unexplained dropouts. Report hardware and achieved values; this is a new target, not an existing measured result.
6. **Persistence:** delayed save A → New B; Open races; immediate dirty New/Open; quit while worker busy; failed autosave retry; stale recovery; output-folder Undo.
7. **Native UI:** minimum and desktop sizes, expanded/collapsed panels, compact browser at 260/300/wider widths, reference-visible Library, keyboard focus in numeric/text fields, accessible control names.
8. **Delivery:** drag finalized WAV into Finder and an installed receiving audio/video application; observe the received file/clip; save receiver state; quit SoundPlus; reopen receiver state and verify media. Distinguish drag import from File → Open.

The current DesignTests suite provides valuable model/DSP/save-function/render/helper checks, but does not instantiate DesignSession or exercise DesignerView mouse gestures. Its current passes cannot establish the session/gesture behaviors above. Sources: [test target](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/CMakeLists.txt#L67-L83), [Designer tests](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/tests/DesignTests.cpp).

## External drag status

The native drag implementation passes a real file path after a drag threshold and is plausible. This review found no definite platform drag defect in that code. The committed report says cross-application drag remains unverified; File → Open and same-app drag do not qualify it.

Sources: [native drag component](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/src/UIComponents.h#L26-L37), [delivery evidence and limitation](https://github.com/Tekie79/Sound-Plus/blob/c2fe068d333efa0e45b832ad895c3039da523c36/docs/DESIGNER_V1.md#L64-L74).

## Immediate next action

Add this review and Codex_Designer_v1_1.md to the repository, then run the implementation prompt on the Mac build environment. Require a report mapping every review ID to fixed, intentionally deferred, or blocked with evidence, plus a native demonstration of fade dragging, scrubbing, trim limits, undo and durable delivery.

For Yekermo Sew production work, keep detailed Foley/mix polishing for picture lock; rough sound remains appropriate for story decisions.
