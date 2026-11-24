"""Crash only the isolated loopback fixture after its own POST has arrived."""
from pathlib import Path
import importlib.util, json, hashlib, signal, subprocess, sys, threading, time, tempfile
fixture,exe,root=map(Path,sys.argv[1:])
spec=importlib.util.spec_from_file_location('local_fixture',fixture)
fx=importlib.util.module_from_spec(spec);spec.loader.exec_module(fx)
p=Path(tempfile.mkdtemp(dir=root))
with fx.ThreadingHTTPServer(('127.0.0.1',0),fx.Handler) as server:
    threading.Thread(target=server.serve_forever,daemon=True).start()
    args=[exe,'--sfx',p,'http://127.0.0.1:'+str(server.server_port)]
    child=subprocess.Popen(args+['crash'])
    try:
        assert fx.STARTED.wait(8) and child.poll() is None
        child.send_signal(signal.SIGABRT)
        assert child.wait(timeout=15)==-signal.SIGABRT
    finally:
        if child.poll() is None: child.kill();child.wait()
    subprocess.run(args+['recover'],check=True)
    assert fx.COUNT['interrupt']==1
    for _ in range(60):
        for path in (Path.home()/'Library/Logs/DiagnosticReports').glob('SoundPlusTests*.ips'):
            raw=path.read_bytes()
            try: meta,body=raw.split(b'\n',1);d=json.loads(body)
            except (ValueError,TypeError):continue
            if d.get('pid')!=child.pid or d.get('procName')!=exe.name:continue
            uuid=json.loads(meta)['slice_uuid']
            assert uuid.lower() in subprocess.check_output(['dwarfdump','--uuid',exe],text=True).lower()
            assert fx.KEY.encode() not in raw and fx.KEY.encode('utf-16-le') not in raw
            reduced={'pid':child.pid,'captured':d.get('captureTime'),'incident':d.get('incident'),'binary':str(exe),'imageUUID':uuid,'rawBytes':len(raw),'rawSha256':hashlib.sha256(raw).hexdigest(),'exception':d['exception'],'triggeredFrames':[s.get('symbol','(unsymbolicated)') for t in d['threads'] if t.get('triggered') for s in t['frames']][:12],'loopbackPosts':fx.COUNT['interrupt'],'rawTokenMatches':0}
            text=json.dumps(reduced,indent=2)
            assert fx.KEY not in text;print(text);sys.exit()
        time.sleep(1)
    raise RuntimeError('No matching macOS diagnostic')
