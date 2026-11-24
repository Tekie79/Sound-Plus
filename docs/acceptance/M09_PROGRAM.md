# SoundPlus integrated acceptance program

**Development source** `a0e26ff835ad330e49c6f93fe1e4c9aff889f055` on `feature/m08-library-metadata`. Select and freeze the actual install candidate before opening an acceptance run. The development app is an isolated arm64 Release build. Distribution remains **No-Go** pending the release gates and owner decision.

## Coverage and triage

The [case registry](m09-cases.json) has 399 distinct IDs with procedures, expected results, priorities, methods and responsible roles. Each new run begins with 399 **Not Run** results.

| Family | Cases | Evidence to retain |
|---|---:|---|
| M00–M09 | 153 | Specified development, integration, physical and release oracles |
| Mixer | 213 | Mixer controls, bus routing, sends, PCM, layout, PDC and stress |
| Transfer | 26 | Exact source, half-open selection, copy receipt, external drag and receiver persistence |
| Integrated journeys | 7 | Newly composed workflows linking generation, editing, performance, portable media, large catalogs and release recovery |

The [original M00 record](../verification/m00/case-results.json) contains 251 reviewed IDs across the first three families. It records 65 passes and 186 unfinished cases. The open Mixer cases comprise 145 Not Run, 13 Partial and five Blocked. Transfer has 15 Partial and three Blocked. The 12 milestone cases have four Partial and one Blocked. Use each historical ID to find its actual observations, blockers and evidence. Every ID requires a new result for the candidate under qualification.

Start with the unfinished P0/P1 cases and the seven `JRN` cases. Execute relevant narrower cases individually even when a journey passes. Historical reports and development fixtures help diagnose failures. They carry their original dates and identities.

## Prepare a run

1. Assign a release owner, DSP/Library QA, a provider account operator, a Resolve operator and a physical-device/listening operator. Record which person performed each actual check. Keep tokens, signing credentials, personal media and unsanitized vendor receipts outside Git.
2. Freeze the commit, clean working tree, SDK, JUCE and dependency revisions. Record the candidate and nested-helper hashes, signing identities, entitlements and installer SHA-256. Keep both build logs and an identical post-test manifest. Close processes belonging to a bundle before rebuilding it. Use an isolated checkout and bundle.
3. Use a new `--data-dir`, generated test media, throwaway receiving projects and fresh export destinations. Record SHA-256, channels, exact frames and extensible channel mask. Include stereo at 44.1/48/96 kHz and discrete role pulses for both 5.1 Side and Rear. For migration and recovery use consented copies. Capture the newest working revision, catalog references, autosaves, journals, source/IR hashes, take IDs/recipe hashes and finalized render hashes before/after.
4. Create a clean detached worktree at the chosen full SHA. Bootstrap the pinned dependencies in that worktree, or clone the verified vendor directories locally. The freeze command requires a fresh build directory and includes all helpers, plugin fixtures and the ASan target. It records the Git tree, dependency contents, code signatures, resource hashes, commands and both logs. A failed build or gate produces no qualified manifest. Keep failed attempts for diagnosis. Start subsequent attempts in a fresh checkout.
5. Use the manifest digest printed by the freeze and retain it independently with the acceptance owner. Use the same checkout, executable and digest on every check.

```sh
cd "$candidate_checkout"
commit="$(git rev-parse HEAD)"
python3 scripts/m09_candidate.py --commit "$commit" --tools "$tools_directory"
# Copy the printed manifestSha256 into manifest_sha.
mkdir "$run_directory"
python3 scripts/m09_acceptance.py --new-run "$run_directory/run.json" --commit "$commit" --manifest-sha256 "$manifest_sha"
python3 scripts/m09_acceptance.py --check-run "$run_directory/run.json" --commit "$commit" --manifest-sha256 "$manifest_sha"
```

Keep `run.identity.json` with its run and retain the original in source control or the QA evidence archive. Store sanitized attachments in that run's `evidence/` directory. To obtain an attachment record, add `--evidence evidence/FILE` to the check command **before** entering the new observation. Paste the resulting object into the case's evidence list. It includes the observed byte count and digest and identifies the run and candidate. UTC observations must fall between initialization and the time of validation. A Blocked entry describes the missing prerequisite. A Partial entry describes exercised and outstanding steps. A Not Applicable entry requires the owner's written scope decision. Validators establish file identity. The named release owner judges evidence sufficiency and approves any release.

**M08-12 procedure supplement v1.1.** Use a locally indexed, licensed recording with an empty category. Request acoustic suggestions, preview the matched interval, reject one and reopen, then undo. Apply one, reopen and verify the overlay and unchanged source hash. Repeat on ambiguous, missing-model and offline examples. Retain scores, ranking, expert listening observations and data identity separately. This elaborates an existing case and preserves all 399 registry IDs.

## Execution order

| Gate | Exercise | Required proof |
|---|---|---|
| Candidate integrity | Clean bootstrap, pinned build, unchanged unfiltered CTest, saved-state compatibility | Commit, manifest before and after, CTest output, session and source audit |
| Vendor and privacy | Missing/restricted Keychain entries, explicit credit or upload consent, real v3 generation and authorized transformation, cancellation, rejected charge and ambiguous response where observable | Sanitized provider receipt with request ID, billed candidates or verified lack of submission, local job transitions, canonical WAV hash. A loopback server qualifies only development behavior |
| Signals and routing | All creative effects, one automation lane, hidden curves, Perform triggers, MIDI, capture, tails, PDC and multichannel routing | Independent decoded samples and masks, timing per channel, frozen identity, deterministic replay, hardware and listening receipts recorded separately |
| Catalog and portability | Large Library queries during audition, offline volumes, smart searches, revision conflicts, unknown tags, collect and open on a second machine | Source and IR hashes, catalog IDs, peak resources, latency percentiles, operator observations, missing dependency and recovery behavior |
| Real receiver | Library source and range, track, stem, mix, pad and frozen take as specified. Test Send and physical Drag separately in a throwaway Resolve Studio project at 24/25/29.97 DF/30 fps | Record source path, copy/reference, Media Pool ID, clip identity, chosen audio track, playhead, duration, rate and role count. Save, quit SoundPlus, close and reopen the receiving project and confirm online use. Separate drag initiation, rejected drop and lost persistence |
| Device and endurance | Stereo headphones, calibrated six-output hardware, MIDI and microphone disconnect/reconnect, representative long session, helper crash and full-volume behavior | Device, driver, sample rate, block size, role callouts, observed latency/dropouts, operator and elapsed time. Use a disposable test volume for exhaustion |
| Distribution | Approved AGPLv3 strategy, corresponding source and notices, authorized Developer ID signing and hardened helpers, notarization, stapling, quarantined install/upgrade/rollback | Legal/package review, authorization, notarization receipt, Gatekeeper check on clean Mac/account, adopter feedback and owner sign-off |

**Release decision.** Keep the gate open for any unqualified supported claim, failed P0/P1, missing distribution credentials or unverified clean install. Record explicit owner approval for a narrower beta or a P2 exception. Neither the registry validator nor a passing development build grants release approval.

## Development references

[Generation](../verification/m01/REPORT.md) · [Transforms](../verification/m02/REPORT.md) · [Event refinement](../verification/m03/REPORT.md) · [Creative effects](../verification/m04/REPORT.md) · [Automation](../verification/m05/REPORT.md) · [Live sources](../verification/m06/REPORT.md) · [Portable projects](../verification/m07/REPORT.md) · [Catalog metadata](../verification/m08/REPORT.md) · [Previous production gate](../PRODUCTION_BETA_GATE_2_REPORT.md)
