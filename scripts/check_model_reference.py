#!/usr/bin/env python3
"""Optional build-time reference parity check. Python is never required by the application."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import socket
import subprocess
import sys
import urllib.request

def main():
    sys.path.insert(0,str(Path('.tools/semantic-reference').resolve()))
    import numpy as np
    from tokenizers import Tokenizer
    parser=argparse.ArgumentParser();parser.add_argument('--helper',type=Path,required=True)
    parser.add_argument('--model',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    source=out/'transformers-4.46.3-audio_utils.py'
    if not source.exists():
        source.write_bytes(urllib.request.urlopen('https://raw.githubusercontent.com/huggingface/transformers/v4.46.3/src/transformers/audio_utils.py').read())
    spec=importlib.util.spec_from_file_location('clap_reference_audio_utils',source);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    random=np.random.default_rng(170526);n=np.arange(57613)
    wave=(.12*np.sin(2*np.pi*n*313/48000)+.04*np.sin(2*np.pi*n*9587/48000)+random.normal(0,.002,len(n))).astype('<f4')
    raw=out/'input48k.f32';wave.tofile(raw)
    generated=out/'native-logmel.f32'
    subprocess.run([str(args.helper.resolve()),'--features',str(raw),str(generated)],check=True)
    repeated=np.tile(wave,480000//len(wave));padded=np.pad(repeated,(0,480000-len(repeated)))
    bank=module.mel_filter_bank(513,64,50,14000,48000,norm='slaney',mel_scale='slaney')
    reference=module.spectrogram(padded,module.window_function(1024,'hann'),1024,480,power=2,mel_filters=bank,log_mel='dB').T
    native=np.fromfile(generated,dtype='<f4').reshape(1001,64)
    error=np.abs(native-reference)
    assert float(error.max())<.02,f'Log-mel parity failed: {error.max()}'
    tokenizer=Tokenizer.from_file(str(args.model/'tokenizer.json'));tokenizer.enable_truncation(max_length=77)
    queries=['A heavy wooden door closing softly.','Leather clothing moving as someone turns.',
             'A small metallic latch followed by a short rattle.','雨 café — footsteps!',
             "can't won't I'M turning.\n  Heavy-door 42",'<mask> a door <s> closing </s>','', 'A door softly closing. '*100]
    parent,child=socket.socketpair();log=(out/'helper.log').open('wb')
    process=subprocess.Popen([str(args.helper.resolve())],stdin=child,stdout=log,stderr=log);child.close()
    stream=parent.makefile('rwb',buffering=0);seq=0
    def call(command):
        nonlocal seq
        seq+=1;command.update(version=4,sequence=seq)
        stream.write((json.dumps(command)+'\n').encode());reply=json.loads(stream.readline())
        assert not reply.get('error'),reply
        return reply
    cases=[]
    try:
        loaded=call({'command':'load','modelDirectory':str(args.model.resolve())})
        for query in queries:
            reply=call({'command':'text','text':query});expected=tokenizer.encode(query).ids
            assert reply['tokens']==expected,(query,reply['tokens'],expected)
            vector=np.asarray(reply['embedding']);assert len(vector)==512 and abs(float(np.linalg.norm(vector))-1)<1e-5
            cases.append({'text':query,'tokens':reply['tokens'],'passed':True})
    finally:
        stream.close();parent.close();process.wait(timeout=10);log.close()
    result={'passed':True,'reference':'Transformers 4.46.3 audio_utils + tokenizers 0.22.2 pinned tokenizer.json',
            'sourceSha256':hashlib.sha256(source.read_bytes()).hexdigest(),'inputSha256':hashlib.sha256(raw.read_bytes()).hexdigest(),
            'maxLogMelErrorDb':float(error.max()),'meanLogMelErrorDb':float(error.mean()),'shape':[1,1,1001,64],
            'tokenizerCases':cases,'helperLoadMs':loaded['loadMs']}
    (out/'reference-parity.json').write_text(json.dumps(result,indent=2,ensure_ascii=False)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k!='tokenizerCases'},indent=2))

if __name__=='__main__':main()
