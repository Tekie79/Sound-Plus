"""Compare verified candidate manifests plus Mach-O sections and embedded source stamps."""
import json,hashlib,struct,sys,subprocess
from pathlib import Path
f,r=map(lambda p:Path(p).resolve(),sys.argv[1:])
sys.path.insert(0,str(f/'scripts'))
import m09_candidate as mc
commit='3be7178ee0525791af3726dbb03b0fdaefec1130'
def sections(data):
    assert struct.unpack_from('<I',data)[0]==0xfeedfacf
    n=struct.unpack_from('<I',data,16)[0];offset=32;result={}
    for _ in range(n):
        cmd,size=struct.unpack_from('<II',data,offset)
        if cmd==0x19:
            ns=struct.unpack_from('<I',data,offset+64)[0]
            for i in range(ns):
                name,seg,addr,length,idx=struct.unpack_from('<16s16sQQI',data,offset+72+80*i)
                if idx and length:
                    key=(seg.rstrip(b'\0')+b','+name.rstrip(b'\0')).decode()
                    result[key]=hashlib.sha256(data[idx:idx+length]).hexdigest()
        offset+=size
    return result
records=[];section=[];manifests=[]
for p in (f,r):
    binary=p/mc.EXE;raw=binary.read_bytes();record=mc.verify(p,commit,binary)
    mf=json.loads((p/mc.BUILD/'candidate.json').read_bytes())
    deps=mc.dependencies(p);assert deps==mf['configuredDependencyTreesSha256']
    assert b'SOUNDPLUS-CANDIDATE:'+commit.encode()+b'\0' in raw
    records.append(dict(**record,configuredDependencies=deps,gate=mf['gate'],provenance=mf['build'],path=str(p),absoluteCheckoutMentions=raw.count(str(p).encode()),qualifiedUtc=mf['qualificationUtc']))
    section.append(sections(raw));manifests.append(mf)
a,b=records;left,right=manifests
assert a['provenance']==b['provenance'] and a['configuredDependencies']==b['configuredDependencies']
assert left['commands']==right['commands'] and left['gate']==right['gate']=={'passed':64,'failed':0}
A,B=[m['inventory']['bundleFiles'] for m in manifests]
assert set(A)==set(B) and left['inventory']['fileModes']==right['inventory']['fileModes']
clock=[]
for folder in [f,r]:
    data=subprocess.check_output(['strings','-a',folder/mc.EXE]).splitlines()
    clock.append(set(data))
unique=[[s.decode('ascii','replace') for s in sorted(a-b) if len(s)>=5] for a,b in [(clock[0],clock[1]),(clock[1],clock[0])]]
print(json.dumps(dict(uniquePrintableStrings=unique,candidates=records,commands=left['commands'],equalSections=[k for k in section[0] if section[0][k]==section[1].get(k)],differentSections=[k for k in section[0] if section[0][k]!=section[1].get(k)],differentBundleFiles=[k for k in A if A[k]!=B[k]],totalBundleFiles=len(A)),indent=2))
