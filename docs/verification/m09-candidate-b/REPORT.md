# SoundPlus Candidate B

The reviewed application was `63c29e483e949e31782df529fce3e7d415a46e61`. The only product change for Candidate B is identifying the source encoding in Generate SFX. A 24-bit local WAV now shows either `WAV (decoded from MP3)` or `WAV (source WAV)`. The provider payload, journal schema, source hash, decoder and WAV writer use their existing paths.

## Native Generate SFX

On Apple Silicon macOS 26.3.2, the native panel was exercised over a 1040 × 680 content window and over the desktop-size window available on the host. The profile used 33 synthetic journal entries. Keyboard navigation preserved access to long descriptions and their individual recovery states. Entering a description enabled Generate. Preview switched to Stop and returned to Preview. Publishing a verified synthetic candidate revealed the three destination actions, and Insert in Edit created a clip from the published WAV. The desktop profile had empty generation history. [MP3](evidence/native-mp3.jpg) · [Provider WAV](evidence/native-source-wav.jpg) · [Keyboard focus](evidence/native-prompt-focus.jpg) · [Published result](evidence/native-published.jpg)

I opened and collapsed the masked Keychain disclosure with a saved key present. The native startup check was existence-only. Transform Audio separately prepared a selected clip as a local 240,000-frame input and showed a missing Stability key. The focused HTTP tests use synthetic credentials, verify actual MP3 and WAV provider fixtures, exercise Save/Test/Delete and failure redaction, and cover cancellation and interrupted job recovery. Native Generate SFX with its saved key revoked, native in-flight cancellation and a new live provider submission remain open. [Disclosure](evidence/native-key-disclosure.jpg) · [Failed history](evidence/native-failed.jpg) · [Interrupted history](evidence/native-needs-review.jpg) · [Transform](evidence/native-transform.jpg)

## Release launcher

The separate normal-stack launch of the Candidate B multichannel test exited with SIGSEGV at 8,372,224 bytes. The symbol and disassembly place the failure at the test's stack guard. The official run inherited the host hard limit of 67,092,480 bytes and passed that case. This is test-runner qualification. [Normal-stack log](evidence/normal-stack.log) · [Diagnostic](evidence/stack-fault.json) · [Stack frame](evidence/stack-frame.txt)

## Historical acceptance

Candidate A and its acceptance receipts remain in their original locations. Across its two runs it has 23 Pass, 1 Partial and 375 Not Run. M08-12 remains Partial on A. NATURAL DISASTER scored 0.4443265 as uncalibrated cosine similarity for a recording annotated as sea waves. The product owner's listening judgment is on file and independent acoustic adjudication remains outstanding. [Candidate A report](../m09-phase-1b/REPORT.md)

## Freeze and Phase 2 decision

**No-Go.** The official freeze ran from clean detached worktrees at source `6510c4eb914f19f5a49dc2ab0b1d2eda2e9747e7`, tree `5d37072c13217ec44a49a11eb37f93a984af4458`. Its manifest and the new acceptance run depend on an unfiltered passing gate. Each attempt built the arm64 bundle with the recorded pinned dependencies. The official inventory check identified the source stamp, helpers, resources and file modes. The [development inventories](evidence/development-artifacts.json) identify these checkouts and test executables.

| Attempt | Full gate failure | Follow-up |
| --- | --- | --- |
| 1 | `surround_xy_device` failed at Stop while retaining paired motion. | Five focused reruns passed with the QA app closed. The second and third full runs passed the same case. |
| 2 | `plugin_live` failed an Undo history assertion at capacity. | The focused suite passed. Its diagnostic comparison shows native sequence 5 on the first value of a new gesture during the failure and sequence 6 at the same point on the passing run. The timing-sensitive failure remains open. |
| 3 | `hosted_instrument_automation_au` reported a closed live insert owner before output certification. | The unchanged focused AU rerun passed in 12.55 seconds. |

[Attempt 1](evidence/release-attempt-1.log) · [Attempt 2](evidence/release-attempt-2.log) · [Attempt 3](evidence/release-attempt-3.log) · [Focused AU rerun](evidence/attempt-3-focused-au.log) · [Surround repeats](evidence/surround-device-repeats.log) · [Focused SFX, Transform, sizing and live insert rerun](evidence/attempt-2-focused-release.log) · [Undo history trace](evidence/plugin-history-trace.json)

The first two executable builds differed in the C-string section. The only differing printable strings were their build-clock values. The executable hashes are distinct. [Section comparison](evidence/build-comparison.json)

Resolve Studio 21.1.2.10 answered a read-only local probe. The isolated Library and role-coded WAVs were prepared. Send, physical drag, and receiver Save/Quit/Reopen require a frozen candidate before the observations can enter a Candidate B run. Live ElevenLabs spend and Stability upload, independent specialist listening, hands-on device tests, Developer ID signing, notarization, clean installation and owner release approval remain open.

All three unfiltered gates reported **63 passed and 1 failed**. Times were 506.88, 520.06 and 509.07 seconds. No approved Candidate B manifest or acceptance run exists. The installed-vendor automation test's opt-in condition was disabled and it reported `NOT_RUN` internally. This record separates fixture automation from installed-vendor acceptance.

The final profile audit matched **1,135** retained files and **78** indexed media paths across the working and default profiles. The latest working revision stayed at 4506. The audited catalog projections matched after the isolated QA work. The original Resolve project was accessed through a read-only probe. [Preservation](evidence/preservation.json) · [Candidate A revalidation](evidence/candidate-a-recheck.log)

The later dedicated Play/Stop control was implemented and verified on a separate follow-up branch. It is available at `0a7f45f01887788662fae0af182673684694885f`. [Playback report](https://github.com/Tekie79/Sound-Plus/blob/0a7f45f01887788662fae0af182673684694885f/docs/verification/sfx-panel/PLAYBACK.md)
