"""Loopback-only provider and independent PCM oracle for the native generation service."""
import hashlib
import io
import json
from collections import Counter
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import time
import wave

KEY = "fixture-secret-NEVER-PERSIST"
COUNT = Counter()
STARTED = threading.Event()
AUDIO = io.BytesIO()
with wave.open(AUDIO, "wb") as w:
    w.setparams((2, 2, 44100, 0, "NONE", "not compressed"))
    w.writeframes(b"".join(struct.pack("<hh", n % 17000 - 8500, 8500 - n % 17000) for n in range(22050)))
MP3 = (Path(__file__).parent / "fixtures/generation-tone.mp3").read_bytes()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        assert self.path == "/v1/user" and self.headers["xi-api-key"] == KEY
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b'{"user_id":"fixture"}')

    def do_POST(self):
        assert self.path.startswith("/v1/sound-generation?output_format=")
        assert self.headers["xi-api-key"] == KEY
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        assert request["model_id"] == "eleven_text_to_sound_v3"
        assert set(request) <= {"text", "model_id", "duration_seconds", "prompt_influence", "loop"}
        prompt = request["text"]
        COUNT[prompt] += 1
        if prompt in ("interrupt", "slow"):
            if prompt == "interrupt":
                STARTED.set()
            time.sleep(2)
        code = int(prompt) if prompt.isdigit() else 200
        wav = self.path.endswith("wav_44100")
        body = AUDIO.getvalue() if wav else MP3
        if prompt == "tone":
            assert request["duration_seconds"] == .5 and request["loop"] and request["prompt_influence"] == .7
        if prompt == "mp3":
            assert "duration_seconds" not in request and not wav
        if code != 200:
            body = KEY.encode()  # Provider error echo must never reach disk or UI.
        if prompt == "truncated":
            body = body[:-11]
        if prompt == "empty":
            body = b""
        if prompt == "disconnect":
            return
        self.send_response(code)
        self.send_header("Content-Type", "text/html" if prompt == "mime" else "audio/wav" if wav else "audio/mpeg")
        self.send_header("request-id", "abc123")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (ConnectionError, BrokenPipeError):
            pass


def verify_pcm(folder):
    db = sqlite3.connect(folder / "jobs/history.sqlite")
    rows = [json.loads(record) for record, in db.execute("SELECT record FROM jobs")]
    tones = [r for r in rows if r["options"]["prompt"] == "tone"]
    assert len(tones) == 3
    for r in tones:
        local = folder / "jobs" / (r["id"] + ".wav")
        wav = local.read_bytes()
        assert hashlib.sha256(wav).hexdigest() == r["sha256"]
        i = 12
        while wav[i:i+4] != b"data":
            size, = struct.unpack_from("<I", wav, i + 4)
            i += 8 + size + size % 2
        pcm = wav[i+8:i+8+22050*6]
        for n in range(22050):
            l = int.from_bytes(pcm[n*6:n*6+3], "little", signed=True)
            rr = int.from_bytes(pcm[n*6+3:n*6+6], "little", signed=True)
            assert abs(l - 256*(n % 17000-8500)) <= 1 and abs(rr+l) <= 2
    assert COUNT["tone"] == 3 and COUNT["interrupt"] == 1
    assert all(COUNT[k] == 1 for k in ("401","402","403","429","500","disconnect","mp3","truncated","empty","mime","slow"))
    for p in folder.rglob("*"):
        if p.is_file():
            assert KEY.encode() not in p.read_bytes(), f"secret in {p}"
    print("Independent oracle passed for 132300 canonical PCM samples, SHA-256, request counts and redaction.")


def main():
    exe, parent = sys.argv[1:]
    Path(parent).mkdir(parents=True, exist_ok=True)
    folder = Path(tempfile.mkdtemp(dir=parent)).resolve()
    with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        args = [exe, "--sfx", str(folder), f"http://127.0.0.1:{server.server_port}"]
        subprocess.run(args + ["suite"], check=True, timeout=45)
        child = subprocess.Popen(args + ["crash"])
        try:
            assert STARTED.wait(8)
        finally:
            child.kill()
            child.wait()
        subprocess.run(args + ["recover"], check=True, timeout=8)
        verify_pcm(folder)


if __name__ == "__main__":
    main()
