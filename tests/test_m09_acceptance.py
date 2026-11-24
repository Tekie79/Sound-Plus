"""Candidate and evidence integrity failures, independent of the operator acceptance matrix."""
import json
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import m09_candidate as rc
import m09_acceptance as receipts


class RunTests(unittest.TestCase):
    def setUp(self):
        tmp = TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name).resolve()
        self.path = self.root / "run.json"
        self.identity = {"commit": "a" * 40, "manifestSha256": "b" * 64}
        self.cases = {"M08-12": {}}
        receipts.new_run(self.path, self.cases, self.identity)
        self.run = receipts.read(self.path)
        self.row = self.run["results"]["M08-12"]
        (self.root / "evidence").mkdir()
        self.log = self.root / "evidence/test.log"
        self.log.write_text("actual observation\n")

    def validate(self):
        self.path.write_text(json.dumps(self.run))
        return receipts.check_run(self.path, self.cases, self.identity)

    def observed(self):
        self.run.update(platform="macOS arm64", catalog="isolated")
        self.row.update(status="Pass", owner="development test", actual="Verified fixture", runUtc=rc.now(),
                        evidence=[receipts.evidence(self.root, "evidence/test.log", self.run)])

    def test_run_immutability(self):
        self.assertEqual(self.validate(), {"Not Run": 1})
        with self.assertRaises(ValueError):
            receipts.new_run(self.path, self.cases, self.identity)
        self.run["runId"] = str(receipts.uuid.uuid4())
        with self.assertRaises(ValueError):
            self.validate()

    def test_removed_run_cannot_reset_retained_identity(self):
        self.path.unlink()
        with self.assertRaises(FileExistsError):
            receipts.new_run(self.path, self.cases, self.identity)

    def test_actual_attachment_and_timestamps(self):
        self.observed()
        self.assertEqual(self.validate(), {"Pass": 1})
        for field, invalid in (("runUtc", "yesterday"), ("runUtc", "2099-12-01T23:00:00Z"),
                               ("runUtc", "2000-01-01T00:00:00Z"), ("runUtc", "2020-01-01T00:00:00"),
                               ("evidence", []), ("status", "Not Run"), ("owner", "")):
            with self.subTest(field=field, invalid=invalid), patch.dict(self.row, {field: invalid}):
                with self.assertRaises(ValueError):
                    self.validate()

    def test_replaced_missing_or_old_run_attachment(self):
        self.observed()
        self.log.write_text("false observation\n")
        with self.assertRaises(ValueError):
            self.validate()
        self.log.unlink()
        with self.assertRaises(ValueError):
            self.validate()
        self.log.write_text("actual observation\n")
        self.row["evidence"][0]["runId"] = str(receipts.uuid.uuid4())
        with self.assertRaises(ValueError):
            self.validate()

    def test_attachment_must_stay_in_run(self):
        (self.root / "evidence/link").symlink_to(self.path)
        for rel in ("evidence/link", "evidence/../run.json", str(self.log), "run.json"):
            with self.subTest(rel=rel), self.assertRaises(ValueError):
                receipts.evidence(self.root, rel, self.run)

    def test_registry_historical_links(self):
        self.assertEqual(len(receipts.catalog(receipts.ROOT)), 399)
        data = receipts.read(receipts.ROOT / rc.REGISTRY)
        data["cases"][0]["priorM00"]["record"] = "absent"
        old = receipts.read(receipts.ROOT / receipts.PRIOR)
        with patch.object(receipts, "read", side_effect=[data, old]), self.assertRaises(ValueError):
            receipts.catalog(receipts.ROOT)


class CandidateTests(unittest.TestCase):
    def setUp(self):
        tmp = TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name).resolve()
        def git(*args):
            return subprocess.check_output(["git", "-C", self.root, *args], text=True).strip()
        git("init", "-q")
        (self.root / ".gitignore").write_text("build/\n")
        (self.root / rc.REGISTRY).parent.mkdir(parents=True)
        (self.root / rc.REGISTRY).write_text("fixture")
        bridge = self.root / "resources/resolve/transfer.py"
        bridge.parent.mkdir(parents=True)
        bridge.write_text("fixture bridge")
        git("add", ".")
        git("-c", "user.name=QA", "-c", "user.email=qa@localhost", "commit", "-qm", "fixture")
        self.commit = git("rev-parse", "HEAD")
        self.exe = self.root / rc.EXE
        self.exe.parent.mkdir(parents=True)
        self.exe.write_bytes(b"fixture SOUNDPLUS-CANDIDATE:" + self.commit.encode() + b"\0")
        self.helper = self.root / rc.APP / "Contents/Helpers/SoundPlusAudio"
        self.helper.parent.mkdir()
        self.helper.write_bytes(b"helper fixture")
        resource = self.root / rc.APP / "Contents/Resources/resolve-transfer.py"
        resource.parent.mkdir()
        resource.write_bytes(bridge.read_bytes())
        def inspect(app, *args, **kwargs):
            return {"bundleFiles": rc.release.hashes(app, [self.exe, self.helper, resource])}
        mock = patch.object(rc.release, "inspect_bundle", side_effect=inspect)
        mock.start()
        self.addCleanup(mock.stop)
        identity = rc.source_identity(self.root, self.commit)
        logs = {}
        for log in ("release.log", "build.log"):
            f = self.root / rc.BUILD / log
            f.write_text("fixture")
            logs[log] = rc.release.digest(f)
        rc.exclusive(self.root / rc.BUILD / "candidate.json",
                     {"version": 1, "source": identity, "binaryPath": str(rc.EXE),
                      "inventory": rc.inventory(self.root, self.commit), "logs": logs})

    def test_checkout_and_only_supported_executable(self):
        self.assertEqual(rc.verify(self.root, self.commit, self.exe)["commit"], self.commit)
        with self.assertRaises(ValueError):
            rc.verify(self.root, "0" * 40, self.exe)
        with self.assertRaises(ValueError):
            rc.verify(self.root, self.commit, self.helper)
        (self.root / rc.REGISTRY).write_text("edited")
        with self.assertRaises(ValueError):
            rc.verify(self.root, self.commit, self.exe)

    def test_stale_helper_executable_or_gate(self):
        for path in (self.helper, self.exe, self.root / rc.BUILD / "release.log"):
            before = path.read_bytes()
            path.write_bytes(b"changed")
            with self.subTest(path=path), self.assertRaises(ValueError):
                rc.verify(self.root, self.commit, self.exe)
            path.write_bytes(before)


if __name__ == "__main__":
    unittest.main()
