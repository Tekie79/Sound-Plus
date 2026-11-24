# M01–M08 integrated development handoff

**Development source** `a0e26ff835ad330e49c6f93fe1e4c9aff889f055`. The branches are successive, with event refinement developed before audio transformation. M00 stays at its reviewed tip. The M09 program and this report add acceptance materials on top of the M08 application source.

| Milestone | Pushed tip | Development evidence |
|---|---|---|
| M00 mixer and receiver | `1a6f53a` | [Retained qualification record](../m00/REPORT.md) |
| M01 generated sound effects | `417e062` | [v3, Keychain, independent PCM and native entry points](../m01/REPORT.md) |
| M03 fine event refinement | `dffe9e1` | [Sample ranges and ambiguity](../m03/REPORT.md) |
| M02 audio transformation | `195d8b4` | [Upload consent, recovery and composition](../m02/REPORT.md) |
| M04 creative effects | `118f0e5` | [Granular, spectral and stutter DSP](../m04/REPORT.md) |
| M05 advanced automation | `a1778f2` | [Write, Latch, Punch, Trim and MIDI Learn](../m05/REPORT.md) |
| M06 live source triggering | `0794437` | [Prepared voices, captured events and replay](../m06/REPORT.md) |
| M07 portable projects | `a16aac8` | [Consented collection and relocation](../m07/REPORT.md) |
| M08 catalog metadata | `a0e26ff` | [Indexed fields, editing, manifests and scale](../m08/REPORT.md) |

## Application and verification

macOS 26.3.2 (25D2150), arm64 Release, CMake 3.31.6. The application SHA-256 is `0a443a5095d2502db007bf4df4e82ceaa9a52f2bf2435b632f02f332437230bf`. The test executable SHA-256 is `88add8f745c52fdd7772cc977627e2ee67604a2bcca39f57ada471f0547fafa9`.

The full, unfiltered Release CTest run passed **62/62 in 530.66 s**, including the AddressSanitizer effects target. Both executable hashes matched before and after the run. Application sources matched the M08 commit. [Release log](release.log)

To try this built candidate on this Mac from the repository root, open an isolated profile.

```sh
open -n "build/development-release/SoundPlus_artefacts/Release/SoundPlus.app" \
  --args --data-dir "$PWD/build/ai-sfx-tryout"
```

Choose **Generate SFX** in Library, Edit or Perform. With no key, setup and history are available. A live generation needs a personal Keychain credential and explicit confirmation of candidate count. Published candidates can use the existing Library transfer controls.

The original application process stayed open on its existing bundle and catalog. At handoff the selected recovery document was still at revision 4506, with 43 original catalog assets on schema 7. All 197 retained state files and 43 source paths checked against the M00 workspace inventory were present, with no modification times later than that audit. This final check examined paths, revision and timestamps. The [historical byte-level audit](../m00/workspace-preservation.json) retains its own candidate identity. Development used separate builds, catalogs, documents, provider fixtures and Keychain test identities.

## Acceptance

The [M09 program](../../acceptance/M09_PROGRAM.md) schedules 399 cases. Receipt-tool checks exercised fresh-run creation, registry identity and completeness, exclusive creation, and refusal of unsupported passes or missing evidence. Paid provider results, physical listening and device qualification, receiver requalification, clean install and release sign-off remain open. The AGPLv3 distribution strategy has owner approval.
