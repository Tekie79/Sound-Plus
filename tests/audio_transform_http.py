"""Exercise native Stability multipart, durable polling and source-mask precision offline."""
import email.parser
import email.policy
import hashlib
import io
import json
import pathlib
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

base = pathlib.Path(sys.argv[2])
base.mkdir(parents=True, exist_ok=True)
root = pathlib.Path(tempfile.mkdtemp(dir=base))
with wave.open(str(root / "input.wav"),"wb") as f:
    f.setparams((1,2,44100,0,"NONE",""))
    f.writeframes(struct.pack("<h",2048)*264600)
stream=io.BytesIO()
with wave.open(stream,"wb") as f:
    f.setparams((2,2,44100,0,"NONE",""))
    f.writeframes(struct.pack("<hh",8192,12288)*264600)
body=stream.getvalue()
seen=threading.Event()
posts=[]
nextid={}
class Receiver(BaseHTTPRequestHandler):
    def log_message(self,*args): pass
    def send(self,code,data,mime):
        self.send_response(code)
        self.send_header("Content-Type",mime)
        self.send_header("Content-Length",str(len(data)))
        self.end_headers()
        self.wfile.write(data)
    def do_POST(self):
        assert self.headers["Authorization"]=="Bearer qa-transform"
        raw=self.rfile.read(int(self.headers["Content-Length"]))
        m=email.parser.BytesParser(policy=email.policy.default).parsebytes(
            ("Content-Type: "+self.headers["Content-Type"]+"\r\n\r\n").encode()+raw)
        parts={p.get_param("name",header="content-disposition"):p.get_payload(decode=True) for p in m.iter_parts()}
        name=parts["prompt"].decode()
        assert parts["model"]==b"stable-audio-3" and parts["output_format"]==b"wav" and float(parts["duration"])==6
        assert parts["audio"][:4]==b"RIFF" and len(parts["audio"])>264600*4
        if name=="restyle":
            assert self.path.endswith("/audio-to-audio") and float(parts["strength"])==.625
        else:
            assert self.path.endswith("/inpaint") and abs(float(parts["mask_start"])*44100-300)<.001
            assert abs(float(parts["mask_end"])*44100-600)<.001
        posts.append(name)
        key="abcd%02d"%len(posts)
        nextid[key]=name
        self.send(202,json.dumps({"id":key}).encode(),"application/json")
    def do_GET(self):
        assert self.headers["Authorization"]=="Bearer qa-transform"
        assert self.path.startswith("/v2beta/audio/results/")
        name=nextid[self.path.rsplit("/",1)[1]]
        if name=="crash" and not seen.is_set():
            self.send(202,b"{}","application/json")
            seen.set()
        elif name=="expired": self.send(404,b"{}","application/json")
        elif name=="corrupt": self.send(200,b"bad","audio/wav")
        else: self.send(200,body,"audio/wav")

with ThreadingHTTPServer(("127.0.0.1",0),Receiver) as server:
    threading.Thread(target=server.serve_forever,daemon=True).start()
    args=[sys.argv[1],"--transform-jobs",str(root),f"http://127.0.0.1:{server.server_port}"]
    child=subprocess.Popen(args+["crash"])
    try: assert seen.wait(12)
    finally:
        child.kill()
        child.wait()
    for action in ("wrong","resume","restyle","expired","corrupt"):
        subprocess.run(args+[action],check=True,timeout=20)
assert posts==["crash","restyle","expired","corrupt"],posts
db=sqlite3.connect(root/"jobs/history.sqlite")
rows=[json.loads(t[0]) for t in db.execute("select record from jobs")]
c=next(r for r in rows if r["options"]["prompt"]=="crash")
assert c["rawHash"]==hashlib.sha256(body).hexdigest()
wav=(root/"jobs"/(c["id"]+".wav")).read_bytes()
assert hashlib.sha256(wav).hexdigest()==c["sha256"]
i=wav.index(b"data")+8
values=[x[0] for x in struct.iter_unpack("<f",wav[i:i+264600*4])]
assert all(abs(v-(.3125 if 300<=j<600 else .0625))<1e-6 for j,v in enumerate(values))
assert all(b"qa-transform" not in f.read_bytes() for f in root.rglob("*") if f.is_file())
print("PASS real multipart serialization, confirmed SHA-256, one POST across kill/reopen, account binding, expired/corrupt provider responses and 264600 composite oracle frames")
