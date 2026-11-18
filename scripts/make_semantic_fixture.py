#!/usr/bin/env python3
"""Download a provenance-tracked ESC-50 evaluation subset. Labels stay outside media/."""
import argparse
import csv
import hashlib
import io
import json
from pathlib import Path
import random
import urllib.request
import wave

REVISION="33c8ce9eb2cf0b1c2f8bcf322eb349b6be34dbb6"
BASE=f"https://raw.githubusercontent.com/karolpiczak/ESC-50/{REVISION}"
CATEGORIES={
 "rain":"Rain pattering steadily outside.",
 "sea_waves":"Ocean waves washing onto a beach.",
 "crackling_fire":"A fire crackling and popping.",
 "crickets":"Crickets chirping at night.",
 "water_drops":"Individual water drops dripping.",
 "pouring_water":"Water being poured into a container.",
 "toilet_flush":"A toilet flushing with rushing water.",
 "door_wood_knock":"Someone knocking on a wooden door.",
 "door_wood_creaks":"A wooden door slowly creaking as it moves.",
 "can_opening":"A metal drinks can opening with a short pop and rattle.",
 "clock_alarm":"An alarm clock ringing insistently.",
 "clock_tick":"A clock ticking regularly.",
 "glass_breaking":"Glass breaking and scattering in sharp fragments.",
 "footsteps":"A person walking with distinct footsteps.",
 "brushing_teeth":"The repeated scraping sound of brushing teeth.",
 "engine":"A motor engine running and rumbling.",
 "hand_saw":"A hand saw scraping back and forth through wood.",
 "chainsaw":"A loud chainsaw buzzing as it cuts.",
 "washing_machine":"A washing machine spinning and rattling.",
 "vacuum_cleaner":"A vacuum cleaner humming while cleaning."}

def main():
    parser=argparse.ArgumentParser();parser.add_argument("output",type=Path);args=parser.parse_args()
    out=args.output.resolve()
    if out.exists() and any(out.iterdir()):raise RuntimeError("Choose an empty evaluation directory")
    media=out/"media";meta=out/"evaluation";media.mkdir(parents=True);meta.mkdir()
    table=urllib.request.urlopen(BASE+"/meta/esc50.csv").read()
    (meta/"esc50.csv").write_bytes(table)
    (meta/"ESC50-LICENSE").write_bytes(urllib.request.urlopen(BASE+"/LICENSE").read())
    rows=list(csv.DictReader(io.StringIO(table.decode())))
    chosen=[]
    for category in CATEGORIES:
        candidates=[r for r in rows if r['category']==category]
        if len(candidates)<2:raise RuntimeError(category)
        chosen.extend(candidates[:2])
    random.Random(170526).shuffle(chosen)
    clips=[];by_category={}
    for i,row in enumerate(chosen):
        filename=f"clip-{i+1:03d}.wav";url=BASE+"/audio/"+row['filename']
        data=urllib.request.urlopen(url).read();(media/filename).write_bytes(data)
        with wave.open(io.BytesIO(data)) as wav:
            rate=wav.getframerate();samples=wav.getnframes();channels=wav.getnchannels()
        clip={"file":filename,"category":row['category'],"sourceFile":row['filename'],"sourceUrl":url,
              "freesoundId":row['src_file'],"freesoundUrl":"https://freesound.org/s/"+row['src_file']+"/",
              "take":row['take'],"license":"ESC-50 compilation CC BY-NC 3.0; see evaluation/ESC50-LICENSE and source Freesound record",
              "sha256":hashlib.sha256(data).hexdigest(),"bytes":len(data),"rate":rate,"samples":samples,"channels":channels}
        clips.append(clip);by_category.setdefault(row['category'],[]).append(filename)
        print(filename,row['category'],flush=True)
    long_events=[];background_events=[]
    for n,(target,background,offset) in enumerate([('door_wood_creaks','rain',20),('can_opening','footsteps',25),('glass_breaking','crackling_fire',40)]):
        source=by_category[target][0];bed=by_category[background][1]
        with wave.open(str(media/source)) as f:params=f.getparams();event=f.readframes(f.getnframes())
        with wave.open(str(media/bed)) as f:ambient=f.readframes(f.getnframes())
        assert len(event)==len(ambient) and params.nchannels==1 and params.sampwidth==2
        # Twelve real 5-second excerpts; the event replaces one ambient excerpt. No synthesized tones.
        data=b''.join(event if i==offset//5 else ambient for i in range(12))
        if n==2:data+=ambient[:params.framerate*5]  # 2.5 s PCM16 tail exercises the final partial window.
        name=f"clip-{41+n:03d}.wav"
        with wave.open(str(media/name),'wb') as f:f.setparams(params);f.writeframes(data)
        raw=(media/name).read_bytes()
        clips.append({'file':name,'category':target,'composite':True,'eventSource':source,'backgroundSource':bed,
                      'eventStartSample':offset*params.framerate,'eventEndSample':(offset+5)*params.framerate,
                      'rate':params.framerate,'samples':len(data)//2,'channels':1,'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest(),
                      'license':'Derived evaluation-only composite from attributed ESC-50 sources; same CC BY-NC 3.0 compilation terms'})
        long_events.append({'file':name,'startSeconds':offset,'endSeconds':offset+5,'category':target})
        for first,last in [(0,offset),(offset+5,len(data)/2/params.framerate)]:
            background_events.append({'file':name,'startSeconds':first,'endSeconds':last,'category':background})
    queries=[]
    for category,text in CATEGORIES.items():
        expected=by_category[category]+sorted(set(x['file'] for x in long_events+background_events if x['category']==category))
        queries.append({'query':text,'expectedFiles':expected,'expectedRegions':[x for x in long_events+background_events if x['category']==category],
                        'judgmentSource':'ESC-50 event annotation; human listener review pending','listenerJudgments':[]})
    queries.append({'query':'A horse neighing beside a trumpet playing a melody.','expectedFiles':[],
                    'expectedRegions':[],'judgmentSource':'Deliberate absent-event query; no horse or trumpet category included','listenerJudgments':[]})
    manifest={'source':'ESC-50 evaluation subset','revision':REVISION,'sourceUrl':'https://github.com/karolpiczak/ESC-50','mediaDirectory':str(media),
              'labelDirectoryOutsideIndex':str(meta),'files':clips,'queries':queries,'longEvents':long_events,'backgroundEvents':background_events,
              'fileCount':len(clips),'audioSeconds':sum(c['samples']/c['rate'] for c in clips),'bytes':sum(c['bytes'] for c in clips),
              'modelTrainingOverlap':'Not established held-out from CLAP training; workflow evaluation, not a generalization claim',
              'license':'Evaluation corpus only; not bundled with application; ESC-50 compilation CC BY-NC 3.0 and source attribution retained'}
    (meta/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps({k:manifest[k] for k in ['fileCount','audioSeconds','bytes','mediaDirectory']},indent=2))

if __name__=='__main__':main()
