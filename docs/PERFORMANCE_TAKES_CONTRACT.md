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
