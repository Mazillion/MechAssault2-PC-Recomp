"""Make the single-file installer: dist/MechAssault 2 Recomp Installer.exe.

  stub.exe (build_stub.cmd)  +  payload  +  trailer

The payload is the kit (../kit: installer, toolkit, source, patches, assets, defaults, enhancer, editor;
never FFmpeg, which the installer downloads on the player's PC)
and a Python 3.13 runtime (a python.org Windows install with Tcl/Tk, without site-packages, tests or
tools) under python/. Files are compressed with LZMS, the Windows Compression API's strongest codec, so
the stub can unpack them with Windows alone. Format: see stub.cpp. Normally run by tools/build_release.py.

  py -3.13 installer-exe/pack_installer.py     (or set MA2_PYTHON_HOME to the Python 3.13 folder to bundle)
"""
import concurrent.futures, ctypes, hashlib, os, struct, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
KIT = HERE.parent / 'kit'
PYTHON = Path(os.environ.get('MA2_PYTHON_HOME') or sys.base_prefix)   # the Python 3.13 that gets bundled
OUT = Path(os.environ['MA2_PACK_OUT']) if os.environ.get('MA2_PACK_OUT') else HERE.parent / 'dist' / 'MechAssault 2 Recomp Installer.exe'   # MA2_PACK_OUT: write elsewhere
KIT_DIRS = ['installer', 'toolkit', 'source', 'patches', 'assets', 'defaults', 'enhancer', 'editor']
PY_FILES = ['python.exe', 'pythonw.exe', 'python3.dll', 'python313.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'LICENSE.txt']
PY_DIRS = ['DLLs', 'Lib', 'tcl']
# Never packed: FFmpeg is downloaded on the player's PC by the installer (kit/installer/ffmpeg_fetch.py), even when a
# developer fetched a copy into the kit to build locally.
KIT_SKIP = ('source/game/vendor/ffmpeg-',)
PY_SKIP = {('Lib', 'site-packages'), ('Lib', 'test'), ('Lib', 'idlelib'), ('Lib', 'turtledemo'),
           ('tcl', 'tk8.6', 'demos')}

def files():
    out = []
    for d in KIT_DIRS:
        for p in sorted((KIT / d).rglob('*')):
            rel = p.relative_to(KIT).as_posix()
            if p.is_file() and '__pycache__' not in p.parts and p.suffix != '.pyc' and not rel.startswith(KIT_SKIP):
                out.append((rel, p))
    for n in PY_FILES:
        out.append(('python/' + n, PYTHON / n))
    for d in PY_DIRS:
        for p in sorted((PYTHON / d).rglob('*')):
            rel = p.relative_to(PYTHON).parts
            if not p.is_file() or '__pycache__' in rel or p.suffix == '.pyc': continue
            if any(rel[:len(skip)] == skip for skip in PY_SKIP): continue
            out.append(('python/' + '/'.join(rel), p))
    return out

cabinet = ctypes.WinDLL('cabinet')
cabinet.CreateCompressor.argtypes = [ctypes.c_ulong, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
cabinet.Compress.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t,
                             ctypes.POINTER(ctypes.c_size_t)]
cabinet.CloseCompressor.argtypes = [ctypes.c_void_p]

def lzms(data):
    if len(data) < 64: return 0, data
    handle = ctypes.c_void_p()
    if not cabinet.CreateCompressor(5, None, ctypes.byref(handle)): raise OSError('CreateCompressor')
    try:
        out = ctypes.create_string_buffer(len(data) + len(data) // 8 + 65536)
        size = ctypes.c_size_t()
        if not cabinet.Compress(handle, data, len(data), out, len(out), ctypes.byref(size)): raise OSError('Compress')
        packed = out.raw[:size.value]
    finally:
        cabinet.CloseCompressor(handle)
    return (1, packed) if len(packed) < len(data) else (0, data)

def main():
    if not (PYTHON / 'python313.dll').is_file() or not (PYTHON / 'tcl').is_dir():
        raise SystemExit('%s is not a python.org Python 3.13 Windows install with Tcl/Tk: run with py -3.13 or '
                         'set MA2_PYTHON_HOME.' % PYTHON)
    stub = (HERE / 'stub.exe').read_bytes()
    entries = files()
    t0 = time.time()
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        packed = list(pool.map(lambda e: lzms(e[1].read_bytes()), entries))
    index = bytearray(b'MA2KIT01' + struct.pack('<I', len(entries)))
    total = 0
    for (rel, path), (method, blob) in zip(entries, packed):
        name = rel.encode('utf-8'); size = path.stat().st_size; total += size
        index += struct.pack('<H', len(name)) + name + struct.pack('<QQB', size, len(blob), method)
    payload_hash = hashlib.sha256(index)
    for _, blob in packed: payload_hash.update(blob)
    ident = payload_hash.hexdigest()[:16].encode('ascii')
    OUT.parent.mkdir(parents=True, exist_ok=True)
    tmp = OUT.with_suffix('.tmp')
    with open(tmp, 'wb') as f:
        f.write(stub)
        offset = f.tell()
        f.write(index)
        for _, blob in packed: f.write(blob)
        size = f.tell() - offset
        f.write(struct.pack('<QQ', offset, size) + ident + b'MA2KEND1')
    tmp.replace(OUT)
    print('%s: %d files, %.1f MB unpacked -> %.1f MB (%.0fs), kit id %s' % (
        OUT.name, len(entries), total / 2**20, OUT.stat().st_size / 2**20, time.time() - t0, ident.decode()))

if __name__ == '__main__':
    main()
