"""Launch the unmodified frozen tests through CTest inside an exclusive QA root."""
from pathlib import Path
import sys, resource, subprocess, tempfile, json
if sys.argv[1] == 'limits':
    print('CTest child RLIMIT_STACK bytes', resource.getrlimit(resource.RLIMIT_STACK))
    sys.exit(0)
binary, app, ctest, root = map(Path,sys.argv[1:])
stack=resource.getrlimit(resource.RLIMIT_STACK)
print('launcher RLIMIT_STACK bytes', stack, flush=True)
assert stack[0] == 65520*1024, "Start through zsh with ulimit -s hard"
p=Path(tempfile.mkdtemp(dir=root))
spec=['add_test(limits "'+sys.executable+'" "'+str(Path(__file__).resolve())+'" limits)']
for i,(flag,basename) in enumerate([('--multichannel','multichannel-test-output'),('--multichannel','multichannel-test-output'),('--similar-integrity','similar-test-output')]):
    folder=Path(tempfile.mkdtemp(dir=p))/basename
    assert not folder.exists() and folder.resolve().is_relative_to(p) and folder.name==basename
    spec.append(f'add_test(run{i} "{binary}" "{flag}" "{folder}" "{app}")')
(p/'CTestTestfile.cmake').write_text('\n'.join(spec)+'\n')
subprocess.run([ctest,'--test-dir',p,'-V'],check=True)
