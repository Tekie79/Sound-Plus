# M09 Phase 2 operator plan

Use the manifest and acceptance run created by qualification. Verify the digest in the exact candidate checkout before opening the app. Start each journey with a fresh QA profile and generated source media. Attach new results using the acceptance validator. The approved development candidate is the test artifact for these journeys.

## Provider budget and cancellation

- Keep spend at zero until the account operator approves both a provider-credit cap and a cash-equivalent cap for their account and records the planned requests. Check the account balance and provider limits immediately before execution.
- Cap the initial run at one ElevenLabs submission for one two-second candidate and one Stability submission using a generated two-second WAV. Give cancellation its own one-request allowance per provider and explicit funding. Stop if either cap would be exceeded. Require approval for every new submission after those allowances.
- Exercise missing credentials, disclosure and local rejection before sending any requests. Retain sanitized account readings, timestamp, provider request ID, model and requested versus completed candidate count. Record failed or cancelled submissions, state whether the vendor acknowledged cancellation, and check the provider account for actual charges. Observe ambiguous outcomes without automatic retry.
- Compare decoded local WAV frames, rate, channels and SHA-256 with the published copy. Verify that preview, Pause, Resume, Save to Library, insertion in Edit and assignment in Perform create no new provider request. Use synthetic media with no private content.

## Resolve receiving workflow

- Duplicate or create a throwaway Resolve project and verify its name, active timeline, timecode rate and target audio track before sending audio. Preserve the original project as-is.
- Cover Library full source and range, Edit clip/track/stem/mix, Perform pad/take, and rendered exports where supported. Use role-coded stereo, 5.1 Side and 5.1 Rear fixtures with exact known frames and masks. Exercise the registry's 24, 25, 29.97 DF and 30 fps cases.
- For direct Send, check Media Pool identity, copy or reference path, placement at playhead, track, duration and channel roles. For physical drag, exercise a real pointer drop, record the receiving app's acceptance and validate the same properties.
- Save, quit and reopen Resolve. Reopen the throwaway project and verify that the imported timeline material is online and correctly placed. Record successful drag initiation separately from successful receiver persistence.

Independent physical 5.1, MIDI/controller and microphone checks, specialist listening, installed vendor acceptance, signed installation and release authorization have their own evidence gates.
