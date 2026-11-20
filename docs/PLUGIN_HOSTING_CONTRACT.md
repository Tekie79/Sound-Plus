# Isolated insert foundations

This extends the finite Process workflow; it does not replace its raw/presentation/delivery authority or the R20–R25 recovery policies. Existing stock document/take canonical fields remain unchanged when no external inserts are present.

## Scope and incomplete requirements

- VST3: isolated persistent rack processing is implemented on macOS arm64.
- Audio Units: system component/extension registration and the JUCE asynchronous creation path are implemented. Apple AUDelay (AUv2) has production PCM/state tests. AUv3 is not runtime-qualified here. Components whose bundles cannot be resolved for fingerprint/architecture checking remain unavailable.
- VST2.x remains a **required, blocked format**. The pinned checkout has no VST2 SDK headers or evidence of an applicable distribution agreement. No unauthorized SDK has been fetched; a VST3 license does not authorize a VST2 host. No VST/VST2 badge or substitute adapter is advertised.
- Native editors currently use a separate isolated state-editing instance and apply on close. Generic parameters address the persistent audible chain. This is not a claim that native editors edit the live DSP instance.
- Plugin-aware take capture/events are not implemented. Record is disabled for a mix containing external inserts; legacy stock takes remain available and unchanged. Expanded Perform remains a following milestone, not decorative controls in this build.

## Discovery and identity

VST3 discovery reconciles `/Library/Audio/Plug-Ins/VST3`, the user's `Library/Audio/Plug-Ins/VST3` and saved custom roots. AU discovery uses registered component identifiers, including asynchronous extensions; bundle lookup checks standard component locations and app extension bundles in standard Applications locations. Discovery/instantiation stays inside supervised helpers. A resolved bundle fingerprint and native arm64 architecture are required. Names never select a substitute format/class.

The shared registry is `plugin-processing/plugins-v1.json` in the catalog data directory. Complete snapshots publish atomically; cancellation preserves prior entries/roots and never becomes quarantine. Bundle failures and class-only failures retain explicit targeted Retry. Registry validity includes probing-policy version 2; an unchanged class rejection is re-probed on explicit Retry. Failed and incompatible candidates remain searchable in the insert chooser with status, requested/supported layout, failure stage, last attempt and retained diagnostic/log paths. Effect groups follow reported categories, with Other effects for unclassified processors. AUv2/AUv3 labels identify the AudioUnit adapter's component kind; they are not invented extra backends.

## Ownership, persistence and order

A track's stock rack is owned by its stable layer ID; the stock master rack by the document/variation/master tuple. External racks and slots also have explicit stable IDs. Owner commands include document incarnation, document/variation and layer/slot identity. Popup/editor completions never use whatever track happens to be selected later. New copies use independent instance state; external duplicates receive new rack/slot identities. Opaque settings are immutable serialized values, separate from `StockEffect`'s fixed numeric array.

External-containing documents use manifest 8 / `hosted-racks-v1`. Owner routing defines the actual stage bus: source interpretation for layout-aware tracks, legacy stereo for the legacy path, and output layout for Master. The plugin's saved description is not authority to reinterpret a source by channel count.

Signal order is explicit:

1. Legacy stereo: source edits → fades/gain/pan → stock track inserts → external legacy-stereo inserts → sum.
2. Layout-aware: source edits/fades/gain → stock track inserts → external source-channel inserts → spatial/matrix routing and explicit filtered LFE send → sum.
3. Sum → stock master inserts → external master bus inserts → master gain.

Reorder is supported within each stock/external stage. Arbitrary interleaving across those stages is not implemented. Mono before a surround panner remains Mono; a true surround external insertion needs a real matching source or master bus. Exact ordered layouts are negotiated; incompatible buses fail with a reason, without automatic downmix, front-pair or multi-mono substitution. Optional MIDI capability does not disqualify an audio effect: input stays empty and unused MIDI output is discarded after every slot/block. Actual instruments and MIDI-only processors remain unsupported. Declared auxiliary buses are retained and disabled, subject to the processor accepting that topology; required auxiliaries fail explicitly. MIDI/sidechain routing and Intel/32-bit bridges remain unsupported.

## Persistent service and real-time boundary

Each populated external rack has a retained processing service; workers are not spawned per block. The existing `SoundPlusAudio` remains the sole SoundPlus device owner. Plugin code never loads into the main UI or device process. Preparation, state restoration, bus negotiation and buffers are prepared off the device callback. Plugin configuration/processing/destruction use the isolated helper's serviced message thread, required by the pinned VST3 restart/latency notifications. A distinct native GUI helper owns editor windows. Retired hosted graphs are reclaimed on a bounded cleanup worker after callback hazard release; child shutdown/reaping does not stall the active PCM producer.

The graph producer exchanges blocks through one fixed mapped request/response slot: at most 16 channels × 512 frames and 8 × 512 parameter controls. Release/acquire atomics publish ownership. The callback only consumes its existing preallocated PCM ring and never waits for a plugin process, parses/serializes state, allocates a graph, or performs file/database IPC.

Application/audio protocol 10 keeps ordinary messages bounded to 64 KiB. Hosted control snapshots have a separate 2 MiB bound and contain owner/slot IDs, normalized values and bypass flags, **not opaque state or bundle descriptors**. State/topology changes prepare a new graph. Control snapshots reconcile into pending generations; the worker validates stable parameter identities.

Startup is cancellable with a 20-second bound; block completion has a 250-ms supervisory bound and resident memory is monitored against one GiB per service. A crash, hang, nonfinite output, changed bus/latency or invalid state stops the graph with an error; stale audio is not repeated indefinitely and no new successful render is published. This deadline is a failure bound, not a real-time throughput guarantee. Resource limits are 16 populated external racks, 64 slots total, eight per rack, 512 parameters per slot and 24 MiB serialized state per mix. Design saves retain the existing 64 MiB load envelope with a conservative 60 MiB write budget.

## Latency, history and tails

Parallel track paths align to the longest reported track-chain latency; master latency is counted once. The common startup is discarded once. Per-slot/rack bypass retains its latency through an aligned dry path while the processor history continues. Compensatable latency is bounded to one second per rack. Creative delay remains content. This synchronous producer exchange adds no queued audio block; the existing device/PCM lookahead still contributes interaction latency.

Dynamic bus/latency changes fail closed and require deliberate Stop/reprepare. Seamless automatic dynamic-latency graph replacement is not claimed. Live seek starts fresh state at its bounded pre-roll start (normally 250 ms); PDC priming does not seed an unrelated frame-zero history. Accurate audition/offline render reconstruct from frame zero. Source-end zeros continue through retained effects, subject to the user's existing finite Output tail cap (up to 30 seconds); arbitrary infinite tails are not promised.

## Output authority

Hosted rendering executes one graph pass into a retained float `Plugin Passes/<uuid>/Raw.wav` plus `pass.json`, in the explicitly selected export layout/rate. Metering, normalization, quantization and split delivery read that same pass. They do not silently execute arbitrary processors again. `DesignOutput` retains raw path/hash and graph provenance; new Preview/Render is an explicit new pass. Original source files are never overwritten.

Master output includes each track chain and the master chain once. Render Selection for a hosted mix is labeled a pre-master track stem: the selected layer resets to zero placement and excludes master inserts/gain. Legacy stock-only Render Selection behavior remains unchanged. There is no claim that separately processed stems through a nonlinear master sum to its combined master. Missing processors preserve saved positions/state and block requested processing; no silent substitute or certified bypass is used.

## Dependencies

The current upstream [VST3 licensing page](https://steinbergmedia.github.io/vst3_dev_portal/pages/VST%2B3%2BLicensing/VST3%2BLicense) describes MIT licensing starting at SDK 3.8. The **pinned** SDK in JUCE 8.0.6 contains its earlier Steinberg/GPLv3 license text; it has not been relabeled MIT. JUCE's own commercial/AGPLv3 terms also remain applicable. Steinberg separately states the [VST2 distribution-agreement restriction](https://steinbergmedia.github.io/vst3_dev_portal/pages/VST%2B3%2BLicensing/Which%2Bfiles%2Bfall%2Bunder%2Bwhich%2Blicense.html). No commercial distribution entitlement is inferred from this local build.

Apple documents [asynchronous AU extension instantiation](https://developer.apple.com/documentation/audiotoolbox/hosting-audio-unit-extensions-using-the-auv2-api). The pinned JUCE adapter invokes `AudioComponentInstantiate` with out-of-process loading for AUv3; actual AUv3 qualification remains open.

## Interactive editor supervision

Editor tasks have independent document-incarnation/variation/owner/slot cancellation tokens. Selecting or removing another slot and scanning do not cancel an existing editor. Removing the actual owner/slot or replacing its document cancels it. Healthy interactive sessions have no fixed 20-minute deadline. The supervisor requires the serviced message-thread heartbeat (30-second startup grace, 15-second stalled heartbeat), retains the existing memory/cancellation guards, and periodically checkpoints state every two seconds to `insert-work/*-editor-recovery.soundplus-insert`. Load that file with the insert's Load preset action if completion conflicts or the helper fails. Recovery covers the latest completed checkpoint, not changes made after an unresponsive plugin stops servicing the thread.

Editors remain separate apply-on-close instances. Unrelated work does not own or replace editor cancellation. Completion reports conflicts instead of silently applying stale state. Parameter identity includes the hosted stable ID while retaining legacy numeric IDs for saved-document compatibility. Large processors may declare up to 16,384 parameters; inline snapshots expose at most 512, excluding non-automatable aliases for large surfaces. Complete opaque state remains bounded to 4 MiB. No claim is made to expose every parameter on a larger surface.

## Parameter correctness and discovery recovery

Probe policy 3 rejects ambiguous native parameter IDs across the **complete** declared schema, including nonautomatable MIDI aliases. A processor with duplicate IDs remains visible with its exact identity and diagnostic; it cannot be inserted or processed through a saved job. The isolated helper's read-only `inspect-parameter-schema` operation can inspect an otherwise rejected schema without preparing or processing audio. This is general validation, not a vendor-specific renumbering or state rewrite.

New captures retain `parameterBaseline` alongside opaque state. Restoration validates index, legacy ID and native stable ID, restores program/state first, then applies only controls whose values differ from that baseline. Legacy captures without a baseline preserve their previous explicit normalized-override behavior. Persistent clients compare **requested** controls with their previous request, so unchanged controls do not overwrite correlated/native values produced by state restoration. Schema signatures include ordered index/IDs, names, automation eligibility and steps; both inline editors rebuild on same-count schema changes and ignore callbacks from destroyed/stale controls.

Rescan discovers complete configured roots and registered AUs. Bundles are opaque discovery candidates; embedded implementation bundles are not separate installations. Retry selected refreshes only its target's registration/bundle/fingerprint and probe while preserving other entries and roots. Publication remains atomic under cancellation. Successful reprobes clear current failure fields; bounded historical diagnostics remain separate. Chooser selection uses format, canonical component/bundle identity and processor class ID, with bundle-only fallback reserved for classless failure rows. Missing classes never select a sibling implicitly.
