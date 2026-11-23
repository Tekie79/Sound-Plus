# SoundPlus macOS release procedure

Production Beta Gate 2 keeps the application feature set frozen. The release identifier pairs the CMake product version with a monotonically increasing integer build number. This cycle uses **0.1.0 (1)**. Both application plists carry the same identifiers. The installation path for PKG is `/Applications/SoundPlus.app`. ZIP and DMG installations can use another folder.

## Build and inspect locally

Use an Apple Silicon Mac with current Apple developer command-line tools. Use the pinned bootstrap and a Release build. For an ordinary pre-existing build, close SoundPlus and its bundle helpers before rebuilding. Keep test catalogs separate.

```sh
./scripts/bootstrap.sh
python3 scripts/guard_bundle_closed.py
.tools/cmake/data/bin/cmake -S . -B build -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$PWD/.tools/bin/ninja" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
  -DSOUNDPLUS_BUILD_NUMBER=1
.tools/cmake/data/bin/cmake --build build --target SoundPlus --parallel 6
python3 -m unittest discover -s tests -p test_release_macos.py -v
python3 scripts/release_macos.py dry-run \
  --app build/SoundPlus_artefacts/Release/SoundPlus.app \
  --build-number 1 --format zip --output build/private-release/structural-zip
```

Use a new output directory for each run. The same dry run supports DMG and PKG. Use `--fresh` in place of `--app` to build from the exact clean Git commit using pinned inputs, without test targets. Ad hoc signing seals only the private staging copy. It proves the inside-out layout and container round trip. The manifest records the actual Gatekeeper exit status. Retain the ad hoc archive in local QA storage.

## Authorized release candidate

The owner approved the **AGPLv3 route** on November 23, 2026. Before providing a candidate to testers, obtain documented approval for the exact source snapshot, license terms and third-party notices. Deliver the matching complete corresponding source and build/install scripts to recipients under the adopted terms. The generated source tar is the tracked project build input snapshot. Use the pinned dependency inventory and bundle notices to complete and review the corresponding-source delivery, including the JUCE source and relevant third-party notices. Keep restricted evaluation recordings and user media out of release and source packages. Public visibility of a Git checkout does not by itself establish compliance. [GNU AGPLv3](https://www.gnu.org/licenses/agpl-3.0.en.html) · [JUCE licensing](https://juce.com/get-juce/)

Place the exact release authorization JSON outside Git. Its fields are `approvedBy`, `evidenceReference`, `license` (currently `AGPL-3.0-only`), `commit` (full SHA), `version`, `build` (string), `format`, `scope` (`invitation-only-macos-qa`) and `correspondingSourceAndNoticesApproved` (true after the owner review). The release manifest keeps a digest of this authorization. Supply authorized identity fingerprints to `SOUNDPLUS_DEVELOPER_ID_APPLICATION` and, for PKG only, `SOUNDPLUS_DEVELOPER_ID_INSTALLER`. Supply the authorized Keychain profile name in `SOUNDPLUS_NOTARY_PROFILE`. Configure credentials on the authorized release host with Apple tooling and keep private keys and passwords out of environment arguments, Git, and shared logs.

```sh
python3 scripts/release_macos.py release --build-number 1 --format zip \
  --approval "$SOUNDPLUS_RELEASE_APPROVAL" --output build/private-release/candidate-001
```

The command builds from a clean commit and creates a new private output only after verification. It signs the ONNX dylib, AI, audio and plug-in workers, nested plug-in editor, then the enclosing app. Each production code signature uses Hardened Runtime and a secure timestamp. Audio input is requested by the app and opened by its audio helper. The plug-in worker and native editor alone load third-party processor code and require the library-validation exception. The other code keeps library validation. No JIT, unsigned executable memory, debugger entitlement, App Sandbox exception, or storefront entitlement is requested. Audit actual signed plug-in and permission behavior on the installed candidate. [Apple distribution signing](https://developer.apple.com/documentation/xcode/creating-distribution-signed-code-for-the-mac) · [Library-validation exception](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.cs.disable-library-validation)

ZIP contains the app and gets rebuilt after the app ticket is stapled. DMG has a signed read-only image. PKG has a separately signed, version-checked component that targets the expected application identifier and contains no installer scripts. For DMG and PKG, notarize and staple the outer container. The pipeline checks the Accepted submission UUID, stores its submitted SHA-256, verifies the staple, assesses the quarantined artifact, and checks the extracted app independently. The ZIP ticket is validated on the extracted app. Keep the final artifact SHA distinct from the submitted pre-staple SHA. Failed private staging is removed and leaves the original bundle and earlier outputs intact. If Apple returns a rejected submission ID, use `xcrun notarytool log` with the authorized profile on the release host; review before retrying. Build checkout, diagnostic logs and approval records can contain private paths. Share only scrubbed excerpts and release manifests. [Apple packaging](https://developer.apple.com/documentation/xcode/packaging-mac-software-for-distribution) · [Apple notarization workflow](https://developer.apple.com/documentation/security/customizing-the-notarization-workflow)

## Clean-account acceptance and recovery

1. On a second Apple Silicon Mac or separate fresh standard user profile, download the exact artifact through the intended delivery channel. Check the SHA against the manifest and keep its quarantine attribute. Run `python3 scripts/release_macos.py verify-download --archive /path/to/archive --manifest /path/to/manifest.json` from a checkout with developer tools on a qualification Mac. On the clean target, use Finder and the system Installer or archive utility for the actual install. If macOS rejects it, collect the assessment details and stop installation.
2. Launch once with a fresh catalog. Test folder access, first audition/default device, microphone recording permission, real MIDI input, and AU/VST3 scan with independently installed plug-ins. Test base Library and Designer workflows on the oldest qualified base OS and optional AI separately at macOS 13.3 or later. The deployment targets are 12.0 base and 13.3 AI; the engineering run covers macOS 26.3.2.
3. With the old version closed, copy the complete QA catalog directory, including any SQLite sidecars, recovery journals, takes, and profiles. Hash the old catalog and original/recorded WAVs, external IRs, frozen takes, and Resolve-linked renders. Keep file paths authoritative. Upgrade only the app, then verify Library and collections, source pools, native plugin behavior, reopened design/takes, and missing-media/IR repair.
4. Quit all SoundPlus processes. Uninstall by removing the app bundle alone. Keep `~/Library/Application Support/SoundPlus`, original media, and exported renders. Reinstall and reopen. For rollback, keep the newer catalog backup and first try the old app with a copy of the compatible pre-upgrade catalog. An older build may reject newer schemas. Never put the old copy over the latest state.
5. Move the app to a second local folder and verify playback, plug-in worker/editor launch, and optional AI inference. Move a generated QA source or IR away and check the missing-reference state. Restore its original path or explicitly relink. Designs reference files in place; collecting and packaging source media is outside this milestone.
6. For external acceptance, use a new QA design and an isolated Resolve project. Place both finalized signed-app 5.1 Side/Rear outputs through Media Pool, save, quit both apps, and reopen the receiving project. Inspect six-channel roles and actual online audio file paths. Test native drag separately. Keep PCM checks, physical channel mapping, and signed human listening notes in separate evidence.

Use the Release report to confirm the approved systems, physical device coverage and release decision before distributing any package.
