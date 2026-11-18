#!/usr/bin/env python3
"""Isolated native UI benchmark. Fixture construction is never timed as import."""
import argparse
import collections
import hashlib
import json
import math
import os
from pathlib import Path
import plistlib
import shutil
import struct
import subprocess
import tempfile
import time
import wave


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")


def prepare(output, count):
    previous=output/"fixture.json"
    if previous.exists():
        existing=json.loads(previous.read_text())
        if Path(existing["fixtureRoot"]).is_dir():
            if existing["fileCount"]!=count:raise RuntimeError("Choose another output directory for a different fixture size")
            print("Reusing existing isolated fixture: "+existing["fixtureRoot"],flush=True)
            return previous
    start = time.perf_counter()
    base = Path(tempfile.mkdtemp(prefix="soundplus-benchmark-", dir="/private/tmp"))
    (base / ".soundplus-benchmark").write_text("Generated disposable benchmark fixture\n")
    seeds = base / "seeds"
    seeds.mkdir()
    samples = struct.pack("<128h", *(round(2000 * math.sin(i * .2)) for i in range(128)))
    wav = seeds / "seed.wav"
    with wave.open(str(wav), "wb") as file:
        file.setparams((1, 2, 48000, 0, "NONE", "not compressed"))
        file.writeframes(samples)
    aiff_pcm = b"".join(samples[i:i+2][::-1] for i in range(0, len(samples), 2))
    comm = struct.pack(">hIh", 1, 128, 16) + bytes.fromhex("400ebb80000000000000")
    chunks = b"COMM" + struct.pack(">I", len(comm)) + comm + b"SSND" + struct.pack(">III", len(aiff_pcm)+8, 0, 0) + aiff_pcm
    (seeds / "seed.aiff").write_bytes(b"FORM"+struct.pack(">I",len(chunks)+4)+b"AIFF"+chunks)
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg:
        subprocess.run([ffmpeg, "-v", "error", "-i", str(wav), str(seeds / "seed.flac")], check=True)
    else:
        subprocess.run(["afconvert", "-f", "flac", "-d", "LEI16", str(wav), str(seeds / "seed.flac")], check=True)
    payloads = {ext:(seeds / ("seed."+ext)).read_bytes() for ext in ("wav","aiff","flac")}
    roots = [base / "library-A", base / "library-B"]
    logical = allocated = malformed = 0
    names = []
    formats = collections.Counter()
    tokens = ("soft mechanical click", "wind forest ambience", "water river splash", "metal impact door")
    for i in range(count):
        ext = "aiff" if i%20==18 else "flac" if i%20==19 else "wav"
        name = "duplicate."+ext if i%200<2 else f"effect {i:06d} {tokens[i%4]}.{ext}"
        if i%997==1:
            name = "雨 café " + name
        root = roots[i%2]
        bucket = i//200
        file = root / f"bank-{bucket//50:02d}" / f"bucket-{bucket%50:02d}" / name
        file.parent.mkdir(parents=True, exist_ok=True)
        invalid = i%9999==0
        data = b"malformed fixture\n" if invalid else payloads[ext]
        file.write_bytes(data)
        size = file.stat()
        logical += len(data)
        allocated += size.st_blocks*512
        malformed += invalid
        formats[ext] += 1
        names.append((name, root.name))
    long = base / "long-recordings"
    long.mkdir()
    long_duration = long_bytes = 0
    for i in range(6):
        rate = (44100,48000,96000)[i%3]
        channels = 1+i%2
        seconds = 180 if i==0 else 45
        block = struct.pack("<"+"h"*(1024*channels), *(round(2000*math.sin(n*.06)) for n in range(1024*channels)))
        file = long / f"long-{i}.wav"
        with wave.open(str(file),"wb") as audio:
            audio.setparams((channels,2,rate,0,"NONE","not compressed"))
            left=rate*seconds
            while left:
                n=min(left,1024);audio.writeframesraw(block[:n*channels*2]);left-=n
        long_duration+=seconds;long_bytes+=file.stat().st_size
    queries=[]
    for i in range(200):
        kind=i%5
        if kind==0: text,source=names[(i*491+123)%count][0],""
        elif kind==1: text,source=("" if i%20==1 else f"effect {i%10:02d}"),""
        elif kind==2: text,source='"soft mechanical"',""
        elif kind==3: text,source="effect",roots[(i//5)%2].name
        else: text,source=f"zzznomatch{i}",""
        queries.append({"text":text,"source":source,"kind":("exact","prefix-or-clear","phrase","source-filter","empty")[kind]})
    actual=sum(1 for root in roots for path in root.rglob('*') if path.is_file())
    if actual!=count:raise RuntimeError(f"Fixture collision: expected {count}, found {actual}")
    device=subprocess.check_output(["df","-P",str(base)],text=True).splitlines()[-1].split()[0]
    storage = plistlib.loads(subprocess.check_output(["diskutil","info","-plist",device]))
    manifest={"fixtureRoot":str(base),"roots":list(map(str,roots)),"fileCount":count,"logicalBytes":logical,"allocatedBytes":allocated,
        "audioSeconds":(count-malformed)*128/48000,"malformed":malformed,"formats":dict(formats),
        "layout":"Two roots; alternating files; 100 entries per bucket per root; 50 buckets per bank; no hardlinks/clones",
        "constructionSeconds":time.perf_counter()-start,"seedHashes":{ext:hashlib.sha256(data).hexdigest() for ext,data in payloads.items()},
        "longCorpus":{"fileCount":6,"seconds":long_duration,"bytes":long_bytes},"longAudio":str(long / "long-0.wav"),
        "templateAudio":str(wav),"changeFolder":str(roots[0] / "bank-00" / "bucket-00"),"queries":queries,
        "storage":{k:storage.get(k) for k in ("FilesystemType","SolidState","DeviceIdentifier","BusProtocol","APFSContainerReference")}}
    write_json(output / "fixture.json",manifest)
    print(json.dumps({k:manifest[k] for k in ("fixtureRoot","fileCount","logicalBytes","audioSeconds","constructionSeconds","storage")},indent=2),flush=True)
    return output / "fixture.json"


def percentile(values, q):
    if not values:return None
    values=sorted(values);return values[max(0,math.ceil(len(values)*q)-1)]


def stats(values):
    return {"n":len(values),"p50":percentile(values,.5),"p95":percentile(values,.95),"p99":percentile(values,.99)}


def run(app, fixture, output, label):
    manifest=json.loads(fixture.read_text());output.mkdir(parents=True,exist_ok=True)
    base=Path(manifest["fixtureRoot"]).resolve()
    change=Path(manifest["changeFolder"]).resolve()
    if not base.name.startswith("soundplus-benchmark-") or not (base/".soundplus-benchmark").is_file() or not change.is_relative_to(base):
        raise RuntimeError("Use a disposable fixture created by this script's prepare command")
    catalog=output/"catalog"
    if catalog.exists():raise RuntimeError("Choose a new output directory: benchmark catalog already exists")
    def clean_changes():
        for f in [*change.glob("benchmarklive *.wav"),change/"000 selection.wav"]:
            f.unlink(missing_ok=True)
    clean_changes()
    plan={**manifest,"expectedFiles":manifest["fileCount"],"queryCount":200,"mode":"full"}
    write_json(output/"plan.json",plan)
    start=time.perf_counter()
    def launch(mode, roots):
        p={**plan,"mode":mode}
        write_json(output/(mode+"-plan.json"),p)
        cmd=[str(app),"--data-dir",str(catalog),"--benchmark-plan",str(output/(mode+"-plan.json")),"--benchmark-output",str(output/(mode+".json"))]
        for root in roots:cmd += ["--add-folder",root]
        with (output/(mode+".log")).open("w") as log:
            launched=time.monotonic()*1000
            result=subprocess.run(cmd,stdout=log,stderr=log,timeout=1800)
        if result.returncode:raise RuntimeError(f"Native benchmark failed: {mode}, exit {result.returncode}")
        report=json.loads((output/(mode+".json")).read_text())
        if not report["success"]:raise RuntimeError(report["error"])
        if report["firstNonemptyPaintMs"]:
            report["launchToFirstNonemptyPaintMs"]=report["startMonotonicMs"]+report["firstNonemptyPaintMs"]-launched
        write_json(output/(mode+".json"),report)
        return report
    decoder=Path("build/SoundPlusTests_artefacts/Release/SoundPlusTests").resolve()
    long_decode=None
    if decoder.exists():
        with (output/"long-decode.log").open("w") as log:
            subprocess.run([str(decoder),"--long-decode",str(Path(manifest["longAudio"]).parent),str(output/"long-decode.json")],stdout=log,stderr=log,check=True)
        long_decode=json.loads((output/"long-decode.json").read_text())
    full=launch("full",manifest["roots"])
    reopened=launch("reopen",[])
    controls=None
    if not label.startswith("baseline"):
        clean_changes()
        catalog=output/"control-catalog"
        controls=launch("controls",manifest["roots"])
    grouped={name:stats([x["dispatchToPaintMs"] for x in full["samples"] if x["workload"]==name]) for name in ("indexing","warm","auditioning")}
    per_run={w:[stats([s["dispatchToPaintMs"] for s in full["samples"] if s["workload"]==w and s["run"]==n]) for n in range(3)] for w in ("warm","auditioning")}
    c=full["importStats"]
    summary={"label":label,"fixture":manifest,"cacheState":"OS file cache uncontrolled and not purged; fixture construction separate; fresh application/catalog process; repeated queries warm",
        "appSha256":hashlib.sha256(app.read_bytes()).hexdigest(),"environment":{"cpu":subprocess.check_output(["sysctl","-n","machdep.cpu.brand_string"],text=True).strip(),"os":subprocess.check_output(["sw_vers"],text=True).strip(),"machine":subprocess.check_output(["sysctl","-n","hw.model","hw.memsize"],text=True).strip(),"configuration":"Release arm64"},
        "uiDispatchToFirstPaintMs":grouped,"uiPerRun":per_run,"auditionDispatchToFirstCallbackMs":stats(full["auditions"]),"incrementalCloseToVisibleMs":stats(full["incrementalChanges"]),
        "firstEligibleToCommittedSearchableMs":c["firstCommitMs"]-c["firstEligibleMs"],
        "discoveryFilesPerSecond":manifest["fileCount"]*1000/(c["discoveryFinishedMs"]-c["firstEligibleMs"]),
        "longDecode":long_decode,"nativeControls":controls,"reopen":reopened,"peakMemoryBytes":full["counters"]["peakResidentBytes"],"peakDurableMetadataBacklog":full["peakDurableMetadataBacklog"],"wallSecondsExcludingConstruction":time.perf_counter()-start}
    write_json(output/"summary.json",summary)
    print(json.dumps({k:summary[k] for k in ("label","uiDispatchToFirstPaintMs","auditionDispatchToFirstCallbackMs","incrementalCloseToVisibleMs","discoveryFilesPerSecond","peakMemoryBytes")},indent=2),flush=True)


def main():
    p=argparse.ArgumentParser();subs=p.add_subparsers(dest="command",required=True)
    prep=subs.add_parser("prepare");prep.add_argument("--files",type=int,default=100000);prep.add_argument("--output",type=Path,default=Path("build/benchmark"))
    bench=subs.add_parser("run");bench.add_argument("--app",type=Path,required=True);bench.add_argument("--fixture",type=Path,default=Path("build/benchmark/fixture.json"));bench.add_argument("--output",type=Path,required=True);bench.add_argument("--label",default="candidate")
    args=p.parse_args()
    if args.command=="prepare":prepare(args.output,args.files)
    else:run(args.app.resolve(),args.fixture.resolve(),args.output.resolve(),args.label)


if __name__=="__main__":main()
