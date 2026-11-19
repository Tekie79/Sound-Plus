# Codex implementation prompt — Performance Mixer v1.3: Recorded Perform Takes v1

Implement this milestone in `Tekie79/Sound-Plus`. Complete the working feature, relevant verification, documentation and local commits. This is a request for implementation, not another proposal.

## 1. Read and preserve the current application

Read `AGENTS.md`, the full supplied review, `docs/PERFORMANCE_MIXER_V1_2.md`, `docs/PERFORMANCE_TAKES_NEXT.md`, and the existing session, performance, live audio and render code. The reviewed baseline is `0cfa5f6cc303af4abf058513430fe74fe1559134`. If HEAD has advanced, inspect those changes and adapt; preserve newer work.

Show and maintain the repository's monochrome Tasks in progress checklist in chat and `docs/`. Protect the latest actual working document and durable outputs before rebuilding. Use separate QA catalogs/documents and generated fixtures. The report's Rain glass r9054 is historical evidence, not permission to replace a newer design. Close processes running from the app bundle before rebuilding that bundle.

Preserve JUCE/C++, the shared Library/Designer/session/audio architecture, Lucide/theme, editable fades, fixed 10-point corner fade handles, selection-follow audition, source-card waveforms and existing durable output workflow. Parsing, allocation, graph preparation, file/database I/O and process management stay outside the real-time callback.

## 2. First commit: two focused reliability corrections

### R3: distinguish user selection from programmatic selection restoration

At the reviewed baseline, `LibraryView::timerCallback` restores a retained ID and calls `selectedRowsChanged`; that handler pins the new request. On explicit Refresh, the fast lexical prefix can therefore pin before delayed semantic results arrive, causing Refresh to offer updated results again instead of adopting them.

Separate selection/detail restoration from an explicit user pin. Preserve selected asset and independent audition identity where valid. An explicit Refresh/new query must be able to adopt its semantic ordering; a deliberate row click or keyboard selection should still pin the appropriate current request. Preserve request/page generation protection and deletion behavior.

Add a controlled regression through the actual Refresh handler: AI enabled, selected ID retained in the lexical prefix, lexical publication before delayed semantics, then semantic completion. Assert ranking adoption without audio retargeting, followed by normal deliberate selection pinning. Cover the equivalent typed-query case. Do not satisfy this only with an empty query, a selection already cleared, or a service-only test bypassing view restoration.

### R4: service overdue waveform validation under sustained high demand

`WaveformPool::tick` chooses validation only when `missing` is empty. With the existing 80 row/layer sources plus selected/card demand and a 48-source cache, retained hot entries can never be revalidated. Steady demand also causes repeated eviction/reload work. Ordinary Perform has at most 45 demands; combined Edit surfaces can exceed the limit.

Add a nonstarvable overdue-validation budget/deadline and prevent pointless steady-state reload churn. Keep bounded memory, cooperative cancellation, selected priority, stale-generation rejection and eventual background service. Avoid using a larger unbounded cache or additional unbounded workers as the fix.

Extend the pressure test beyond first completion: keep the demand set unchanged, disconnect/change a retained selected/card source, advance the injected clock, verify bounded service to Offline/Changed, reconnect and recover. Verify useful progress and stable resource/work counts after demand settles. Define scheduler service bounds separately from uninterruptible OS I/O.

Land these corrections with their targeted tests, then continue to the recording feature. Do not turn this into another indefinite reliability-only milestone.

## 3. Deliver the complete recording workflow

Record → perform with XY and four macros → Stop → Audition → Keep/name → Render Take → drag the finalized WAV or print it back as an ordinary layer.

A snapshot remains a static recipe. A take is a frozen starting recipe plus an applied control timeline. A rendered WAV is a separate durable output with provenance. These must remain distinguishable in the model and interface.

For this first slice, Record starts from design frame zero with a freshly prepared, defined effect-history state. Use the existing mouse/keyboard XY and four macros in stereo. Freeze source assignments, trims, mappings, blend law, processing chain, source references and project rate for the active capture. Browsing/searching remain responsive. Additional sampler triggers, MIDI, plug-in hosting, surround, loop/overdub recording and picture spotting are later milestones.

Implement explicit Idle, Arming, Recording, Finalizing, Ready and Incomplete states, or an equivalent documented state machine. Arming must complete preparation before the recording clock starts. Record owns Designer transport until completion/cancel; competing audition, seek, document change, device/rate change and graph replacement require clear safe handling. No silent transport takeover or capture into an unrelated generation.

Normal Stop finalizes one take at an authoritative frame boundary. EOF behavior must be defined. Cancel abandons the draft without leaving a partial take labeled Ready. Helper loss, unavailable sources or event overflow leave an explicit recoverable Incomplete result. Preserve the user's editable recipe and existing outputs.

## 4. Record what the engine actually applies

Trace coalescing, preparation generations, producer read-ahead and callback consumption before choosing the capture point. Capture typed normalized control changes at the boundary where they actually affect produced audio, with an integer design-frame timestamp, take/session/play generation and monotonic event sequence. Record the XY pair atomically and include the four macros, enable/bypass transitions and effective reset values. Specify initial values, same-frame ordering and smoothing semantics.

UI mouse timestamps, dispatch logs and the new `lastCommandMs`/`lastCommandSequence` health fields are not the capture clock. Coalesced or discarded commands that never affect accepted audio must not become take events. Tentatively rendered future audio may be discarded; reconcile events with the accepted playback generation and final captured end frame. Do not retain events past Stop simply because the producer ran ahead.

Use a bounded preallocated event path and move serialization/checkpoint work to non-real-time threads. Define duration, event-count and queue limits. Queue overflow or lost sequence integrity must be detectable and make the draft Incomplete; never silently drop movement and report success. Persist recoverable draft checkpoints off-thread, with recovered prefixes clearly labeled Incomplete.

Keep recording events separate from UI undo history. Do not create an undo entry per sample/event or wrap existing non-nested gesture transactions in a way that closes recording accidentally. Adding/removing/renaming/keeping a take must have intentional undo behavior. Document how live control values settle after Stop/Cancel and keep the frozen take independent of later design edits.

## 5. Immutable take model, persistence and replay

Store stable take ID/name, frozen starting recipe, source IDs and fingerprints/revisions, blend law, stock-DSP compatibility version, initial control state, project sample rate, frame-zero/effect-history policy, ordered applied events, body end frame and completion status. Use stable control identities, not labels or UI row positions. Store event and frozen-recipe hashes for provenance.

Add explicit document/reader version gating for time-varying takes. Unsupported readers must reject that content safely. Preserve existing static document semantics and versions when no take is stored. Use atomic document writes and the established recovery path. Source media stays in place; relinking must verify identity/revision and must not silently substitute different content into a kept take.

Replay the immutable timeline through the existing shared performance resolver and stock DSP. The event frame, ordering, bypass and smoothing behavior must be shared by live replay and offline render. Do not reapply UI coalescing during replay. Handle supported project/output rate conversion without cumulative timing drift.

Use a defined body end frame and render the effect tail once. Accurate audition/render rebuilds complete effect history from the take origin. Any approximate live seek must retain its existing explicit limitation; it cannot be used as exact-render evidence. Monitor volume remains excluded from rendered audio.

Changing the live design or recalling a different snapshot must not alter an existing take's playback/render. During take audition, display its moving controls as playback state without writing them into the live recipe. Repeated play/render uses the same frozen data.

## 6. Keep the interface simple, modern and usable

Retain Edit for detailed timeline work and Perform for the four waveform cards, XY and macros. Add a compact contextual take strip/list using current layout/theme/icons. Clearly identify the active context: Live Design or Take: name. Show recording time/status and provide Record, Stop, Audition, Keep/Rename and Render Take. Drag becomes ready only after the permanent WAV exists.

Keep the common flow visible; advanced details can use disclosure. Avoid a new full DAW automation editor in this slice. Use clear labels/tooltips and accessible names/states, including an unambiguous Incomplete state. Preserve usable XY space, readable filenames, four source waveforms and macro controls at normal and minimum supported window sizes. Reflow the take UI instead of compressing the pad further.

The shared transport and render actions must resolve the selected context correctly. A selected take must never silently audition/render the current static design. Render receipts identify output kind, take ID, recipe/event hashes, processing version, sources, rate, frame count/tail policy and final WAV hash. Use the existing durable location and ordinary referenced-layer print-back path.

## 7. Verification that proves the feature

Add meaningful tests through the recording path, not only hand-authored replay fixtures:

1. Capture XY and all four macros, bypass/enable/reset, multiple same-frame changes and a block-boundary event. Include coalesced/stale inputs and producer read-ahead; verify only actually applied accepted events survive, in order and before the final end frame.
2. Compare captured pre-monitor audio, replay and offline render for the same take/configuration using a declared numerical tolerance and measured differences. Comparing two replay paths alone does not prove the original performance was captured. Add impulse/delay/reverb history and Stop/tail cases.
3. Verify supported project rates and at least one differing output-rate configuration for timing drift and event placement. Report exactly which configurations are qualified.
4. Save/reopen takes, change the live recipe, recall a snapshot and confirm the kept take still replays/renders its frozen recipe. Exercise rename/keep/delete/Undo and reader-version rejection.
5. Test Cancel, zero/very short capture, EOF, document/transport changes, pending generation adoption, missing/relinked sources, helper loss, queue overflow, interrupted draft recovery and write failure. None may silently produce a falsely complete take or overwrite an existing output.
6. Exercise recording while Library search, indexing and waveform work continue. Measure event backlog, bounded memory and audio continuity. Preserve R3/R4 regressions and all existing functional gates. Run a focused sustained recording qualification appropriate to the configured capture limit; repeat broader benchmarks only if the changed path creates a concrete remaining risk.
7. On the supported Mac, complete the actual Record/perform/Stop/Audition/name/save/reopen/render/print-back workflow and inspect normal/minimum layouts. Include diverse waveform shapes in screenshots. Keep human listening, native pointer/keyboard checks and numerical PCM checks explicitly separate.

If touching helper recovery, strengthen the restart-notice test to wait for the new helper PID and a subsequent healthy idle heartbeat before checking notice persistence. Preserve the historical restart as cause unresolved unless new evidence identifies it.

Cross-application delivery remains separately qualified: actual receiver import/drop, track placement, save, exit SoundPlus and reopen the receiving document. If the available automation still cannot cross application boundaries, report that precise blocker and retain a short manual check. Drag initiation alone is not receiving-app acceptance.

## 8. Finish with reviewable evidence and commits

Run the applicable native Release build and required regression gates. Record actual commands, configuration, frame/event counts, PCM comparison tolerances/results, capture limits, evidence paths and failed/blocked checks. Do not claim native or listening passes when the environment cannot provide them.

Update implementation documentation, the take data/clock/history contract and the checklist. Deliver at least one named saved take and corresponding generated-fixture render/receipt as reproducible QA evidence. Preserve/reopen the latest user session and audit protected files.

Finish with a corrective commit and coherent feature commit(s), reporting their identifiers. Summarize what works, what was tested, any limits, and the next genuine remaining feature. The completed result must be a usable recorded-take workflow, not only a Record button or another design brief.
