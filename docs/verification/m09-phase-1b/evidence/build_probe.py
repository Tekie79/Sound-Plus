"""Compile an external instrument with the frozen build's own flags and objects."""
import sys,shlex,subprocess,hashlib
from pathlib import Path
build,ninja,work=map(lambda p:Path(p).resolve(),sys.argv[1:4])
src=Path(__file__).with_name(sys.argv[4] if len(sys.argv)>4 else 'probe.cpp').resolve()
target='SoundPlusTests_artefacts/Release/SoundPlusTests'
commands=subprocess.check_output([ninja,'-C',build,'-t','commands',target],text=True).splitlines()
compile=shlex.split(next(l for l in commands if ' -c ' in l and l.endswith('/tests/MixerGraphTests.cpp')))
obj=str(work/'probe.o')
subprocess.run(compile[:compile.index('-MD')]+['-o',obj,'-c',src],cwd=build,check=True)
link=shlex.split(commands[-1].split(' && ')[1])
link=[x for x in link if not (x.endswith('.o') and '/tests/' in x)]
link.insert(link.index('-o'),obj);link[link.index('-o')+1]=str(work/'probe')
subprocess.run(link,cwd=build,check=True)
for p in [src,build/'libsoundplus_core.a',build/target,work/'probe']:
 print(p.name,hashlib.file_digest(p.open('rb'),'sha256').hexdigest())
