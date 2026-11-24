#!/usr/bin/env python3
"""Prepare private, auditable macOS release candidates without publishing them."""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import plistlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
from uuid import UUID
from xml.etree import ElementTree
from zipfile import BadZipFile, ZipFile

ROOT = Path(__file__).resolve().parents[1]
APP = 'SoundPlus.app'
EDITOR = 'Contents/Helpers/SoundPlusPluginEditor.app'
ONNX = 'libonnxruntime.1.22.1.dylib'
RUNTIME = f'Contents/Frameworks/{ONNX}'
MICROPHONE = 'com.apple.security.device.audio-input'
PLUGINS = 'com.apple.security.cs.disable-library-validation'
# The enclosing bundle is always signed after all its nested code.
SIGNING_ORDER = (
    (RUNTIME, RUNTIME, 'org.soundplus.onnxruntime', ()),
    ('Contents/Helpers/SoundPlusAI', 'Contents/Helpers/SoundPlusAI', 'org.soundplus.helper.ai', ()),
    ('Contents/Helpers/SoundPlusAudio', 'Contents/Helpers/SoundPlusAudio', 'org.soundplus.helper.audio', (MICROPHONE,)),
    ('Contents/Helpers/SoundPlusPlugin', 'Contents/Helpers/SoundPlusPlugin', 'org.soundplus.helper.plugin', (PLUGINS,)),
    (EDITOR, f'{EDITOR}/Contents/MacOS/SoundPlusPluginEditor', 'org.soundplus.plugin-editor', (PLUGINS,)),
    ('.', 'Contents/MacOS/SoundPlus', 'org.soundplus.workstation', (MICROPHONE,)),
)
NOTICE_DIR = 'Contents/Resources/Licenses'
NOTICES = {
    'AGPL-3.0.txt', 'GPL-3.0.txt', 'JUCE-8.0.6.txt', 'VST3-SDK.txt', 'VST3-base.txt',
    'VST3-interfaces.txt', 'VST3-public-sdk.txt', 'Apache-2.0.txt', 'CLAP-upstream-CC0.txt',
    'Lucide-Attribution.txt', 'Lucide-ISC-and-Feather-MIT.txt', 'ONNXRuntime-MIT.txt',
    'ONNXRuntime-ThirdPartyNotices.txt',
}
PLIST_PATHS = [Path('Contents'), Path(EDITOR) / 'Contents']
APP_FILES = {entry[1] for entry in SIGNING_ORDER} | {
    str(path / name) for path in PLIST_PATHS
    for name in ('Info.plist', 'PkgInfo', 'Resources/RecentFilesMenuTemplate.nib')
} | {'Contents/Resources/resolve-transfer.py'} | {f'{NOTICE_DIR}/{name}' for name in NOTICES}
SEALS = {str(p / '_CodeSignature/CodeResources') for p in PLIST_PATHS}
APP_TICKET = 'Contents/CodeResources'
SOURCE_INPUTS = ('CMakeLists.txt', 'cmake', 'src', 'resources', 'scripts', 'tests', 'README.md',
                 'docs/DEPENDENCIES.md', 'docs/MACOS_RELEASE.md')


class ReleaseError(Exception):
    pass


def command(*args, cwd=ROOT, check=True, expose=True):
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if check and result.returncode:
        note = f': {result.stderr.strip() or result.stdout.strip()}' if expose else ''
        raise ReleaseError(f'{Path(args[0]).name} failed ({result.returncode}){note}')
    return result


def require(ok, message):
    if not ok:
        raise ReleaseError(message)


def digest(path):
    with path.open('rb') as stream:
        sha = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            sha.update(chunk)
        return sha.hexdigest()


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + '\n')


def hashes(root, paths):
    return {str(p.relative_to(root)): digest(p) for p in sorted(paths)}


def inspect_bundle(app, version, build, *, pristine=False):
    require(app.name == APP and app.is_dir() and not app.is_symlink(), f'Expected a {APP} bundle')
    files = set()
    directories = {parent for name in APP_FILES | SEALS | {APP_TICKET}
                   for parent in Path(name).parents if parent != Path('.')}
    for path in app.rglob('*'):
        relative = path.relative_to(app)
        require(not path.is_symlink(), f'Unexpected bundle symlink {relative}')
        if path.is_dir():
            require(relative in directories, f'Unexpected bundle directory {relative}')
        else:
            require(path.is_file(), f'Unexpected bundle entry {relative}')
            files.add(str(relative))
    require(APP_FILES <= files, f'Missing bundle files: {sorted(APP_FILES - files)}')
    require(files <= APP_FILES | SEALS | {APP_TICKET}, f'Unexpected bundle files: {sorted(files - APP_FILES - SEALS - {APP_TICKET})}')
    require(not pristine or APP_TICKET not in files, 'Expected a build output without an existing notarization ticket')

    for bundle, executable, identifier, _ in (SIGNING_ORDER[-2], SIGNING_ORDER[-1]):
        plist = plistlib.loads((app / bundle / 'Contents/Info.plist').read_bytes())
        expected = {'CFBundleIdentifier': identifier, 'CFBundleExecutable': Path(executable).name,
                    'CFBundleShortVersionString': version, 'CFBundleVersion': build, 'CFBundlePackageType': 'APPL'}
        require(all(plist.get(k) == v for k, v in expected.items()), f'Incorrect Info.plist identity or version for {identifier}')
        require(bool(plist.get('NSMicrophoneUsageDescription')) == (bundle == '.'), f'Microphone purpose text mismatch for {identifier}')

    architecture = {}
    for _, binary, _, _ in SIGNING_ORDER:
        path = app / binary
        arch = command('/usr/bin/lipo', '-archs', path).stdout.strip()
        require(arch == 'arm64', f'{binary} must contain exactly one arm64 slice')
        info = command('/usr/bin/xcrun', 'vtool', '-show-build', path).stdout
        minimum = '13.3' if binary == RUNTIME or binary.endswith('/SoundPlusAI') else '12.0'
        require(re.findall(r'^\s*platform\s+(\S+)', info, re.M) == ['MACOS'], f'Wrong platform for {binary}')
        require(re.findall(r'^\s*minos\s+(\S+)', info, re.M) == [minimum], f'Unexpected deployment target for {binary}')
        sdk = re.findall(r'^\s*sdk\s+(\S+)', info, re.M)
        architecture[binary] = {'architecture': arch, 'macOSDeploymentTarget': minimum, 'sdk': sdk[0]}
        deps = command('/usr/bin/otool', '-L', path).stdout.splitlines()[1:]
        expected_onnx = binary == RUNTIME or binary.endswith('/SoundPlusAI')
        paths = [line.strip().split(' (compatibility version', 1)[0] for line in deps]
        require(all(p.startswith(('/usr/lib/', '/System/Library/')) or
                    (expected_onnx and p == f'@rpath/{ONNX}') for p in paths), f'Non-system absolute or unexpected link in {binary}')
        require((f'@rpath/{ONNX}' in paths) == expected_onnx, f'Wrong optional runtime link in {binary}')
        load = command('/usr/bin/otool', '-l', path).stdout
        rpaths = re.findall(r'\bcmd LC_RPATH\s+cmdsize \d+\s+path (.+?) \(offset \d+\)', load)
        expected_rpaths = (['@loader_path'] if binary == RUNTIME else
                           ['@executable_path/../Frameworks'] if binary.endswith('/SoundPlusAI') else [])
        require(rpaths == expected_rpaths,
                f'Unexpected rpath in {binary}')
    if pristine:
        from fetch_runtime import LIB_SHA256
        require(digest(app / RUNTIME) == LIB_SHA256, 'Bundled ONNX Runtime differs from the pinned upstream binary')
    return {'binaries': architecture, 'bundleFiles': hashes(app, (app / p for p in files))}


def sign(app, identity=None, entitlements_dir=None):
    require(identity is None or entitlements_dir is not None, 'Entitlement staging is required for Developer ID')
    for path, _, identifier, entitlements in SIGNING_ORDER:
        args = ['--force', '--sign', identity or '-', '--identifier', identifier]
        if identity:
            args += ['--options', 'runtime', '--timestamp']
            if entitlements:
                plist = entitlements_dir / (identifier + '.plist')
                plist.write_bytes(plistlib.dumps(dict.fromkeys(entitlements, True)))
                args += ['--entitlements', plist]
        else:
            args += ['--timestamp=none']
        result = command('/usr/bin/codesign', *args, app / path, check=False)
        require(result.returncode == 0, f'Code signing failed for {identifier} ({result.returncode})')


def verify_signature(app, developer_id):
    command('/usr/bin/codesign', '--verify', '--deep', '--strict', '--verbose=4', app)
    if not developer_id:
        return
    team = None
    for path, _, identifier, entitlements in reversed(SIGNING_ORDER):
        code = app / path
        details = command('/usr/bin/codesign', '-dvvv', code).stderr
        match = re.search(r'^TeamIdentifier=([A-Z0-9]{10})$', details, re.M)
        require(match and (team is None or team == match[1]), f'Unexpected signing team for {identifier}')
        team = match[1]
        require(f'Identifier={identifier}\n' in details and 'Authority=Developer ID Application:' in details and
                re.search(r'^CodeDirectory .*\bflags=.*\(runtime\)', details, re.M) and
                'Timestamp=' in details, f'Developer ID, Hardened Runtime or timestamp verification failed for {identifier}')
        embedded = command('/usr/bin/codesign', '-d', '--entitlements', ':-', code).stdout
        require((plistlib.loads(embedded.encode()) if embedded.strip() else {}) == dict.fromkeys(entitlements, True),
                f'Unexpected entitlements for {identifier}')


def identity(kind, policy, variable):
    value = os.environ.get(variable, '').upper()
    require(re.fullmatch('[0-9A-F]{40}', value), f'Set {variable} to the authorized {kind} identity fingerprint')
    listed = command('/usr/bin/security', 'find-identity', '-v', '-p', policy).stdout
    require(re.search(rf'^\s*\d+\) {value} "{re.escape(kind)}:', listed, re.M),
            f'The authorized {kind} identity is unavailable in the Keychain')
    return value


def release_authorization(path, revision, version, build, package):
    require(path is not None, 'Release requires a private owner approval file for the exact candidate')
    approval = json.loads(path.read_text())
    require(isinstance(approval, dict) and all(approval.get(k) == v for k, v in {
        'license': 'AGPL-3.0-only', 'commit': revision, 'version': version,
        'build': build, 'format': package, 'scope': 'invitation-only-macos-qa'}.items()) and
        approval.get('correspondingSourceAndNoticesApproved') is True and
        all(isinstance(approval.get(k), str) and approval[k].strip() for k in ('approvedBy', 'evidenceReference')),
        'The approval file must record owner-reviewed corresponding source, notices and the exact candidate scope')
    application = identity('Developer ID Application', 'codesigning', 'SOUNDPLUS_DEVELOPER_ID_APPLICATION')
    installer = identity('Developer ID Installer', 'basic', 'SOUNDPLUS_DEVELOPER_ID_INSTALLER') if package == 'pkg' else None
    profile = os.environ.get('SOUNDPLUS_NOTARY_PROFILE', '').strip()
    require(profile and not profile.startswith('-'), 'Set SOUNDPLUS_NOTARY_PROFILE to the authorized Keychain profile name')
    command('/usr/bin/xcrun', 'notarytool', 'history', '--keychain-profile', profile, '--output-format', 'json', expose=False)
    return application, installer, profile


def accepted_receipt(reply):
    require(isinstance(reply, dict), 'Malformed notarization response')
    try:
        submission = str(UUID(reply['id']))
    except (ValueError, TypeError, KeyError, AttributeError) as error:
        raise ReleaseError('Apple did not return a valid notarization submission ID') from error
    require(reply.get('status') == 'Accepted', f'Apple has not accepted submission {submission}')
    return {'status': 'Accepted', 'id': submission}


def notarize(artifact, profile, evidence):
    reply = command('/usr/bin/xcrun', 'notarytool', 'submit', artifact, '--keychain-profile', profile,
                    '--wait', '--timeout', '30m', '--output-format', 'json', check=False)
    # Keep the service-issued ID even if the submission failed or timed out. Never record credentials or raw output.
    try:
        receipt = accepted_receipt(json.loads(reply.stdout))
    except json.JSONDecodeError as error:
        raise ReleaseError(f'No usable notarization response ({reply.returncode})') from error
    require(reply.returncode == 0, f'Notarization command did not complete for submission {receipt["id"]}')
    write_json(evidence, {**receipt, 'submittedSHA256': digest(artifact), 'format': artifact.suffix[1:]})


def staple(path):
    command('/usr/bin/xcrun', 'stapler', 'staple', path)
    command('/usr/bin/xcrun', 'stapler', 'validate', path)


def zip_app(app, archive):
    command('/usr/bin/ditto', '-c', '-k', '--sequesterRsrc', '--keepParent', app, archive)


def build_container(app, archive, scratch, application, installer):
    if archive.suffix == '.zip':
        zip_app(app, archive)
    elif archive.suffix == '.dmg':
        files = scratch / 'disk-image'
        files.mkdir()
        command('/usr/bin/ditto', app, files / APP)
        command('/usr/bin/hdiutil', 'create', '-fs', 'HFS+', '-format', 'UDZO',
                '-volname', 'SoundPlus', '-srcfolder', files, archive)
        if application:
            command('/usr/bin/codesign', '--sign', application, '--timestamp',
                    '--identifier', 'org.soundplus.disk-image', archive, expose=False)
    else:
        payload = scratch / 'installer-payload'
        payload.mkdir()
        command('/usr/bin/ditto', app, payload / APP)
        components = scratch / 'installer-components.plist'
        components.write_bytes(plistlib.dumps([{'RootRelativeBundlePath': APP, 'BundleIsRelocatable': False,
            'BundleIsVersionChecked': True, 'BundleHasStrictIdentifier': True, 'BundleOverwriteAction': 'upgrade'}]))
        build = plistlib.loads((app / 'Contents/Info.plist').read_bytes())['CFBundleVersion']
        component = scratch / 'SoundPlus-application.pkg'
        command('/usr/bin/pkgbuild', '--root', payload, '--component-plist', components,
                '--install-location', '/Applications', '--identifier', 'org.soundplus.workstation.installer',
                '--version', build, component)
        signature = ['--sign', installer] if installer else []
        requirements = scratch / 'installer-requirements.plist'
        requirements.write_bytes(plistlib.dumps({'os': ['12.0'], 'arch': ['arm64']}))
        command('/usr/bin/productbuild', '--package', component, '--product', requirements, *signature, archive, expose=False)


def quarantine(path):
    value = f'0083;{int(time.time()):x};SoundPlus Release QA;'
    command('/usr/bin/xattr', '-w', 'com.apple.quarantine', value, path)
    require(command('/usr/bin/xattr', '-p', 'com.apple.quarantine', path).stdout.strip() == value,
            'Could not confirm quarantine on the QA copy')


def gatekeeper(path, kind, *options, check=True):
    return command('/usr/sbin/spctl', '--assess', '--ignore-cache', '--no-cache',
                   '--type', kind, '--verbose=4', *options, path, check=check).returncode


def inspect_zip(archive):
    payload = {PurePosixPath(APP) / p for p in APP_FILES | SEALS | {APP_TICKET}}
    folders = {p for entry in payload for p in entry.parents if p != PurePosixPath('.')}
    with ZipFile(archive) as container:
        entries = container.infolist()
        require(len(entries) == len({item.filename for item in entries}) and
                sum(item.file_size for item in entries) < 512 * 1024 * 1024, 'Unexpected ZIP size or duplicate entries')
        for item in entries:
            path = PurePosixPath(item.filename)
            require(path.parts, 'Unexpected empty ZIP entry')
            require(not stat.S_ISLNK(item.external_attr >> 16), 'ZIP contains a symlink')
            if path.parts[0] == '__MACOSX':
                require(not path.is_absolute() and '..' not in path.parts, 'Unexpected ZIP metadata path')
                path = PurePosixPath(*path.parts[1:])
                if item.is_dir():
                    require(path in folders | {PurePosixPath('.')}, 'Unexpected ZIP metadata directory')
                    continue
                require(path.name.startswith('._') and (path.parent / path.name[2:]) in payload | folders,
                        'Unexpected ZIP metadata entry')
                continue
            require(path in (folders if item.is_dir() else payload), f'Unexpected ZIP entry {path}')


@contextmanager
def extracted_app(archive, temporary, *, trusted):
    # This operates on the exact artifact created by this invocation or matching its manifest digest.
    extracted = temporary / 'expanded'
    if archive.suffix == '.zip':
        inspect_zip(archive)
        extracted.mkdir()
        command('/usr/bin/ditto', '-x', '-k', archive, extracted)
        yield extracted / APP
    elif archive.suffix == '.dmg':
        command('/usr/bin/hdiutil', 'verify', archive)
        if trusted:
            command('/usr/bin/codesign', '--verify', '--strict', archive)
            gatekeeper(archive, 'open', '--context', 'context:primary-signature')
        extracted.mkdir()
        command('/usr/bin/hdiutil', 'attach', '-readonly', '-nobrowse', '-mountpoint', extracted, archive)
        try:
            yield extracted / APP
        finally:
            command('/usr/bin/hdiutil', 'detach', extracted)
    else:
        if trusted:
            command('/usr/sbin/pkgutil', '--check-signature', archive)
            gatekeeper(archive, 'install')
        command('/usr/sbin/pkgutil', '--expand-full', archive, extracted)
        require(not any(extracted.rglob('Scripts')), 'Installer has a Scripts payload')
        packages = list(extracted.rglob('PackageInfo'))
        require(len(packages) == 1, 'The installer must contain exactly one component')
        info = ElementTree.parse(packages[0]).getroot()
        require(info.get('install-location') == '/Applications' and info.get('relocatable') == 'false' and
                info.get('identifier') == 'org.soundplus.workstation.installer' and info.find('scripts') is None,
                'The installer can only write the application in /Applications')
        require(not info.findall('./relocate/bundle'), 'The installer must not relocate an existing copy')
        for policy in ('upgrade-bundle', 'bundle-version', 'strict-identifier'):
            require([node.get('id') for node in info.findall(f'./{policy}/bundle')] == ['org.soundplus.workstation'],
                    f'Unexpected installer {policy} policy')
        apps = list(extracted.rglob(APP))
        require(apps == [packages[0].parent / 'Payload' / APP] and list(apps[0].parent.iterdir()) == apps,
                'The installer payload must contain exactly one SoundPlus app')
        yield apps[0]


def verify_download(archive, expected, version, build, *, trusted):
    require(command('/usr/sbin/spctl', '--status').stdout.strip() == 'assessments enabled',
            'Gatekeeper assessments must be enabled')
    with tempfile.TemporaryDirectory(prefix='soundplus-download-') as work:
        temporary = Path(work)
        download = temporary / archive.name
        shutil.copyfile(archive, download)
        require(digest(download) == expected, 'The artifact bytes do not match the manifest')
        quarantine(download)
        if trusted and download.suffix != '.zip':
            command('/usr/bin/xcrun', 'stapler', 'validate', download)
        with extracted_app(download, temporary, trusted=trusted) as unpacked:
            # Mounts can be read-only and command-line extraction can drop quarantine.
            installed = temporary / APP
            command('/usr/bin/ditto', unpacked, installed)
            quarantine(installed)
            bundle = inspect_bundle(installed, version, build)
            verify_signature(installed, trusted)
            if trusted and download.suffix == '.zip':
                command('/usr/bin/xcrun', 'stapler', 'validate', installed)
            result = gatekeeper(installed, 'execute', check=trusted)
        return {'sha256': expected, 'quarantineApplied': True, 'strictSignatureVerified': True,
                'developerIdVerified': trusted, 'gatekeeperExit': result, 'verifiedBundle': bundle}


def provenance():
    from fetch_runtime import ARCHIVE_SHA256, LIB_SHA256, VERSION
    return {'commit': command('git', 'rev-parse', 'HEAD').stdout.strip(),
            'tree': command('git', 'rev-parse', 'HEAD^{tree}').stdout.strip(),
            'macOS': platform.mac_ver()[0], 'architecture': platform.machine(),
            'sdk': command('/usr/bin/xcrun', '--show-sdk-version').stdout.strip(),
            'clang': command('/usr/bin/xcrun', 'clang', '--version').stdout.splitlines()[0],
            'pinnedInputs': {'juce': {'version': '8.0.6',
                'archiveSHA256': '95752f8e8b37d802b4c10182bc757de7d88f0c8899e879afa8798589be306a11'},
                'onnx': {'version': VERSION, 'archiveSHA256': ARCHIVE_SHA256, 'librarySHA256': LIB_SHA256},
                'cmake': '3.31.6', 'ninja': '1.11.1.3'}}


def product_version():
    project = (ROOT / 'CMakeLists.txt').read_text()
    return re.search(r'project\(SoundPlus VERSION (\d+\.\d+\.\d+)', project)[1]


def build_fresh(work, revision, build, source_archive):
    source = work / 'source'
    source.mkdir()
    command('git', 'archive', '--format=tar', '--output', source_archive, revision, *SOURCE_INPUTS)
    command('/usr/bin/tar', '-xf', source_archive, '-C', source)
    bootstrap = source / 'scripts/bootstrap.sh'
    command('/bin/zsh', bootstrap, cwd=source)
    cmake = source / '.tools/cmake/data/bin/cmake'
    ninja = source / '.tools/bin/ninja'
    output = work / 'build'
    sdk = command('/usr/bin/xcrun', '--show-sdk-path').stdout.strip()
    command(cmake, '-S', source, '-B', output, '-G', 'Ninja', f'-DCMAKE_MAKE_PROGRAM={ninja}',
            '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_TESTING=OFF', '-DCMAKE_OSX_ARCHITECTURES=arm64',
            '-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0', f'-DCMAKE_OSX_SYSROOT={sdk}',
            f'-DSOUNDPLUS_BUILD_NUMBER={build}', cwd=source)
    command(cmake, '--build', output, '--target', 'SoundPlus', '--parallel', '6', cwd=source)
    return output / 'SoundPlus_artefacts/Release' / APP


def sbom(version, build, commit):
    def component(name, version, license_expression, role, url):
        return {'type': 'library', 'name': name, 'version': version, 'licenses': [{'expression': license_expression}],
                'externalReferences': [{'type': 'website', 'url': url}],
                'properties': [{'name': 'soundplus:distribution', 'value': role}]}
    return {'bomFormat': 'CycloneDX', 'specVersion': '1.6', 'version': 1,
            'metadata': {'component': {'type': 'application', 'name': 'SoundPlus', 'version': f'{version}+{build}',
                                       'properties': [{'name': 'soundplus:sourceCommit', 'value': commit}]}},
            'components': [
                component('JUCE', '8.0.6', 'AGPL-3.0-only OR LicenseRef-JUCE-8', 'compiled',
                          'https://github.com/juce-framework/JUCE/tree/8.0.6'),
                component('Steinberg VST 3 SDK (as vendored in JUCE)', '3.7.12', 'GPL-3.0-only AND BSD-3-Clause',
                          'compiled; GPL option for dual-licensed files',
                          'https://github.com/juce-framework/JUCE/tree/8.0.6/modules/juce_audio_processors/format_types/VST3_SDK'),
                component('ONNX Runtime', '1.22.1', 'MIT', 'bundled optional helper dylib; see third-party notices',
                          'https://www.nuget.org/packages/Microsoft.ML.OnnxRuntime/1.22.1'),
                component('Lucide with Feather attribution', 'de8d11aac831d519d2377ff516868244475fac06',
                          'ISC AND MIT', 'compiled vector assets', 'https://github.com/lucide-icons/lucide'),
                component('CLAP HTSAT unfused', 'c28f2883575e590e04d3146ff0713c2448d691ba', 'Apache-2.0',
                          'separate optional model download', 'https://huggingface.co/Xenova/clap-htsat-unfused'),
                component('SQLite', 'macOS system', 'LicenseRef-Public-Domain', 'OS-provided',
                          'https://www.sqlite.org/copyright.html'),
            ]}


def stage(args):
    require(platform.system() == 'Darwin' and platform.machine() == 'arm64', 'Run on an Apple Silicon Mac')
    require(re.fullmatch(r'[1-9][0-9]{0,3}', args.build_number), 'Use a build number from 1 to 9999')
    output = args.output.absolute()
    require(not os.path.lexists(output), 'Choose a new output path; existing staging is retained')
    version, build = product_version(), args.build_number
    source = provenance()
    trusted = args.action == 'release'
    fresh = trusted or args.fresh
    application, installer, profile = None, None, None
    if fresh:
        require(not command('git', 'status', '--porcelain').stdout.strip(), 'Release requires a clean Git worktree')
    if trusted:
        application, installer, profile = release_authorization(args.approval, source['commit'], version, build, args.format)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent, prefix='.soundplus-stage-') as work:
        scratch = Path(work)
        staged = scratch / 'candidate'
        staged.mkdir(mode=0o700)
        source_archive = staged / 'SoundPlus-corresponding-source.tar'
        original = build_fresh(scratch, source['commit'], build, source_archive) if fresh else args.app.absolute()
        before = inspect_bundle(original, version, build, pristine=True)
        app = staged / APP
        command('/usr/bin/ditto', original, app)
        dev = command('/usr/bin/codesign', '--verify', '--deep', '--strict', original, check=False)
        sign(app, application, scratch)
        verify_signature(app, trusted)
        name = f'SoundPlus-{version}-{build}-arm64' if trusted else 'SoundPlus-local-check-only'
        archive = staged / f'{name}.{args.format}'
        build_container(app, archive, scratch, application, installer)
        if trusted:
            notarize(archive, profile, staged / 'apple-notarization.json')
            if args.format == 'zip':
                staple(app)
                archive.unlink()
                zip_app(app, archive)
            else:
                staple(archive)
        candidate_hash = digest(archive)
        verified = verify_download(archive, candidate_hash, version, build, trusted=trusted)
        expected = inspect_bundle(app, version, build)
        require(verified['verifiedBundle'] == expected, 'Packaged app differs from the staged app')
        require(inspect_bundle(original, version, build, pristine=True) == before, 'Build output changed during staging')
        verified.pop('verifiedBundle')
        write_json(staged / 'sbom.cdx.json', sbom(version, build, source['commit']))
        write_json(staged / 'manifest.json', {
            'product': 'SoundPlus', 'version': version, 'build': build,
            'mode': 'developer-id-candidate' if trusted else 'local-ad-hoc-check-only',
            'provenance': {**source, 'builtFromCleanCommit': fresh},
            'preSigning': before, 'preSigningStrictExit': dev.returncode, 'postSigning': expected,
            'artifact': archive.name, 'downloadVerification': verified,
            'sourceSnapshotSHA256': digest(source_archive) if fresh else None,
            'ownerApprovalSHA256': digest(args.approval) if trusted else None,
            'externalAcceptanceComplete': False,
        })
        require(not os.path.lexists(output), 'Staging destination appeared during preparation')
        staged.rename(output)
    print(f'{args.action} complete: {output}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest='action', required=True)
    dry = subcommands.add_parser('dry-run', help='stage a local ad hoc copy and exercise bundle checks without credentials')
    input_kind = dry.add_mutually_exclusive_group(required=True)
    input_kind.add_argument('--app', type=Path)
    input_kind.add_argument('--fresh', action='store_true', help='build the native app from a clean pinned checkout')
    production = subcommands.add_parser('release', help='build a private candidate, sign, notarize, and verify it')
    production.add_argument('--approval', type=Path)
    for subcommand in (dry, production):
        subcommand.add_argument('--format', choices=('zip', 'dmg', 'pkg'), default='zip')
        subcommand.add_argument('--output', type=Path, required=True)
        subcommand.add_argument('--build-number', required=True)
    qa = subcommands.add_parser('verify-download', help='check the exact archive against a release manifest')
    qa.add_argument('--archive', type=Path, required=True)
    qa.add_argument('--manifest', type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.action == 'verify-download':
            manifest = json.loads(args.manifest.read_text())
            require(manifest['mode'] == 'developer-id-candidate', 'Developer ID manifest required')
            require(args.archive.name == manifest['artifact'] and args.archive.suffix in ('.zip', '.dmg', '.pkg'),
                    'Use the exact artifact named in the release manifest')
            print(json.dumps(verify_download(args.archive, manifest['downloadVerification']['sha256'],
                                             manifest['version'], manifest['build'], trusted=True), indent=2))
        else:
            stage(args)
    except (ReleaseError, OSError, ValueError, KeyError, BadZipFile, ElementTree.ParseError) as error:
        parser.exit(1, f'SoundPlus release blocked: {error}\n')


if __name__ == '__main__':
    main()
