#!/usr/bin/env python3
"""Fetch the official Microsoft NuGet native runtime; no .NET/Python runtime is shipped."""
import hashlib
from pathlib import Path
import tempfile
import urllib.request
import zipfile

VERSION = "1.22.1"
URL = f"https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/{VERSION}/microsoft.ml.onnxruntime.{VERSION}.nupkg"
ARCHIVE_SHA256 = "7b091e012dcfef2f9cc965f2252041101b1840c748ffbc1c18ba047bd6508b1c"
LIB_SHA256 = "36a46aeb65eee6618daded125577192514d058d0cce6adc4892bc1bcc77cb9be"

def main():
    root = Path(__file__).resolve().parents[1] / "vendor" / f"onnxruntime-{VERSION}"
    lib = root / "runtimes/osx-arm64/native/libonnxruntime.dylib"
    if lib.exists() and hashlib.sha256(lib.read_bytes()).hexdigest() == LIB_SHA256 and (root/"LICENSE").exists():
        print(f"Verified native ONNX Runtime {VERSION}")
        return
    root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="soundplus-runtime-") as temporary:
        archive = Path(temporary)/"runtime.nupkg"
        urllib.request.urlretrieve(URL, archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != ARCHIVE_SHA256:
            raise RuntimeError("ONNX Runtime archive checksum mismatch")
        with zipfile.ZipFile(archive) as package:
            for name in package.namelist():
                if name.startswith("build/native/include/") or name == "runtimes/osx-arm64/native/libonnxruntime.dylib" or name in ["LICENSE", "ThirdPartyNotices.txt"]:
                    target = root/name
                    if name.endswith("/"):continue
                    target.parent.mkdir(parents=True,exist_ok=True)
                    target.write_bytes(package.read(name))
    print(f"Installed verified native ONNX Runtime {VERSION}: {root}")

if __name__ == "__main__": main()
