"""Keep installer tests out of the real %LOCALAPPDATA%\\MechAssault2 (11-25).

Every tests/installer suite imports this first. The installer code (precheck.user_data_dir, report.store_path, download's
cache, the window's log folder, wine_host) and MA2 Report.exe all read the LOCALAPPDATA variable when they are called, so
the variable is the one route to the player's folder. On import:

- LOCALAPPDATA that is this user's real Local AppData folder (or missing) is replaced by a new folder in tests/tmp. A value a
  runner already set to a test folder (tools/release.py, the gate's run_suites) is kept.
- An audit hook refuses (PermissionError) any open, listing, create, rename, copy or delete of a path inside the real
  %LOCALAPPDATA%\\MechAssault2 for the rest of the process, and lists each refusal on stderr at exit, so a test that builds
  that path some other way fails instead of reading or writing the player's files.

test_isolation.py checks that every suite imports this module and that no installer path resolves inside the real folder.
The installer itself is unchanged: without this module it uses LOCALAPPDATA as before."""
import atexit, ctypes, os, sys, tempfile
from pathlib import Path

WORK = Path(__file__).resolve().parents[1]                 # tests/


def _known_local():
    """This user's Local AppData folder from Windows itself (SHGetKnownFolderPath), whatever LOCALAPPDATA says."""
    try:
        class GUID(ctypes.Structure):
            _fields_ = [('a', ctypes.c_uint32), ('b', ctypes.c_uint16), ('c', ctypes.c_uint16), ('d', ctypes.c_ubyte * 8)]
        g = GUID(0xF1B32785, 0x6FBA, 0x4FCF, (ctypes.c_ubyte * 8)(0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91))   # FOLDERID_LocalAppData
        out = ctypes.c_wchar_p()
        if ctypes.windll.shell32.SHGetKnownFolderPath(ctypes.byref(g), 0, None, ctypes.byref(out)) == 0:
            try:
                return out.value
            finally:
                ctypes.windll.ole32.CoTaskMemFree(out)
    except (AttributeError, OSError):
        pass
    return None


def _norm(p):
    return os.path.normcase(os.path.abspath(os.fspath(p))).rstrip('\\/')


_REALS = []
for _c in (_known_local(), str(Path(os.environ.get('USERPROFILE') or Path.home()) / 'AppData' / 'Local')):
    if _c and _norm(_c) not in _REALS:
        _REALS.append(_norm(_c))
REAL_LOCAL = _REALS[0] if _REALS else None
REAL = [_r + os.sep + 'mechassault2' for _r in _REALS]        # normcase lowercases on Windows


def inside_real(path):
    """True when `path` is the real %LOCALAPPDATA%\\MechAssault2 or anything in it."""
    try:
        n = _norm(path)
    except (TypeError, ValueError):
        return False
    return any(n == r or n.startswith(r + os.sep) for r in REAL)


_cur = os.environ.get('LOCALAPPDATA')
if not _cur or _norm(_cur) in _REALS:
    (WORK / 'tmp').mkdir(exist_ok=True)
    os.environ['LOCALAPPDATA'] = tempfile.mkdtemp(prefix='ma2-isolated-local-', dir=str(WORK / 'tmp'))

BLOCKED = []
_EVENTS = {'open', 'os.listdir', 'os.scandir', 'os.mkdir', 'os.remove', 'os.rename', 'os.rmdir', 'os.chmod', 'os.utime',
           'os.truncate', 'os.link', 'os.symlink', 'os.chdir', 'os.startfile', 'shutil.copyfile', 'shutil.copymode',
           'shutil.copystat', 'shutil.copytree', 'shutil.move', 'shutil.rmtree', 'shutil.make_archive', 'shutil.unpack_archive'}


def _hook(event, args):
    if event not in _EVENTS:
        return
    for a in args:
        if isinstance(a, (str, bytes, os.PathLike)):
            try:
                s = os.fsdecode(a)
            except (TypeError, ValueError):
                continue
            if s and inside_real(s):
                BLOCKED.append('%s %s' % (event, s))
                raise PermissionError('installer test tried to use the real %%LOCALAPPDATA%%\\MechAssault2 (%s %s)' % (event, s))


def _report():
    if BLOCKED:
        sys.stderr.write('isolate: %d access(es) to the real %%LOCALAPPDATA%%\\MechAssault2 refused:\n  %s\n'
                         % (len(BLOCKED), '\n  '.join(BLOCKED[:20])))


if REAL and not getattr(sys, '_ma2_isolate_hook', False):
    sys.addaudithook(_hook)
    sys._ma2_isolate_hook = True
    atexit.register(_report)
