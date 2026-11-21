# Recorded Perform takes v1 — implementation contract

Implementation decisions for Performance Mixer v1.3. Executed evidence and qualifications are in [the milestone report](PERFORMANCE_MIXER_V1_3.md).

## Ownership and states

| State | Meaning and allowed completion |
| --- | --- |
| Idle | Editable Live Design. Record freezes a worker snapshot and reserves transport immediately. |
| Arming | Source resolution/content hashes, atomic draft storage and fresh graph preparation. The capture clock has not started. Stop/Cancel invalidates the pending completion; a slow OS read may finish later but cannot publish that cancelled draft. No arbitrary preparation timeout. |
| Recording | The accepted generation produces PCM. Only XY, Blend enable and four macro values/enables/reset can change. Search/browsing remain available. Document/structural edits, seeks, device changes, Library audition and other graph requests cannot take over. |
| Finalizing | Stop quiesces callback consumption and reconciles the captured prefix. The checkpoint worker persists it atomically. It is not Ready before durable acknowledgement. |
| Ready | Complete consumed body with an immutable frozen recipe and timeline. Audition, Keep/name, Render Take and ordinary-layer print-back are available. |
| Incomplete | Explicit interrupted prefix or failed preparation/finalization. It cannot audition/render as a complete take. Source/helper/device/generation failure, overflow, sequence corruption, empty capture, rejected work or storage failure cannot become Ready. |

Cancel removes the document draft and restores the unchanged editable controls. Normal source EOF and the 120-second limit finalize the consumed boundary. A recovered prefix is always Incomplete, even if its last checkpoint had marked completion. Source verification, worker-admission failure and cancellation release the Arming reservation. Closing/changing documents during active capture requires Stop/Cancel first.

## Clock and event boundary

Record begins at design frame zero with freshly prepared stock DSP and defined initial normalized controls. One typed full state contains atomic X/Y, Blend enable, four macro values and four enable flags. Stable identities are XY/Blend and macro slots 0–3, never labels or row positions. Reset is captured as its effective value. Assignments, mappings, trims, layers/racks, blend law, project rate and source references freeze before capture starts.

IPC/producer inputs coalesce. `DesignReader` retains the latest pending state until its graph actually produces source frames. Immediately before that production it applies the state through the performance resolver and appends a contiguous sequence at `DesignGraph::position()`, in integer project frames. Commands received while the FIFO is full do not themselves create events. Resampler lookahead is part of this source-production clock. UI time, IPC receipt and output-buffer bookkeeping are not capture timestamps. Unapplied same-frame input states collapse to the final full state; persisted same-frame events have explicit sequence order and replay accepts that order without UI coalescing.

The device callback consumes existing SPSC PCM and publishes cursor/hazard state. On Stop, the producer removes the active pointer and waits outside the callback for its hazard to clear. Exact captured duration is the rational `consumedOutputFrames / captureRate`. For nonempty capture, the exclusive source interpolation dependency is:

```text
bodyEnd = ceil((consumedOutputFrames - 1) * projectRate / captureRate) + 1
```

Zero consumed frames gives bodyEnd zero. This may retain one guard frame beyond the nominal project boundary without extending delivered duration. Events at or after that exclusive boundary are removed; tentative read-ahead never lengthens a completed take. Pending requests and finalization are generation-gated.

## Limits and persistence

The limits are 120 seconds, 8192 applied events and 16 takes per document. One fixed 8192-slot event array (655,360 bytes on the qualified build), one latest coalesced state, one serial audio producer and one checkpoint worker bound active recording resources. Slots are published once with release/acquire ordering. No per-event callback allocation, serialization, graph work or I/O occurs. Peak checkpoint backlog is measured.

Atomic checkpoints have 250 ms opportunities; storage/OS delays can extend this interval. They serialize only the consumed prefix off the producer/callback. The frozen recipe stores source IDs/revisions/stamps and SHA-256 content fingerprints. Live source resolution cannot substitute different media into a take. Explicit relink accepts identical bytes and stores the override path/stamps separately from the frozen recipe/hash.

Capture controls use a separate gesture snapshot and neither mutate the live recipe nor add Undo entries per event. Stop/Cancel returns to the unchanged editable values. One Undo removes a finished recording. Keep/name/delete are intentional independent undoable actions. Immutable recipe and copy-on-write event storage keep historical take data shared across ordinary Undo snapshots.

Documents containing takes require manifest 4 / `stock-takes-v1`; unsupported static readers reject them. Static documents retain their existing versions/tags. Recipe/event hashes and the document envelope checksum are checked. Atomic staging/backup/recovery remain the shared session pipeline. Loading is bounded to 64 MiB.

## Replay, conversion and history

The shared `DesignGraph` applies frozen events at absolute source frames through the same performance resolver and stock DSP. Processing runs at the stored project rate. Live replay and offline output conversion use the same reader/resampler; integer frames and the exact rational body duration avoid cumulative drift. Take processing uses a partition-independent 32-frame filter coefficient phase so FIFO refill/offline block sizes do not alter smoothing. Existing static processing is unchanged.

Take audition always starts at frame zero and rebuilds full history. Live Design's approximate seek policy remains unchanged; use Accurate audition for long-tail decisions. Take source input is gated at the final consumed interpolation dependency. The reader delivers the exact rational body plus one chosen effect tail; the tail is not duplicated. Monitor volume/monitor transitions are excluded from renders and original pre-monitor comparisons.

## Context and output

Live Design and Take: name are explicit contexts. A selected take drives shared transport and full render; selection rendering is unavailable in take context. During audition, moving controls show accepted playback state read-only, without writing the live recipe. The take strip reuses the existing tool row, preserving the pad/card area.

Receipts identify output kind, take ID, recipe/event hashes, processing/source provenance, project/capture/output rates, consumed/body/output frames, tail policy, render settings and final WAV hash. Drag is enabled only for a finalized existing output matching the selected context. Print-back switches to Live Design and adds that WAV through the ordinary referenced external-layer path. Physical listening and cross-application receiving-project persistence are distinct qualifications, not inferred from PCM equality or drag initiation.

## v1.3.1 integrity corrections

Metadata actions explicitly use `ChangeImpact::metadata`. Names/Keep, output receipts and all output-settings callbacks do not schedule live parameters or graph replacement. Pending take preparation has no live revision catch-up. Mutable Live Design (including its editable variations) has one document transport context; immutable replay carries document/take identity plus recipe/event hashes. The service independently rejects live commands during immutable replay, and both reader and graph reject external parameter updates. Internal frozen events remain the only replay automation. Undo/context restoration stops an incompatible take audition rather than labeling its audio as another context. Monitor volume remains independent.

One Ready-output resolver is used after restoration, completion, selection, Undo/Redo and deletion. A take receipt must match take ID, recipe/event hashes, processing version, project/capture clocks, consumed/body frames and source IDs/content fingerprints. The finalized path must exist. Delivery refreshes and the actual drag/print actions resolve that current context again; a nonempty cached path is not authoritative. Completed receipts and permanent WAVs survive take deletion/Undo. Live Design retains the latest existing static-output behavior. Output settings affect future audition preparation/renders, not an audition already running; Render Take always exports the complete captured performance, so its timeline-range fields are unavailable.

Rendering prepares one verified effective take-input snapshot, including explicit identical-content relocations. Metering/normalization, graph production, receipt source paths/revisions/stamps and final publication guards use that same snapshot. Public reader/graph construction cannot supply an unchecked replacement input. Unrelated missing Live Design paths do not participate. Existing-source mutation detected before publication rejects the temporary output; older finalized outputs remain intact.

Capture reservations now reach the helper before worker verification finishes. Reservation IDs distinguish a current Arming failure from stale status. A callback-safe first-fault latch records Stop, error (bounded 191-character diagnostic), or meaningful rate change independently of the latest rate. A subsequent same-rate Start cannot clear it. Callbacks use only bounded lock-free atomics and character copies; diagnostics, interruption, serialization and recovery run off the callback. Idle initialization is separate from capture; a deliberate new reservation establishes a fresh lifecycle baseline. Failure before a frozen draft exists produces explicit Incomplete status with no accepted audio; it invalidates the held worker completion. Existing prefixes remain recoverable Incomplete. Capture intent is not automatically replayed after a device event.

The latch stays armed through finalization. After durable checkpoint storage, an atomic armed-to-committed transition authorizes Ready. A competing first fault that wins the latch prevents Ready, including a late checkpoint completion. A device event after this commit is an idle/future-device event and does not invalidate an already completed performance. Physical unplug/replug is qualified separately from controlled invocation of the actual device handlers and producer/checkpoint service.

Every write to the display-only waveform cache uses the same summary constructor, including Offline/Changed recovery. Retained bounds are 48 full-resolution sources, 96 geometries, and 160 summaries of at most 2048 all-channel conservative peak pairs (2,621,440 bytes total summary peak payload). Selected/card promotion still obtains full peaks. The oldest overdue validation retains its protected admission at least every fourth admission; this is a scheduler-service bound, not an interruption bound for blocked OS I/O.

Executed v1.3.1 results and carried qualifications are recorded in [the correction report](PERFORMANCE_MIXER_V1_3_1.md).

## Plugin-aware recorded takes v1 (manifest 10)

A take containing hosted inserts uses document `10 / plugin-takes-v1`, frozen recipe `recipeVersion: 3` and processing `plugin-ordered-takes-v1`. Older stock recipes retain their encoding, hashes and event interpretation. A lower document/recipe version cannot label a hosted recipe as stock. The recipe checksum includes canonical typed Track/Master order, slot IDs, rack/slot/legacy-group bypass, stock state, native opaque state and stable parameters, plugin descriptor/version/binary fingerprint, source IDs/SHA-256, pool-choice provenance, source region/fades/rate/routing, named roles, project rate and initial Perform state. The explicit render bus is Stereo for a historical unnamed design; its historical Track staging is preserved.

Record reserves transport and enters Arming before the coherent native-state barrier. Completion is bound to the operation/document/variation context and a source/recipe/identity signature. Native state can advance at this barrier, but source/timing/routing/order/identity changes cannot silently enter the pending take. Failed/cancelled/stale capture does not publish a recording. Native editors are hidden during capture and selected-take audition; open-editor commands are rejected in these contexts. Return to Live Design to edit/reopen them.

`fixed-state-independent-frame-zero-v1`: the capture graph and every replay/render construct independent headless processors from the frozen recipe, starting at frame zero with fresh history. They never borrow retained Live services. Service jobs explicitly reject a fixed-state/live-service combination and fixed-state control writes. External graph/parameter messages cannot mutate a replay; during capture only the existing typed Perform-control state is accepted. Native plugin-knob automation is **not** recorded. Plugin repeatability is plugin-dependent; immutable state is not a universal bit-exactness promise.

`body-input-gate-output-tail-once-v1`: the existing consumed-output boundary determines the body; source input closes at the frozen body end and effects run for the explicitly requested output tail. Reported Track/Master latency is compensated independently of creative delay. Version-three events use the producer's post-priming source clock; replay translates them to the same internal processing position using the frozen pipeline latency. Historical stock event timing is unchanged. Accurate take audition always rebuilds from frame zero, never the Live seek approximation.

Before final Ready, the checkpoint worker rechecks source content and plugin bundle fingerprints and persists any verification failure as Incomplete. Callback device first-fault/commit gating remains authoritative. Hashing, capture, processor construction and checkpoint/file work are outside the audio callback. A plugin failure cannot be replaced by dry audio and certified as success.

Rendering uses the same ordered graph and one authoritative raw float pass. Final interleaved/split WAVs and normalization derive from that pass. Each take receipt carries take/recipe/event/source/role/rate/body/tail provenance, a frozen owner/chain/plugin-state summary, raw-pass hash and finalized WAV hash. Readiness resolution verifies frozen provenance and finalized existing output context before enabling delivery/print-back. Mini waveforms continue to depict original sources before inserts. See [the plugin-take report](PLUGIN_TAKES_REPORT.md) for executed evidence and remaining qualifications.
