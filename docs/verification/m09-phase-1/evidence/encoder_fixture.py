import json,struct,subprocess,socket,wave,math,tempfile,sys
from pathlib import Path
helper, model, output = map(Path,sys.argv[1:])
a,b=socket.socketpair()
with a,b,subprocess.Popen([helper],stdin=b) as process,a.makefile('rb') as stream,tempfile.TemporaryDirectory() as tmp:
 b.close()
 def call(command,**kw):
  a.sendall((json.dumps(dict(version=11,command=command,**kw))+'\n').encode())
  r=json.loads(stream.readline());assert not r.get('error'),r.get('error');return r
 try:
  call('load',modelDirectory=str(model))
  vectors={}; rms={}
  for name,ch,amp in [('front',0,29000),('rear',5,29000),('silence',0,0),('quiet',5,1)]:
   f=Path(tmp)/(name+'.wav')
   samples=bytearray(6*2*48000)
   for i in range(48000):
    n=round(amp*math.exp(-(i%6000)/260)*math.sin(i*.31))
    struct.pack_into('<h',samples,2*(6*i+ch),n)
   with wave.open(str(f),'wb')as w:w.setparams((6,2,48000,0,'NONE',''));w.writeframes(samples)
   st=f.stat();r=call('audio',path=str(f),size=st.st_size,modified=st.st_mtime_ns,inode=st.st_ino,startSample=0,endSample=48000,sourceRate=48000,sourceSamples=48000)
   vectors[name]=r['embedding'];rms[name]=r['analysisRms']
  dot=lambda i,j:sum(a*b for a,b in zip(vectors[i],vectors[j]))
  print(rms,dot('front','rear'),dot('rear','silence'),dot('quiet','silence'),flush=True)
  assert rms['front']==rms['rear']>0 and dot('front','rear')>.99999 and dot('rear','silence')<.995 and dot('quiet','silence')>=.97
  output.write_text(json.dumps(dict(sampleRate=48000,channels=6,frames=48000,analysisRms=rms,frontRearCosine=dot('front','rear'),rearSilenceCosine=dot('rear','silence'),quietSilenceCosine=dot('quiet','silence')),indent=2)+'\n')
 finally:
  a.shutdown(socket.SHUT_WR);process.wait(timeout=10)
