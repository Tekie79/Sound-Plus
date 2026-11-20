#!/usr/bin/env python3
"""Audit the generated finite-plugin evidence using only relative archive bytes."""
import argparse, array, hashlib, json, pathlib, struct, sys, tempfile, zipfile
checks = 0
sha = lambda b: hashlib.sha256(b).hexdigest()
def require(ok, reason):
    global checks
    checks += 1
    if not ok: raise AssertionError(reason)
def raw_field(text, key):
    decoder = json.JSONDecoder(); pos = text.index('{') + 1
    while True:
        while text[pos].isspace() or text[pos] == ',': pos += 1
        name, pos = decoder.raw_decode(text, pos)
        while text[pos].isspace(): pos += 1
        require(text[pos] == ':', 'JSON field separator'); pos += 1
        while text[pos].isspace(): pos += 1
        start = pos; _, pos = decoder.raw_decode(text, pos)
        if name == key: return text[start:pos]
        require(text[pos] != '}', 'Required field ' + key)
def state_bytes(text):
    length, encoded = text.split('.', 1); length = int(length)
    require(0 <= length <= 4 * 1024 * 1024, 'Bounded opaque state')
    alphabet = '.ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+'
    value = bits = 0; output = bytearray()
    for c in encoded:
        require(c in alphabet, 'JUCE state alphabet')
        value |= alphabet.index(c) << bits; bits += 6
        while bits >= 8:
            output.append(value & 255); value >>= 8; bits -= 8
    require(len(output) >= length, 'Complete opaque state bytes')
    return bytes(output[:length])
def wav(path):
    b = path.read_bytes(); require(b[:4] == b'RIFF' and b[8:12] == b'WAVE', 'RIFF/WAVE')
    pos = 12; fmt = data = None
    while pos + 8 <= len(b):
        key, n = struct.unpack_from('<4sI', b, pos); part = b[pos+8:pos+8+n]
        require(len(part) == n, 'Finalized chunk')
        if key == b'fmt ': fmt = part
        if key == b'data': data = part
        pos += 8 + n + (n & 1)
    require(fmt is not None and data is not None, 'Format and PCM present')
    kind, channels, rate, _, align, depth = struct.unpack_from('<HHIIHH', fmt)
    mask = 0
    if kind == 65534: mask = struct.unpack_from('<I', fmt, 20)[0]; kind = struct.unpack_from('<H', fmt, 24)[0]
    require(kind == 3 and depth == 32 and align == channels*4 and len(data) % align == 0, 'Finite float32 representation')
    samples = array.array('f', data)
    if sys.byteorder != 'little': samples.byteswap()
    require(all(abs(x) < float('inf') for x in samples), 'Finite PCM')
    return samples, dict(channels=channels, rate=rate, frames=len(samples)//channels, mask=mask)
def audit(root):
    manifest = json.loads((root/'manifest.json').read_text())
    for name, digest in manifest['files'].items():
        p = pathlib.PurePosixPath(name); require(not p.is_absolute() and '..' not in p.parts, 'Relative archive member')
        require(sha((root/name).read_bytes()) == digest, 'Member hash ' + name)
    index = json.loads((root/'index.json').read_text())
    def mapped(path):
        name = index['pathMap'][path]; require(name in manifest['files'], 'Bound original path maps to retained bytes'); return root/name
    raw = (root/'processing.soundplus-process').read_text(); envelope = json.loads(raw); doc = envelope['payload']
    require(sha(raw_field(raw, 'payload').encode()) == envelope['sha256'], 'Stored processing document payload checksum')
    require(doc['version'] == 1, 'Separate processing persistence gate')
    result = json.loads((root/'result.json').read_text()); job = json.loads((root/'job.json').read_text()); receipt = json.loads((root/'receipt.json').read_text())
    require(doc['result'] == result and doc['settings'] == job and doc['receipt'] == receipt, 'Saved job/result/receipt identity')
    require(result['job'] == job and result['generation'] == job['generation'], 'Exact frozen generation/configuration')
    require(result['plugin'] == job['plugin'], 'Plugin class/version/native architecture/fingerprint identity')
    require(job['plugin']['format'] == 'VST3' and job['plugin']['architecture'] == 'arm64', 'Executed native format')
    require(sha(state_bytes(result['state'])) == result['stateHash'] == job['stateHash'], 'Complete opaque state checksum')
    require(result['state'] == job['state'], 'Exact initial processing state')
    require(result['history'] == 'reset-before-frame-zero-v1', 'Explicit fresh effect history')
    input = result['input']; require(input == job['input'] == doc['input'], 'Frozen finite input identity')
    source = input['selection']['source']; require(sha(mapped(source['path']).read_bytes()) == input['sourceHash'], 'Original generated source bytes')
    require(input['selection']['first'] == 0 and input['selection']['last'] == 4099, 'Exact original-frame range')
    require(input['selection']['inputs'] == list(range(6)) and not input['selection']['mean'], 'Ordered physical inputs')
    require(input['layout'] == job['layout'] == result['layout'], 'Same ordered bus, no implicit conversion')
    require(result['layout']['roles'] == ['L','R','C','LFE','Ls','Rs'], 'Side roles and LFE preserved')
    require(sha(mapped(input['path']).read_bytes()) == input['sha256'], 'Staged input checksum')
    a, ah = wav(mapped(input['path'])); b, bh = wav(mapped(result['path'])); expected, eh = wav(root/'expected.wav')
    require(ah == bh == eh and ah == dict(channels=6, rate=48000, frames=4099, mask=1551), 'Exact dimensions/mask/order')
    require(sha(mapped(result['path']).read_bytes()) == result['sha256'], 'Authoritative processed PCM hash')
    require(result['latencyDiscarded'] == 37 and result['tailSeconds'] == 0, 'Frame-accurate latency/tail policy')
    gain = max(abs(y + .5*x) for x,y in zip(a,b)); require(gain < 1e-6, 'Independent gain/polarity and zero-shift impulse oracle')
    oracle = max(abs(x-y) for x,y in zip(b,expected)); require(oracle < 1e-6, 'Separate expected fixture output')
    require(receipt['authoritativePcmHash'] == result['sha256'] == receipt['sha256'], 'Copy did not rerun or alter processed sound')
    require(receipt['normalizationGain'] == 1, 'Unmodified delivered representation')
    require(sha(mapped(receipt['path']).read_bytes()) == receipt['sha256'], 'Durable master bytes')
    require(len(receipt['files']) == 6, 'Complete split set')
    for c, item in enumerate(receipt['files']):
        require(item['role'] == result['layout']['roles'][c], 'Split ordered role')
        require(sha(mapped(item['path']).read_bytes()) == item['sha256'], 'Finalized split checksum')
        samples, h = wav(mapped(item['path'])); require(h['channels'] == 1 and h['frames'] == 4099 and h['rate'] == 48000, 'Split dimensions')
        require(max(abs(x-b[n*6+c]) for n,x in enumerate(samples)) < 1e-6, 'Split identity against authoritative channel')
    artifacts = json.loads((root/'artifacts.json').read_text())
    require(sha(json.dumps(artifacts['sourceFiles'], sort_keys=True, separators=(',',':')).encode()) == artifacts['compiledSourceTreeSha256'] == manifest['compiledSourceTreeSha256'], 'Exact compiled source manifest identity')
    return dict(passed=True, checks=checks, independentPcmMaxError=gain, expectedPcmMaxError=oracle, frames=4099, roles=result['layout']['roles'], compiledSourceTreeSha256=manifest['compiledSourceTreeSha256'], qualification='Generated numerical/state/file evidence. Executable/plugin hashes are recorded; their binary bytes are not redistributed. No vendor, human listening, physical surround or receiver claim.')
def main():
    ap=argparse.ArgumentParser(description=__doc__); ap.add_argument('bundle',type=pathlib.Path);ap.add_argument('--output',type=pathlib.Path);args=ap.parse_args()
    with tempfile.TemporaryDirectory() as temp:
        root=args.bundle
        if root.is_file():
            with zipfile.ZipFile(root) as z:
                require(len(z.infolist())<=64 and sum(i.file_size for i in z.infolist())<=128*1024*1024,'Bounded evidence archive')
                for item in z.infolist():
                    p=pathlib.PurePosixPath(item.filename);require(not p.is_absolute() and '..' not in p.parts,'Safe extraction member')
                z.extractall(temp)
            root=pathlib.Path(temp)
        result=audit(root)
    output=json.dumps(result,indent=2)+'\n';print(output,end='')
    if args.output:args.output.write_text(output)
if __name__=='__main__':main()
