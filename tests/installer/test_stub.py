"""The installer stub finds its kit when the file is Authenticode-signed (certificate table appended after the
trailer, PE security directory pointing at it, 8-byte padding before it).

    python tests/installer/test_stub.py        (needs installer-exe/stub.exe: build it with installer-exe/build_stub.cmd)
"""
import ctypes, lzma, os, random, shutil, struct, subprocess, sys, tempfile, unittest
from pathlib import Path
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import isolate                                             # noqa: E402  (11-25: never the real %LOCALAPPDATA%\MechAssault2; first, before any installer import)

REPO = Path(__file__).resolve().parents[2]
STUB = REPO / 'installer-exe' / 'stub.exe'
SECURITY = 4                                    # IMAGE_DIRECTORY_ENTRY_SECURITY


def fake_installer(payload_extra=0):
    """stub + an empty kit index (+ filler, so the length is not a multiple of 8) + trailer."""
    stub = STUB.read_bytes()
    payload = b'MA2KIT01' + struct.pack('<I', 0) + b'\0' * payload_extra
    trailer = struct.pack('<QQ', len(stub), len(payload)) + b'0123456789abcdef' + b'MA2KEND1'
    return bytearray(stub + payload + trailer)


def security_dir_offset(data):
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    assert data[pe:pe + 4] == b'PE\0\0'
    opt = pe + 4 + 20
    magic = struct.unpack_from('<H', data, opt)[0]
    return opt + (112 if magic == 0x20B else 96) + 8 * SECURITY


def sign(data):
    """Append a dummy WIN_CERTIFICATE the way signtool does: pad to 8 bytes, then the table; point the directory at it."""
    data = bytearray(data)
    data += b'\0' * (-len(data) % 8)
    cert_offset = len(data)
    blob = os.urandom(512) + b'MA2KEND1' + b'\0' * 8          # a stray magic inside the certificate must not confuse it
    entry = struct.pack('<IHH', 8 + len(blob), 0x0200, 0x0002) + blob
    data += entry
    struct.pack_into('<II', data, security_dir_offset(data), cert_offset, len(entry))
    return data


def lzms(data):
    cab = ctypes.WinDLL('cabinet'); h = ctypes.c_void_p()
    cab.CreateCompressor.argtypes = [ctypes.c_ulong, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
    cab.Compress.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    assert cab.CreateCompressor(5, None, ctypes.byref(h))
    out = ctypes.create_string_buffer(len(data) + 65536); n = ctypes.c_size_t()
    assert cab.Compress(h, data, len(data), out, len(out), ctypes.byref(n))
    cab.CloseCompressor(h)
    return out.raw[:n.value]


def installer_with(files):
    """stub + a kit of `files` [(path, data, method)] packed as pack_installer.py does (0 stored, 1 LZMS, 2 LZMA) + trailer."""
    index = bytearray(b'MA2KIT01' + struct.pack('<I', len(files))); blobs = []
    for path, data, method in files:
        blob = data if method == 0 else lzms(data) if method == 1 else lzma.compress(data, format=lzma.FORMAT_ALONE, preset=9)
        name = path.encode('utf-8')
        index += struct.pack('<H', len(name)) + name + struct.pack('<QQB', len(data), len(blob), method); blobs.append(blob)
    stub = STUB.read_bytes(); payload = bytes(index) + b''.join(blobs)
    return stub + payload + struct.pack('<QQ', len(stub), len(payload)) + b'fedcba9876543210' + b'MA2KEND1'


@unittest.skipUnless(STUB.is_file(), 'build installer-exe/stub.exe first (installer-exe/build_stub.cmd)')
class StubTrailer(unittest.TestCase):
    def setUp(self):
        base = REPO / 'tests' / 'tmp'; base.mkdir(parents=True, exist_ok=True)
        self.tmp = Path(tempfile.mkdtemp(prefix='stub-', dir=str(base)))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def check(self, data, name):
        exe = self.tmp / name
        exe.write_bytes(bytes(data))
        r = subprocess.run([str(exe), '--check-payload'], capture_output=True, text=True, timeout=60)
        return r.returncode, (r.stdout + r.stderr).strip()

    def test_unsigned(self):
        code, out = self.check(fake_installer(3), 'plain.exe')
        self.assertEqual(code, 0, out); self.assertIn('payload: ok', out); self.assertIn('id 0123456789abcdef', out)

    def test_signed_with_padding(self):
        for extra in (0, 1, 3, 7):                 # every padding length signtool can insert
            code, out = self.check(sign(fake_installer(extra)), 'signed%d.exe' % extra)
            self.assertEqual(code, 0, '%d: %s' % (extra, out)); self.assertIn('payload: ok', out)

    def test_appended_data_without_a_certificate_is_refused(self):
        code, out = self.check(fake_installer(2) + b'junk at the end', 'damaged.exe')
        self.assertEqual(code, 1, out); self.assertIn('not found', out)


    def test_unpack_lzma_and_lzms_give_the_same_files(self):
        """1.2.1: the kit is packed with LZMA (method 2, the stub's own decoder, which also runs under Wine/Proton); LZMS kits
        (method 1, 1.2.0 and earlier) still unpack. Every file comes out byte for byte."""
        rnd = random.Random(5)
        data = [('a/zeros.bin', bytes(300000)), ('b/text.py', (REPO / 'kit' / 'installer' / 'install.py').read_bytes()),
                ('c/random.bin', bytes(rnd.getrandbits(8) for _ in range(70000))), ('d/tiny.txt', b'x' * 10)]
        for method in (1, 2):
            exe = self.tmp / ('m%d.exe' % method)
            exe.write_bytes(installer_with([(p, d, 0 if len(d) < 64 else method) for p, d in data]))
            out = self.tmp / ('out%d' % method)
            r = subprocess.run([str(exe), '--unpack-to', str(out)], capture_output=True, text=True, timeout=120, creationflags=0x08000000)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr); self.assertIn('unpack: ok', r.stderr)
            for p, d in data: self.assertEqual((out / p).read_bytes(), d, p)
            self.assertTrue((out / '.unpacked').is_file())

    def test_damaged_lzma_is_refused(self):
        good = bytearray(installer_with([('f.bin', bytes(range(256)) * 400, 2)]))
        good[len(STUB.read_bytes()) + 60] ^= 0x55              # inside the packed data, after the index
        exe = self.tmp / 'bad.exe'; exe.write_bytes(bytes(good))
        r = subprocess.run([str(exe), '--unpack-to', str(self.tmp / 'o')], capture_output=True, text=True, timeout=120, creationflags=0x08000000)
        self.assertEqual(r.returncode, 1); self.assertIn('damaged', r.stderr)

    def test_unpack_needs_an_empty_folder(self):
        exe = self.tmp / 'k.exe'; exe.write_bytes(installer_with([('f.txt', b'hello' * 30, 2)]))
        (self.tmp / 'full').mkdir(); (self.tmp / 'full' / 'keep.txt').write_text('mine')
        r = subprocess.run([str(exe), '--unpack-to', str(self.tmp / 'full')], capture_output=True, text=True, timeout=60, creationflags=0x08000000)
        self.assertEqual(r.returncode, 1); self.assertEqual((self.tmp / 'full' / 'keep.txt').read_text(), 'mine')


if __name__ == '__main__':
    unittest.main(verbosity=2)
