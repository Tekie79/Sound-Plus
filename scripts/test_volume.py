#!/usr/bin/env python3
"""Exercise only a disposable HFS+ image; never eject a user volume."""
import argparse
import json
import plistlib
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import time
import wave


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def wait_for(check, label, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            value = check()
            if value:
                return value
        except (sqlite3.Error, FileNotFoundError):
            pass
        time.sleep(0.1)
    raise RuntimeError(label)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    result = {"fixture": "disposable HFS+ disk image", "passed": False}
    with tempfile.TemporaryDirectory(prefix="soundplus-volume-", dir="/tmp") as temp:
        base = Path(temp).resolve()
        image = base / "fixture.dmg"
        first, second = base / "mount-a", base / "mount-b"
        first.mkdir()
        second.mkdir()
        mounted = None
        app = None
        try:
            run("hdiutil", "create", "-size", "16m", "-fs", "HFS+", "-volname", "SoundPlusFixture", str(image))
            run("hdiutil", "attach", "-nobrowse", "-mountpoint", str(first), str(image))
            mounted = first
            (first / "SFX").mkdir()
            with wave.open(str(first / "SFX" / "external click.wav"), "wb") as audio:
                audio.setparams((1, 2, 48000, 0, "NONE", "not compressed"))
                audio.writeframes(b"\0\x10\0\xf0" * 12000)
            catalog = base / "catalog" / "library.sqlite"

            def rows(sql):
                with sqlite3.connect(f"file:{catalog}?mode=ro", uri=True) as db:
                    return db.execute(sql).fetchall()

            with (args.output / "volume-app.log").open("w") as log:
                app = subprocess.Popen([str(args.executable.resolve()), "--data-dir", str(catalog.parent),
                                        "--add-folder", str(first / "SFX")], stdout=log, stderr=log)
                wait_for(lambda: rows("select id from assets where state='ready'"), "Initial volume import failed")
                identity = rows("select id from assets")[0][0]
                print("Initial root:", rows("select path,volume,volume_relative from roots"), flush=True)
                result["volume_identity_stored"] = bool(rows("select volume from roots")[0][0])
                run("hdiutil", "detach", str(first))
                mounted = None
                wait_for(lambda: rows("select id from roots where online=0"), "Detached volume was not marked offline")
                result["offline_entries_retained"] = rows("select id from assets")[0][0] == identity
                run("hdiutil", "attach", "-nobrowse", "-mountpoint", str(second), str(image))
                mounted = second
                info = plistlib.loads(subprocess.check_output(["diskutil", "info", "-plist", str(second)]))
                print("Remounted volume:", {k: info.get(k) for k in ("VolumeUUID", "MountPoint")}, flush=True)
                wait_for(lambda: rows("select id from roots where online=1") and
                         rows("select path from roots")[0][0] == str(second / "SFX"), "Volume did not relink at a new mount path")
                result["remount_at_new_path"] = True
                result["stable_asset_id"] = rows("select id from assets")[0][0] == identity
                result["unchanged_revision"] = rows("select revision from assets")[0][0] == 1
                result["passed"] = all(result[k] for k in ("volume_identity_stored", "offline_entries_retained",
                    "remount_at_new_path", "stable_asset_id", "unchanged_revision"))
        except Exception:
            if app:
                print("Root at failure:", rows("select path,volume,volume_relative,online,error from roots"), flush=True)
            raise
        finally:
            if app:
                app.terminate()
                try:
                    app.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    app.kill()
                    app.wait()
            if mounted:
                run("hdiutil", "detach", str(mounted))
    (args.output / "volume-result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    if not result["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
