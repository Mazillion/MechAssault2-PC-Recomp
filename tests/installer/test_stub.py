"""The installer stub finds its kit when the file is Authenticode-signed (certificate table appended after the
trailer, PE security directory pointing at it, 8-byte padding before it).

    python tests/installer/test_stub.py        (needs installer-exe/stub.exe: build it with installer-exe/build_stub.cmd)
"""
import os, shutil, struct, subprocess, sys, tempfile, unittest
from pathlib import Path

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


if __name__ == '__main__':
    unittest.main(verbosity=2)
