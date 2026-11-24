"""Build the pinned UCS text prototypes with the existing local SoundPlusAI helper."""
import argparse
import base64
from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import wave

ROOT = Path(__file__).resolve().parents[1]


def packed(v):
    length = math.sqrt(sum(x*x for x in v))
    if len(v) != 512 or not math.isfinite(length) or length < 1e-6:
        raise ValueError("Invalid CLAP prototype")
    return base64.b64encode(struct.pack("<512f", *(x / length for x in v))).decode()


def build(helper, model):
    taxonomy = ROOT / "resources/ucs-8.2.1.json"
    manifest = json.loads((ROOT / "resources/clap-model.json").read_text())
    groups = defaultdict(list)
    for category, subcategory, aliases in json.loads(taxonomy.read_text()).values():
        groups[category].append((subcategory.lower(), aliases.split(",")[0].strip().lower()))
    host, child = socket.socketpair()
    host.settimeout(45)
    with child, host, subprocess.Popen([helper], stdin=child) as process, host.makefile("rb") as replies:
        child.close()
        def call(command, **fields):
            host.sendall((json.dumps(dict(version=11, command=command, **fields))+"\n").encode())
            answer = json.loads(replies.readline(65536))
            if answer.get("error") or answer.get("model") != manifest["id"] or answer.get("preprocessing") != manifest["preprocessing"]:
                raise ValueError(answer.get("error", "Embedding space mismatch"))
            return answer
        try:
            call("load", modelDirectory=str(model.resolve()))
            rows = []
            for category, examples in sorted(groups.items()):
                indices = sorted({round(i*(len(examples)-1)/5) for i in range(6)})
                terms = ", ".join(dict.fromkeys(w for i in indices for w in examples[i] if w != "misc"))
                words = category.lower().replace("&", "and")
                prompts = [f"The sound of {words}.", f"A recording of {words}: {terms}."]
                vectors = [call("text", text=q)["embedding"] for q in prompts]
                rows.append(dict(category=category, prompts=prompts, vector=packed([a+b for a,b in zip(*vectors)])))
            with tempfile.TemporaryDirectory() as tmp:
                silence = Path(tmp)/"silence.wav"
                with wave.open(str(silence),"wb") as out:
                    out.setparams((1,2,48000,0,"NONE","")); out.writeframes(bytes(96000))
                s = silence.stat()
                quiet = call("audio", path=str(silence),size=s.st_size,modified=s.st_mtime_ns,inode=s.st_ino,
                             startSample=0,endSample=48000,sourceRate=48000,sourceSamples=48000)["embedding"]
            return dict(version=1, model=manifest["id"],preprocessing=manifest["preprocessing"],
                        taxonomySha256=hashlib.sha256(taxonomy.read_bytes()).hexdigest(),silence=packed(quiet),categories=rows)
        finally:
            host.shutdown(socket.SHUT_WR)
            process.wait(timeout=10)


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    for arg in ("helper","model","output"):
        p.add_argument("--"+arg,type=Path,required=True)
    args = p.parse_args()
    args.output.write_text(json.dumps(build(args.helper.resolve(),args.model),indent=1)+"\n")
