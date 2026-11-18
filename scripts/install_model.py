#!/usr/bin/env python3
"""Install the same pinned, verified pack used by the native Install AI Model control."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile
import urllib.request

def main():
    manifest=json.loads((Path(__file__).resolve().parents[1]/"resources/clap-model.json").read_text())
    parser=argparse.ArgumentParser()
    parser.add_argument("--destination",type=Path,default=Path.home()/"Library/Application Support/SoundPlus/Models"/manifest["id"])
    parser.add_argument("--from-pack",type=Path,help="Import these files without network access")
    args=parser.parse_args()
    def verify(folder):
        return all((folder/f['name']).is_file() and (folder/f['name']).stat().st_size==f['bytes'] and hashlib.sha256((folder/f['name']).read_bytes()).hexdigest()==f['sha256'] for f in manifest['files'])
    if args.destination.exists():
        if verify(args.destination):print(f"Verified installed model: {args.destination}");return
        raise RuntimeError("Destination exists but does not match the verified pack; choose a fresh destination")
    args.destination.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".soundplus-model-install-",dir=args.destination.parent) as temporary:
        stage=Path(temporary)
        for file in manifest['files']:
            target=stage/file['name']
            if args.from_pack:shutil.copyfile(args.from_pack/file['name'],target)
            else:urllib.request.urlretrieve(file['url'],target)
            if target.stat().st_size!=file['bytes'] or hashlib.sha256(target.read_bytes()).hexdigest()!=file['sha256']:
                raise RuntimeError(f"Model checksum mismatch: {file['name']}")
            print(f"Verified {file['name']} ({file['bytes']} bytes)",flush=True)
        (stage/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
        stage.rename(args.destination)
    print(f"Installed model pack: {args.destination}")

if __name__ == "__main__":main()
