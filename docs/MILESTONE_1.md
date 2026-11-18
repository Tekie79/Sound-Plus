# Library and audition milestone

Historical first-milestone report. The result cap, serial source scanning and reconciliation behavior described below are superseded by [the library performance milestone](LIBRARY_PERFORMANCE.md).

This implementation follows the repository's `Sound_Design_Workstation_Architecture_and_Requirements.docx`, version 1.0, 17 November 2026, with the user's narrower first-milestone scope. It implements a general-purpose global library and whole-file audition. The specification's complete M1 gate, hybrid retrieval and region audition remain future work.

## Implemented architecture

The native JUCE Library window owns no audio callback and performs no database queries. A serial index worker owns the writable SQLite connection. A separate search worker owns a read-only connection. WAL allows committed filename batches to become searchable while discovery and metadata jobs continue. Queries carry monotonically increasing request IDs; a stale response cannot replace current results. Background refresh retains selection by asset ID and restores the viewport position.

The schema uses transactional, versioned creation, foreign keys, unique root/location constraints, and UUID asset IDs. A file's root UUID and relative path identify its location. Size, nanosecond modification time and inode detect revisions. A unique inode whose old location disappeared can retain its asset UUID through a rename inside a root. Hard links and same-name files at existing locations remain separate records. Header publication rechecks the source stamp and revision. Source files are opened for reading and are never moved, copied, tagged or rewritten by indexing.

Discovery commits its first eligible filename immediately, then batches up to 32 files or 100 ms. Headers are a later stage. Unsupported, empty and corrupt recordings remain visible with a failure reason. Rescan retries failed headers. Hidden directories, app bundles, `__MACOSX`, and the active catalog directory are excluded. Symlinks within roots are not followed, preventing cycles. Root selection canonicalizes parent symlinks.

macOS FSEvents is registered before enumeration. The watcher observes the selected roots without opening all their ancestors. Notifications are coalesced; audio files must settle for 750 ms before metadata extraction, and stamps are checked again afterward. Every two seconds the worker checks source availability. Every 30 seconds it reconciles directories to cover dropped events, sleep and mounts without reliable notifications. Restart performs filesystem enumeration/stat checks but reuses unchanged metadata and FTS records. It does not decode unchanged audio again. Interrupted discovery resumes by reconciling committed locations, rather than serializing directory-iterator offsets.

Roots retain a volume UUID and a volume-relative location. Reconnection checks the volume identity before trusting the old mount path, and searches mounted filesystems if the path changes. Missing roots and missing files remain searchable and show an explicit Offline status. Relocation never escapes a volume using an unconfirmed parent-directory path.

The bundled `Contents/Helpers/SoundPlusAudio` process owns Core Audio. The desktop supervises it with a private inherited Unix socketpair, protocol version 1, bounded JSON messages, a five-second heartbeat timeout and three automatic restart attempts. No port is opened. A process restart stops audition and retains the catalog; Reconnect Audio allows another attempt. The helper is a background process and does not own a Library window.

The audio helper decodes and resamples off the callback into a fixed stereo FIFO (131,072 frames, about 1 MiB of PCM). The callback clears output, acquires a protected stream pointer, copies prepared PCM, smooths monitor gain, and publishes lock-free counters. It allocates nothing, takes no mutex, and performs no file, database or IPC operations. A hazard pointer allows stream reclamation outside the callback. Audio position and callback frame counts use integer frames; background resampling currently uses linear interpolation. Mono is duplicated to stereo; recordings with more channels audition their first two channels. The Library reports the original channel count.

## Requirement coverage

Every ID below is covered only to the stated extent. Most specification IDs contain more than this milestone requests.

| Requirement IDs | Working behavior in this milestone | Remaining portion of the specification |
| --- | --- | --- |
| LIB-01 | Add multiple existing folders; nested files; root counts, scan state and availability; original files remain in place | Configurable exclusion patterns, export-inbox handling and mounted-share qualification |
| LIB-02 | Watch before enumeration; searchable filename batches before headers; discovered, ready and failed counts | Waveform/semantic stages, visible-item priority and analyzed coverage |
| LIB-03 | WAV/BWF container, AIFF and FLAC technical headers; filename/path Unicode; duration, channels, rate; per-item failures | BWF/iXML descriptive fields, UCS mapping, provenance and local overrides |
| LIB-04 | FSEvents, settled-file checks, reconciliation, durable committed batches, zero unchanged header jobs on reopen, remount recovery | Durable iterator checkpoints and broad network-filesystem qualification |
| LIB-05, DATA-01 | Distinct UUIDs for same-name assets, source revisions, confirmed within-root inode renames, offline retention, UUID volume relink | Verified byte deduplication, content hashes, multiple locations per asset, cross-root/copy relink review |
| LIB-07 | Index existing local libraries in place without a DAW or proprietary integration | Explicit proprietary interchange and cloud-transfer workflows |
| DATA-02, DATA-03 | Read-only original-media access, transactional schema version, foreign keys, source-stamp/revision checks | User metadata overlays, historical derivative validation, backup/migration recovery beyond schema v1 |
| SRCH-01 | One field; filename/folder tokens, prefix terms, quoted phrases, exact filename promotion and asset UUID lookup; source filter | Semantic descriptions, tags, UCS, structured metadata filters and arbitrary infix matching |
| SRCH-02 | SQLite FTS5; independent reader; request generations; available during import | Concurrent semantic retrieval |
| UX-01, UX-02 | Folder-first use without setup; search, Down/Up result navigation, Space play/stop, Command-F, output selection, named controls and accessible row metadata | Open-file/drop flow, region editing and later creative/export keyboard commands |
| UX-03, UX-04 | Units, text Offline/Unreadable state, listening volume, pinned audition, retained selection/scroll; no original-file deletion action | Waveforms, design feedback and undoable organization |
| DSP-03, DSP-04 | Separate audio process, decode-ahead FIFO, bounded callback, explicit sample-rate conversion and monitor gain | Layer graphs, scheduled performances, latency compensation and DSP qualification |
| OPS-01, OPS-02 | Local catalog/search/audition; no runtime cloud calls; no DAW/account/model requirement | Later offline design/render/plugin capabilities and provider settings |
| Q01, Q02, Q09 | Partial scenarios for identity/rename/remount, lexical search during import, interrupted import, WAL rollback and reopen | Tags/designs, full 100k corpus/relevance gates, migration/index-generation/render fault matrix |

OPS-05/OPS-06 distribution qualification is not complete. OPS-07's full hardware/rate and 30-minute workload is not complete. AI embeddings, Designer, effects/plugin hosting, rendering/export, native file drag, region audition, Spotting and Resolve placement are intentionally outside this milestone.

## Verification environment and evidence

Executed on 17 November 2026 on a physical **Apple M5 Pro MacBook Pro, 48 GB RAM, macOS 26.3.2 (25D2150)**. The Release executable and bundled audio helper target arm64 with a macOS 12.0 deployment minimum, built with Apple Clang 21.0.0 and SDK 26.5. Older OS releases are compilation targets, not hardware-qualified configurations.

The generated-fixture CTest exercises real SQLite FTS5, real background services, real macOS FSEvents, and the bundled helper using **MacBook Pro Speakers at 48 kHz**. It checks:

- Searchable names before header completion; WAV, AIFF and FLAC header extraction; Unicode and escaped queries; request-generation freshness.
- Same-name/different-content asset IDs; confirmed rename ID retention; changed-file revision and metadata replacement.
- Live file additions; source disappearance and restoration; searchable offline entries; persisted reopen with zero unchanged header jobs.
- A 2,400-file nested import searched during active enumeration, interrupted after partial commits, and resumed to exactly 2,400 records.
- Abrupt writer exit during a transaction, with rollback to committed WAL state.
- Generated mono/stereo PCM at 44.1/48/96 kHz, exact resampled frame counts and nonzero sample energy; callback-side reads do not invoke the decoder.
- Live Core Audio callback progress and non-silent PCM during 100 alternating searches; no additional FIFO starvation in that interval; stop and restart into a new helper generation.

The run logs are generated at `build/Testing/Temporary/LastTest.log`. These are small-fixture functional tests, not certification of PERF-01/02/03 targets. FIFO starvation counts are not a measurement of all hardware/driver underruns.

The final packaged-helper run passed **2,805 checks** in **11.41 seconds**, including fixture construction. It observed progressive search at **577 of 2,400 files** and non-silent live callback output while queries changed. A 200-query microbenchmark on the small functional catalog averaged **0.062 ms per SQLite query**; this excludes UI dispatch/painting and is not a 100,000-file latency result. Artifact hashes and portable evidence are in [the verification record](verification/macos-milestone-1.json).

The separate volume test mounts a disposable **HFS+ disk image**, imports a generated recording, detaches the volume, checks that its records remain visible, and remounts at a different directory. It verifies the volume UUID, new source path, unchanged asset ID and revision. Its result is written to `build/volume-test/volume-result.json`. It never ejects a user drive. A disk-image remount is real macOS mount handling, but does not qualify a physical USB/Thunderbolt drive, an unreliable cable, SMB/NFS, or every external filesystem.

Native app verification uses the macOS folder chooser and accessibility tree to inspect the Library controls and results, metadata, pinned audition, search and restart counters. Audio evidence confirms PCM reached Core Audio; human listening quality, VoiceOver usability, external interfaces, 30-minute playback, large-library performance, thermal behavior and older macOS releases remain unqualified.

The final native picker imported a second source and displayed **19 indexed / 18 ready / 1 unreadable** across two generated roots. Reopening the existing 12-file demo reported **0 headers read this session**. Changing the visible query from `soft mechanical` to `neutral` replaced the result rows while the original 30-second recording kept advancing in the pinned transport. Space audition was observed during the initial native pass; the final automation session did not reliably deliver raw keyboard events, so the five-minute test should also confirm physical keyboard behavior on the user's setup.

## Practical limits

Search paints at most 1,000 results and displays `1000+` when capped; narrow the query or choose a source for larger libraries. Prefix/token search is implemented, rather than semantic or arbitrary substring retrieval. All roots use one serial worker; a filesystem call stalled by an unavailable network mount can delay later index work, while committed search and the audio process remain independent. There is no pause UI, custom retry scheduler, collection editor or catalog removal control yet. Monitor volume and selected output are session settings. The source stamp is a fast revision heuristic, not a cryptographic content identity.

Build/launch commands, the five-minute own-library test, and fixture commands are in the [README](../README.md).
