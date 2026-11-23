"""Release staging policy regressions. Apple trust requires a real Developer ID candidate."""
from argparse import Namespace
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
from zipfile import ZipFile, ZipInfo

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import release_macos as release


class ReleaseTests(unittest.TestCase):
    def test_valid_submission_and_invalid_or_unfinished_status(self):
        identifier = '74375430-2d58-47a7-8ef1-0cc8c635b9c9'
        self.assertEqual(release.accepted_receipt({'id': identifier, 'status': 'Accepted'}),
                         {'id': identifier, 'status': 'Accepted'})
        for reply in (None, [], {}, {'id': 'bad', 'status': 'Accepted'},
                      *({'id': identifier, 'status': state} for state in ('Invalid', 'Rejected', 'In Progress'))):
            with self.subTest(reply=reply), self.assertRaises(release.ReleaseError):
                release.accepted_receipt(reply)

    def test_missing_keychain_identity_fails_without_notary_invocation(self):
        with patch.dict(os.environ, {'SOUNDPLUS_DEVELOPER_ID_APPLICATION': 'A' * 40}), \
                patch.object(release, 'command', return_value=Mock(stdout='0 valid identities found')) as call:
            with self.assertRaisesRegex(release.ReleaseError, 'identity is unavailable'):
                release.identity('Developer ID Application', 'codesigning', 'SOUNDPLUS_DEVELOPER_ID_APPLICATION')
        call.assert_called_once_with('/usr/bin/security', 'find-identity', '-v', '-p', 'codesigning')

    def test_approval_applies_to_exact_commit_build_and_container(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'approval.json'
            path.write_text(json.dumps({'approvedBy': 'QA', 'evidenceReference': 'fixture',
                'license': 'AGPL-3.0-only', 'commit': 'fixture', 'version': '0.1.0', 'build': '1',
                'format': 'zip', 'scope': 'invitation-only-macos-qa', 'correspondingSourceAndNoticesApproved': True}))
            with patch.object(release, 'identity') as identity:
                for rev, build, fmt in (('different', '1', 'zip'), ('fixture', '2', 'zip'), ('fixture', '1', 'pkg')):
                    with self.subTest(commit=rev, build=build, package=fmt), self.assertRaises(release.ReleaseError):
                        release.release_authorization(path, rev, '0.1.0', build, fmt)
                identity.assert_not_called()

    def test_bundle_rejects_outside_link_unknown_fixture_and_missing_files(self):
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory) / release.APP
            resources = app / 'Contents/Resources'
            resources.mkdir(parents=True)
            link = resources / 'link'
            link.symlink_to(Path(directory) / 'outside')
            with self.assertRaisesRegex(release.ReleaseError, 'Unexpected bundle symlink'):
                release.inspect_bundle(app, '0.1.0', '1')
            link.unlink()
            with self.assertRaisesRegex(release.ReleaseError, 'Missing bundle files'):
                release.inspect_bundle(app, '0.1.0', '1')
            for rel in release.APP_FILES:
                path = app / rel
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            (resources / 'fixture.wav').touch()
            with self.assertRaisesRegex(release.ReleaseError, 'Unexpected bundle files'):
                release.inspect_bundle(app, '0.1.0', '1')

    def test_missing_receipt_does_not_preserve_an_accepted_record(self):
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            for exit_code, reply in ((0, {'id': 'bad', 'status': 'Accepted'}),
                                     (1, {'id': '74375430-2d58-47a7-8ef1-0cc8c635b9c9', 'status': 'Accepted'})):
                with patch.object(release, 'command', return_value=subprocess.CompletedProcess(
                        [], exit_code, json.dumps(reply), '')):
                    evidence = temporary / 'notary.json'
                    with self.assertRaises(release.ReleaseError):
                        release.notarize(temporary / 'unsent.zip', 'FAKE-PROFILE', evidence)
                    self.assertFalse(evidence.exists())

    def test_failed_validation_retains_original_and_creates_no_candidate(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            original = folder / release.APP
            original.mkdir()
            marker = original / 'keep'
            marker.write_text('original bundle')
            output = folder / 'private'
            with patch.object(release, 'provenance', return_value={'commit': 'fixture'}):
                args = Namespace(action='dry-run', build_number='1', output=output, app=original, fresh=False, format='zip')
                with self.assertRaises(release.ReleaseError):
                    release.stage(args)
            self.assertEqual(list(folder.iterdir()), [original])
            self.assertEqual(marker.read_text(), 'original bundle')

    def test_signing_order_and_local_entitlement_isolation(self):
        with patch.object(release, 'command', return_value=Mock(returncode=0)) as call, \
                tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            app = folder / release.APP
            release.sign(app)
            self.assertEqual([cmd.args[-1] for cmd in call.call_args_list], [app / rel for rel, *_ in release.SIGNING_ORDER])
            self.assertTrue(all('--entitlements' not in cmd.args and '--options' not in cmd.args for cmd in call.call_args_list))
            self.assertEqual(call.call_args.args[-1], app)

            call.reset_mock()
            release.sign(app, 'FAKE-DEVELOPER-ID', folder)
            self.assertTrue(all('--timestamp' in cmd.args and 'runtime' in cmd.args for cmd in call.call_args_list))
            entries = {p.stem: release.plistlib.loads(p.read_bytes()) for p in folder.glob('*.plist')}
            self.assertEqual(entries, {
                'org.soundplus.workstation': {release.MICROPHONE: True},
                'org.soundplus.helper.audio': {release.MICROPHONE: True},
                'org.soundplus.helper.plugin': {release.PLUGINS: True},
                'org.soundplus.plugin-editor': {release.PLUGINS: True},
            })

    def test_package_checksum_failure_precedes_extraction(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / 'SoundPlus.zip'
            archive.write_bytes(b'tampered')
            with patch.object(release, 'command', return_value=Mock(stdout='assessments enabled')), \
                    patch.object(release, 'extracted_app') as extract:
                with self.assertRaisesRegex(release.ReleaseError, 'bytes do not match'):
                    release.verify_download(archive, '0' * 64, '0.1.0', '1', trusted=True)
                extract.assert_not_called()

    def test_zip_rejects_traversal_and_symlinks(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / 'candidate.zip'
            with ZipFile(archive, 'w') as output:
                output.writestr('SoundPlus.app/../../../outside', 'owned QA fixture')
            with self.assertRaisesRegex(release.ReleaseError, 'Unexpected ZIP entry'):
                release.inspect_zip(archive)

            symlink = ZipInfo('SoundPlus.app/Contents/MacOS/SoundPlus')
            symlink.create_system = 3
            symlink.external_attr = 0o120777 << 16
            with ZipFile(archive, 'w') as output:
                output.writestr(symlink, '../../outside')
            with self.assertRaisesRegex(release.ReleaseError, 'ZIP contains a symlink'):
                release.inspect_zip(archive)


if __name__ == '__main__':
    unittest.main()
