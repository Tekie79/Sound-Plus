#!/usr/bin/env python3
"""Independent Python-stdlib auditor for the generated v1.5.1 archive.

Reads only relative archive members. The referenced design deliberately retains
its original source paths and immutable hashes; index.json maps those names to
archived bytes. No JUCE or files from the originating Mac are required.
"""
import argparse,array,hashlib,json,math,pathlib,struct,tempfile,zipfile
sha=lambda b:hashlib.sha256(b).hexdigest()
checks=0
def require(value,why):
    global checks
    checks+=1
    if not value:raise AssertionError(why)
def raw_field(source,key):
    decoder=json.JSONDecoder();pos=source.index('{')+1
    while True:
        while source[pos].isspace() or source[pos]==',':pos+=1
        name,end=decoder.raw_decode(source,pos);pos=end
        while source[pos].isspace():pos+=1
        require(source[pos]==':','JSON member separator');pos+=1
        while source[pos].isspace():pos+=1
        start=pos;_,pos=decoder.raw_decode(source,pos)
        if name==key:return source[start:pos]
        require(source[pos]!='}','Required JSON member '+key)
def wav(path):
    b=path.read_bytes();require(b[:4]==b'RIFF' and b[8:12]==b'WAVE','RIFF/WAVE signature');pos=12;fmt=data=None
    while pos+8<=len(b):
        tag=b[pos:pos+4];size=struct.unpack_from('<I',b,pos+4)[0];part=b[pos+8:pos+8+size]
        require(len(part)==size,'Complete finalized WAV chunk')
        if tag==b'fmt ':fmt=part
        if tag==b'data':data=part
        pos+=8+size+(size&1)
    require(fmt is not None and data is not None,'WAV format/data present')
    kind,channels,rate,_,align,bits=struct.unpack_from('<HHIIHH',fmt)
    mask=0
    if kind==65534:mask=struct.unpack_from('<I',fmt,20)[0];kind=struct.unpack_from('<H',fmt,24)[0]
    require(kind in (1,3) and bits==32 and align==channels*4 and len(data)%align==0,'Qualified finalized 32-bit PCM/float')
    samples=array.array('f' if kind==3 else 'i',data)
    import sys
    if sys.byteorder!='little':samples.byteswap()
    if kind==1:samples=[x/(2**31) for x in samples]
    return samples,dict(channels=channels,rate=rate,frames=len(samples)//channels,mask=mask)
def audit(root):
    manifest=json.loads((root/'manifest.json').read_text())
    def member(name):
        p=pathlib.PurePosixPath(name);require(not p.is_absolute() and '..' not in p.parts,'Relative archive path')
        return root/name
    for name,expected in manifest['files'].items():require(sha(member(name).read_bytes())==expected,'Stored bytes '+name)
    artifacts=json.loads(member('artifacts.json').read_text());source_digest=sha(json.dumps(artifacts['sourceFiles'],sort_keys=True,separators=(',',':')).encode());require(source_digest==artifacts['compiledSourceTreeSha256']==manifest['compiledSourceTreeSha256'],'Source/artifact manifest aggregate identity')
    index=json.loads((root/'index.json').read_text());docraw=(root/'document.soundplus').read_text();envelope=json.loads(docraw);payloadraw=raw_field(docraw,'payload');d=envelope['payload']
    require(sha(payloadraw.encode())==envelope['sha256'],'Stored document payload checksum')
    take=d['takes'][0];takesraw=raw_field(payloadraw,'takes');_,end=json.JSONDecoder().raw_decode(takesraw,1);traw=takesraw[1:end]
    require(sha(raw_field(traw,'recipe').encode())==take['recipeHash'],'Exact frozen recipe hash from stored tokens')
    timeline='{'+', '.join(json.dumps(k)+': '+raw_field(traw,k) for k in ['events','bodyEnd','outputFrames','captureRate'])+'}'
    require(sha(timeline.encode())==take['eventHash'],'Exact event/duration hash from stored tokens')
    require(take['id']==index['take'] and take['complete'] and take['outputFrames']==index['captureFrames'],'Complete exact consumed take')
    require(d['version']==7 and d['sourcePoolMetadataVersion']==1,'Unchanged pool metadata version')
    recipe=take['recipe'];require(recipe['processing']=='stock-layout-takes-v2','Existing frozen DSP identity')
    layers=recipe['mix']['layers'];require(all('sourcePool' not in l for l in layers),'Frozen recipe contains no mutable pool')
    origins=take['poolOrigins'];require(origins['version']==1 and len(origins['entries'])==1,'Versioned exact pool origin')
    origin=origins['entries'][0];entry=origin['entry'];layer=layers[0];route=layer['routing']
    require(origin['entry']['id']==index['selectedEntry'] and origin['layer']==layer['id'],'Pool entry/layer identity')
    require(entry['first']==layer['first']==index['sourceFirst'] and entry['last']==layer['last']==index['sourceLast'],'Exact original-frame range')
    require(entry['routing']==route and route['inputs']==index['physicalInputs']==[0,3],'Exact reviewed physical selection and complete routing')
    require(route['interpretation']['id']=='stereo-v1' and route['interpretation']['roles']==['L','R'],'Ordered source interpretation')
    for key in ['id','revision','size','modified','inode','rate','samples','channels','path']:
        require(entry['source'][key]==layer['source'][key],'Frozen source identity '+key)
    live=d['original']['layers'][0];pool=live['sourcePool'];require(pool['id']==origin['pool'] and pool['selected']==entry['id'],'Live pool origin identity')
    require(live['gainDb']==-9 and layer['gainDb']==0,'Later Live edits do not mutate frozen parameters')
    require(entry['reviewed'] and entry['reviewedBus']==recipe['mix']['busLayout'],'Reviewed destination identity')
    source_name=index['pathMap'][entry['source']['path']];source_path=member(source_name)
    require(sha(source_path.read_bytes())==entry['sha256']==take['sources'][0]['sha256'],'Actual source bytes match frozen fingerprints')
    source,header=wav(source_path);require(header['channels']==6 and header['rate']==48000,'Independent six-physical-channel fixture')
    capture,ch=wav(member('pcm/consumed.wav'));replay,rh=wav(member('pcm/replay.wav'));require(ch['frames']==rh['frames']==index['captureFrames'] and ch['channels']==rh['channels']==6,'Stored capture/replay dimensions')
    tolerance=index['pcmTolerance'];require(len(capture)==len(replay),'Capture/replay sample counts');error=max(abs(a-b) for a,b in zip(capture,replay));require(error<=tolerance,'Consumed capture versus replay PCM')
    expected_error=0
    for n in range(ch['frames']):
        for output in range(6):
            expected=0.
            for edge in route['matrix']:
                if edge['out']==output and not edge['mute']:
                    physical=route['inputs'][edge['in']];value=source[(n+layer['first'])*6+physical]
                    expected+=value*(10**(edge['db']/20))*(-1 if edge['invert'] else 1)
            expected_error=max(expected_error,abs(capture[n*6+output]-expected))
    require(expected_error<=tolerance,'Independent physical-channel/mapping/precision/polarity/mute oracle')
    roles=['L','R','C','LFE','Ls','Rs'];output_errors=[]
    for receipt in d['outputs']:
        require(receipt['recipeHash']==take['recipeHash'] and receipt['eventHash']==take['eventHash'],'Receipt immutable identity')
        p=member(index['pathMap'][receipt['path']]);require(sha(p.read_bytes())==receipt['sha256'],'Finalized output hash')
        samples,wh=wav(p);require(wh['channels']==6 and wh['frames']==ch['frames'] and wh['rate']==48000,'Finalized interleaved header')
        output_errors.append(max(abs(a-b) for a,b in zip(samples,capture)));require(output_errors[-1]<=tolerance,'Finalized interleaved PCM equals consumed capture')
        require(receipt['channelProvenance']['export']['roles']==roles,'Export channel order')
        require(receipt['sources'][0]['poolOrigins']==origins['entries'],'Receipt pool origin exact identity')
        if 'files' in receipt:
            require(len(receipt['files'])==6,'All split members retained')
            for c,stem in enumerate(receipt['files']):
                require(stem['role']==roles[c],'Split ordered role')
                p=member(index['pathMap'][stem['path']]);require(sha(p.read_bytes())==stem['sha256'],'Split finalized hash')
                mono,mh=wav(p);require(mh['channels']==1 and mh['frames']==ch['frames'],'Split member dimensions')
                require(max(abs(x-capture[n*6+c]) for n,x in enumerate(mono))<=tolerance,'Split physical channel PCM')
    printed=d['original']['layers'][1];require(printed['source']['channels']==6 and printed['routing']['interpretation']['id']=='5.1-side-v1','Actual Print layer remains multichannel')
    require(printed['source']['path'] in [o['path'] for o in d['outputs']],'Printed finalized master identity')
    require(d['outputSelections'][take['id']]==d['outputs'][1]['path'],'Selected output set persists independently')
    return dict(passed=True,checks=checks,captureReplayMaxError=error,independentMappingMaxError=expected_error,outputErrors=output_errors,frames=ch['frames'],roles=roles,compiledSourceTreeSha256=manifest['compiledSourceTreeSha256'],qualification='Generated numerical byte/PCM evidence; no native receiving-app or human listening claim')
def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('bundle',type=pathlib.Path);ap.add_argument('--output',type=pathlib.Path);args=ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        root=args.bundle
        if root.is_file():
            root=pathlib.Path(tmp)
            with zipfile.ZipFile(args.bundle) as z:
                require(sum(i.file_size for i in z.infolist())<64*1024*1024,'Bounded generated archive')
                for i in z.infolist():
                    p=pathlib.PurePosixPath(i.filename);require(not p.is_absolute() and '..' not in p.parts,'Safe archive member')
                    z.extract(i,root)
        result=audit(root)
    text=json.dumps(result,indent=2)+'\n';print(text,end='')
    if args.output:args.output.write_text(text)
if __name__=='__main__':main()
