#!/usr/bin/env python3
"""Create a local, blind listening form. No ratings or listener claims are generated."""
import argparse
import html
import json
import os
from pathlib import Path

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--fixture',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    manifest=json.loads(args.fixture.read_text());media=Path(manifest['mediaDirectory']);out=args.output.resolve();out.parent.mkdir(parents=True,exist_ok=True)
    cases=[
        ('Rain pattering steadily outside.','clip-003.wav',0,5),
        ('A wooden door slowly creaking as it moves.','clip-029.wav',0,5),
        ('A wooden door slowly creaking as it moves.','clip-041.wav',20,30),
        ('A metal drinks can opening with a short pop and rattle.','clip-006.wav',0,5),
        ('A metal drinks can opening with a short pop and rattle.','clip-042.wav',20,30),
        ('Glass breaking and scattering in sharp fragments.','clip-043.wav',40,50),
        ('A person walking with distinct footsteps.','clip-042.wav',0,10),
        ('A vacuum cleaner humming while cleaning.','clip-036.wav',0,5),
        ('A clock ticking regularly.','clip-025.wav',0,5),
        ('A horse neighing beside a trumpet playing a melody.','clip-035.wav',0,5)]
    cards=[];data=[]
    for i,(query,file,start,end) in enumerate(cases,1):
        source=Path(os.path.relpath(media/file,out.parent)).as_posix();case={'case':i,'query':query,'file':file,'startSeconds':start,'endSeconds':end};data.append(case)
        choices=''.join(f'<label><input type="radio" name="case{i}" value="{score}">{score}</label>' for score in range(4))
        cards.append(f'<article><h2>{i}. {html.escape(query)}</h2><p>{file} · {start}–{end} seconds</p><audio id="audio{i}" controls preload="none" src="{html.escape(source)}"></audio> <button onclick="play({i},{start},{end})">Play returned range</button><fieldset><legend>Relevance</legend>{choices}</fieldset></article>')
    document='''<!doctype html><meta charset="utf-8"><title>SoundPlus listening check</title>
<style>body{font:16px system-ui;max-width:900px;margin:40px auto;padding:0 20px;background:#111820;color:#e5edf4}h1{font-size:30px}h2{font-size:18px}article{padding:18px;background:#1b2632;margin:18px 0;border:1px solid #34485b;border-radius:8px}p{color:#b8c9d8}audio{width:330px;vertical-align:middle}button{background:#8dcced;color:#112331;border:0;padding:10px 15px;border-radius:5px;cursor:pointer}fieldset{display:inline-block;border:0;margin:10px}fieldset label{padding:9px}legend{font-size:13px;color:#b8c9d8}pre{white-space:pre-wrap}</style>
<h1>SoundPlus listening check</h1><p>Listen to each returned range and judge it against the description. File names are anonymous. These are local recordings; this page sends no data anywhere.</p>
<p><strong>0</strong> = no relevant sound, <strong>1</strong> = weak/uncertain, <strong>2</strong> = relevant, <strong>3</strong> = clearly matches. Judge the sound you hear, including whether a long-file range contains it. About two minutes of listening.</p>'''+''.join(cards)+'''
<button onclick="exportRatings()">Export ratings JSON</button><p id="hint">Rate all ten pairs. You can also reply with the numbered ratings below.</p><pre id="summary"></pre>
<script>const cases=CASES;
function play(i,start,end){document.querySelectorAll('audio').forEach(a=>a.pause());const a=document.getElementById('audio'+i);a.currentTime=start;a.ontimeupdate=()=>{if(a.currentTime>=end)a.pause()};a.play()}
function ratings(){return cases.map(c=>({...c,rating:document.querySelector('input[name=case'+c.case+']:checked')?.value??null}))}
document.addEventListener('change',()=>{document.getElementById('summary').textContent=ratings().map(c=>c.case+': '+(c.rating??'?')).join(', ')});
function exportRatings(){const rows=ratings();if(rows.some(c=>c.rating===null)){document.getElementById('hint').textContent='Please rate every pair before exporting.';return}const result={listener:'human-listener-1',timestamp:new Date().toISOString(),scale:{0:'none',1:'weak',2:'relevant',3:'clear'},judgments:rows.map(c=>({...c,rating:Number(c.rating)}))};const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([JSON.stringify(result,null,2)],{type:'application/json'}));a.download='soundplus-listener-ratings.json';a.click();setTimeout(()=>URL.revokeObjectURL(a.href),1000)}
</script>'''.replace('CASES',json.dumps(data))
    out.write_text(document);print(out)

if __name__=='__main__':main()
