# M01 · AI sound effects

Development started at M00 tip `1a6f53a`. In Library, Edit or Perform, choose **Generate SFX**. Save a personal key to macOS Keychain, enter a description and confirm the candidate count. Each candidate can incur a separate charge. The panel offers an account-read credential check on demand. Restricted keys may lack that permission.

A provider declares its model, duration range and supported operations. The ElevenLabs adapter explicitly sends `eleven_text_to_sound_v3`, with Auto or 0.5–30 seconds, prompt influence, loop and one to four candidates. MP3 is the default. The user can select 44.1 kHz WAV when their plan supports it. A denied format stops the batch. The confirmation says “Estimate unavailable” because the app does not have an account-specific quote. [Provider API](https://elevenlabs.io/docs/api-reference/text-to-sound-effects/convert)

A single worker owns network, Keychain, decode and job history. Each authorized candidate gets a local UUID and durable SQLite record before submission. It records options, consent, timestamps, provider request ID when available, upstream format and a SHA-256 of verified canonical 24-bit WAV. MP3 provenance survives conversion. Timeout, disconnect and interrupted submissions require review. Cancellation stops local processing and any remaining submissions. ElevenLabs may still charge a submitted request. There is no automatic paid retry.

Preview uses the existing audition helper. **Save to Library** publishes to a chosen folder using exclusive atomic rename, then registers the file with the catalog writer and chosen collection. **Show in Library** selects that source for existing Resolve and native drag actions. **Insert in Edit** uses the selected Audio Track or creates an Audio Track. **Assign to Pad** offers the four corners of the current pad and identifies replacement. The new clip and assignment share one Undo. Older project and frozen-take schema versions need no migration. M02 requires a separate explicit audio-upload consent flow and asynchronous adapter.

## Executed on the arm64 Mac

- Release CTest passed 57/57 in 505.15 seconds. [Full log](release.log)
- Real loopback HTTP tests covered explicit v3 request fields, omitted Auto duration, three independent calls, declined consent, no key, queue bounds, cancellation, 401/402/403/429/500, disconnect, absent/truncated audio and misleading MIME. The fixture killed the native process after its POST arrived. Relaunch retained Needs review and the server observed exactly one POST for that job.
- The independent Python reader compared 132,300 stereo sample values across three canonical WAVs, verified their SHA-256 and scanned the test journal and published files for the fixture secret. The separate generated MP3 fixture decoded to WAV. macOS Keychain add, replace, read and deletion ran on a unique QA account.
- The real catalog writer indexed the saved candidate. Repeated Save retained one physical WAV and the same asset ID and collection membership. Tampering caused refusal. Generated clip and corner assignment survived Undo, Redo and serialization/reopen.
- The isolated native app opened its shared generator from all three workspace tabs. It displayed setup and history with an absent key. The working SoundPlus process stayed open on its original catalog. QA used a separate app bundle, generation journal and projects.
- Executable SHA-256 `9b2d06db8a2a53492baa785f40ab28e57d8f1e35294cfbb425dba338e0ff4deb`. Test executable `c3f71d69f0d27fbf24b5dc4c2cc2cd6bf1b1bba04f3b7240187b6e2323743cf2`.

Live paid generation, account billing, human listening and another physical Resolve drop remain for M09. The panel shows the latest 500 candidate records. Direct-response v3 has no remote cancellation or job status lookup. Production connects only to ElevenLabs over TLS. The fake endpoint is injected by the test binary.

Reproduce the focused test with `.tools/cmake/data/bin/ctest --test-dir build/development-release -R '^sfx_generation$' --output-on-failure`.
