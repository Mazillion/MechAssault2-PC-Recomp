"""Checks the installer runs before it installs anything (pure functions; the wizard and install.py share them).

   clean_path / is_absolute   what the user typed or pasted ("Copy as path" adds quotes)
   validate_image             is this file the supported MechAssault 2 disc image? exact reasons if not
   check_folder               is this a sensible, writable install folder with enough space?
   detect_gpus / recommend    graphics cards and VRAM, and the texture option that suits them
   estimate_minutes           rough install time from CPU threads and available RAM
   machine facts              Windows, battery, Smart App Control, DirectX 11
   merge_ini                  add-only merge of new settings keys into a user's pc_settings.ini
   support_bundle             text for "Copy log" (user name replaced by default)
"""
import ctypes, hashlib, json, os, re, shutil, struct, sys, tempfile, time
from pathlib import Path

GB = 1024 ** 3
HERE = Path(__file__).resolve().parent
KIT = HERE.parent
# What the supported disc's ma2.xbe certificate says (title id "MS" 0x006B, version 6, all regions).
SUPPORTED_TITLE_ID, SUPPORTED_VERSION, SUPPORTED_REGION = 0x4D53006B, 6, 7
STAMP = 'recomp-install.json'
# D-11: the user fills in the support/issue-tracker URL here (one place for the whole installer)
MA2_SUPPORT_URL = 'https://github.com/Mazillion/MechAssault2-PC-Recomp/issues'
SUPPORT_FALLBACK = 'the page you downloaded the installer from'
# The one wording for "what the installer carries" (W-53; the same sentences are in stub.rc, install.py's docstring and the README).
NO_GAME_DATA = ('No game data files are included: the game\'s program, art, sound and levels come from your own disc.')
SHADER_LINE = 'Graphics shaders are compiled on your PC as you play; none come with the installer.'
FAN_LINE = ('Unofficial fan project. Not affiliated with or endorsed by Microsoft, Xbox, Day 1 Studios or the '
            'BattleTech/MechWarrior rights holders. MechAssault and Xbox are trademarks of their respective owners. '
            + NO_GAME_DATA + ' ' + SHADER_LINE)

# ---------------------------------------------------------------------------- places

def support_where():
    """Where a problem report goes: MA2_SUPPORT_URL, or words pointing at the download page until a URL exists."""
    return MA2_SUPPORT_URL or SUPPORT_FALLBACK

def report_hint():
    """The sentence that tells a user how to report a failure (used by the failure screen's advice)."""
    if MA2_SUPPORT_URL: return 'press Copy log, then Report a problem, and paste the log into the report'
    return 'press Copy log and paste it into a message on %s' % SUPPORT_FALLBACK

def local_dir():
    """%LOCALAPPDATA%\\MechAssault2 (settings, saves, logs, installer.json), read at call time."""
    base = os.environ.get('LOCALAPPDATA') or str(Path.home() / 'AppData' / 'Local')
    return Path(base) / 'MechAssault2'

def session_id():
    """The id of this installer run (MA2_SESSION_ID, set by the installer exe; made here when run without it). Every log names it, and
    the processes the installer starts (the tools, the Build Tools, the game from Play) inherit it, so one report's files match."""
    sid = os.environ.get('MA2_SESSION_ID', '').strip()
    if not sid:
        try:
            sid = '%08X' % (int.from_bytes(os.urandom(4), 'little'))
            os.environ['MA2_SESSION_ID'] = sid
        except Exception:
            return '-'
    return sid

def diagnostics_dir():
    """%LOCALAPPDATA%\\MechAssault2\\Diagnostics: where diagnostics go by default from 1.1.4 (never a folder Controlled Folder Access guards)."""
    return local_dir() / 'Diagnostics'

def logs_dir():
    return local_dir() / 'Logs'

PLAIN_CHARS = frozenset('ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 _.-\\:')

def plain_path(path):
    """Can the C++ build run under this path? A whitelist: letters, digits, spaces, - _ . and the drive / folder separators.
    Anything else (accented letters, commas, brackets, & % ; # ! ^ $ ' ...) can break cmd.exe, CMake or link.exe options
    (link.exe splits options at commas), so the build then runs in a plain folder such as C:\\MA2RecompBuild-xxxx."""
    return all(c in PLAIN_CHARS for c in str(path))

def is_install(dest):
    dest = Path(dest)
    return all((dest / p).is_file() for p in ('MechAssault 2.exe', 'game_files/ma2.xbe', 'pc_assets/common.mgf'))

def read_json(path):
    try:
        return json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None

def read_stamp(dest):
    return read_json(Path(dest) / STAMP)

def read_state(dest):
    return read_json(Path(dest) / '_work' / 'state.json')

def remembered_install():
    """The folder of the last install this PC made (installer.json), if it still exists."""
    j = read_json(local_dir() / 'installer.json')
    p = j.get('dest') if isinstance(j, dict) else None
    return p if p and Path(p).exists() else None

# ---------------------------------------------------------------------------- what the user typed

def clean_path(text):
    """Strip whitespace and the quotes Explorer's "Copy as path" adds; expand %VARS%."""
    s = (text or '').strip().strip('"\u201c\u201d').strip()
    return os.path.expandvars(s) if '%' in s else s

def is_absolute(text):
    return bool(re.match(r'^[A-Za-z]:[\\/]', text or '') or (text or '').startswith('\\\\'))

# ---------------------------------------------------------------------------- disc image

class ImageResult:
    def __init__(self, ok, code, message, headline='', info=None):
        self.ok, self.code, self.message, self.headline, self.info = ok, code, message, headline, info or {}
    def __repr__(self):
        return 'ImageResult(%s, %s)' % (self.ok, self.code)

def _xiso():
    for root in (KIT / 'toolkit', KIT.parents[0]):
        if (root / 'tools' / 'xiso' / 'xdvdfs.py').is_file():
            if str(root) not in sys.path: sys.path.append(str(root))
            break
    from tools.xiso.xdvdfs import Xiso, XisoError
    return Xiso, XisoError

def xbe_certificate(head):
    """(title_id, region, version) from the first bytes of an XBE, or None."""
    try:
        if head[:4] != b'XBEH': return None
        base = struct.unpack_from('<I', head, 0x104)[0]; cert = struct.unpack_from('<I', head, 0x118)[0] - base
        if not 0 < cert < len(head) - 0xB0: return None
        return (struct.unpack_from('<I', head, cert + 8)[0], struct.unpack_from('<I', head, cert + 0xA0)[0],
                struct.unpack_from('<I', head, cert + 0xAC)[0])
    except struct.error:
        return None

MAGICS = ((b'PK\x03\x04', 'zip'), (b'7z\xbc\xaf\x27\x1c', '7z'), (b'Rar!', 'rar'), (b'\x1f\x8b', 'gz'))

def _judge_xbe(data, release, info, ok_message, what='disc image'):
    """The verdict on the bytes of ma2.xbe, whether they came from a disc image or from a folder copied from the disc."""
    got = hashlib.sha256(data).hexdigest()
    info = dict(info, sha=got)
    if got == release['xbe_sha256']:
        return ImageResult(True, 'ok', ok_message, 'MechAssault 2 (US retail) recognised', info)
    cert = xbe_certificate(data[:0x2000])
    if cert and cert[0] != SUPPORTED_TITLE_ID:
        return ImageResult(False, 'wrong_title', 'This is an Xbox %s, but not MechAssault 2 (it looks like title ID %08X).'
                           % (what, cert[0]), 'Not MechAssault 2', info)
    if not cert or cert[2] != SUPPORTED_VERSION or cert[1] != SUPPORTED_REGION:
        return ImageResult(False, 'wrong_version', 'This is MechAssault 2, but a different region or version (PAL/European, '
                           'Japanese or a demo). This installer supports the US (NTSC) retail release only, because the PC '
                           'fixes are made for that exact program.', 'Different region or version', info)
    return ImageResult(False, 'modified', 'This copy of ma2.xbe is modified or patched (hash %s...). Use an unmodified dump '
                       'of your retail disc.' % got[:6], 'Modified game file', info)

def validate_folder(folder, release):
    """A folder copied from the disc (what an extraction tool makes): ma2.xbe must be in it and be the supported one."""
    folder = Path(folder)
    xbe = next((p for p in folder.iterdir() if p.is_file() and p.name.lower() == release['xbe'].lower()), None) if folder.is_dir() else None
    if xbe is None:
        return ImageResult(False, 'no_xbe', 'This folder has no %s in it. Choose the folder that holds the files copied from the MechAssault 2 disc '
                           '(the one with %s inside), or a disc image.' % (release['xbe'], release['xbe']), 'Not a MechAssault 2 folder')
    try:
        data = xbe.read_bytes()
    except OSError as e:
        return ImageResult(False, 'read_error', 'Couldn\'t read the file: %s.' % (e.strerror or e), 'Can\'t read the file')
    return _judge_xbe(data, release, {'folder': str(folder)}, 'Game folder OK: ma2.xbe matches the supported version.', 'game folder')

def validate_image(path, release):
    """Open the disc image in-process and say exactly what is wrong with it (or that it is the supported disc)."""
    path = Path(clean_path(str(path)))
    if path.is_dir(): return validate_folder(path, release)
    if path.is_file() and path.name.lower() == release['xbe'].lower(): return validate_folder(path.parent, release)     # ma2.xbe itself: its folder
    if not path.is_file():
        return ImageResult(False, 'missing', 'That file was not found.', 'File not found')
    try:
        size = path.stat().st_size
        with open(path, 'rb') as f: head = f.read(8)
    except OSError as e:
        return ImageResult(False, 'read_error', 'Couldn\'t read the file: %s.' % (e.strerror or e), 'Can\'t read the file')
    if size == 0:
        return ImageResult(False, 'empty', 'This file is empty.', 'Empty file')
    for magic, name in MAGICS:
        if head.startswith(magic):
            return ImageResult(False, 'archive', 'This is a compressed archive (.%s). Extract the .iso/.xiso from it first, '
                               'then choose that file.' % name, 'Compressed archive')
    if head[:4] in (b'CISO', b'ZISO') or path.suffix.lower() in ('.cso', '.cci'):
        return ImageResult(False, 'compressed_image', '.cso / .cci compressed images aren\'t supported. Use a plain .iso or .xiso.',
                           'Compressed image')
    Xiso, XisoError = _xiso()
    try:
        iso = Xiso(str(path))
    except XisoError:
        import gc; gc.collect()                    # the failed constructor leaves the file open
        return ImageResult(False, 'not_xbox', 'This file isn\'t an original-Xbox disc image. Choose a .iso or .xiso made '
                           'from the MechAssault 2 disc.', 'Not an Xbox disc image')
    except OSError as e:
        return ImageResult(False, 'read_error', 'Couldn\'t read the file: %s.' % (e.strerror or e), 'Can\'t read the file')
    try:
        try:
            entry = iso.find(release['xbe'], '')
            if entry is None:
                other = iso.find('default.xbe', '')
                title = ''
                if other is not None:
                    cert = xbe_certificate(iso._read_at(other.sector, min(other.size, 0x2000)))
                    if cert: title = ' (it looks like title ID %08X)' % cert[0]
                return ImageResult(False, 'no_xbe', 'This is an Xbox disc image, but not MechAssault 2%s.' % title,
                                   'Not MechAssault 2', {'title_id': title})
            end = 0
            for _, e in iso.walk(): end = max(end, iso.base + e.sector * 2048 + e.size)
            if end > size:
                return ImageResult(False, 'truncated', 'The image is incomplete or damaged (it ends early). Dump the disc again.',
                                   'Incomplete image')
            data = iso._read_at(entry.sector, entry.size)
        except (OSError, XisoError, struct.error) as e:
            return ImageResult(False, 'read_error', 'Couldn\'t read the file: %s.' % e, 'Can\'t read the file')
        if len(data) < entry.size:
            return ImageResult(False, 'truncated', 'The image is incomplete or damaged (it ends early). Dump the disc again.',
                               'Incomplete image')
        return _judge_xbe(data, release, {'size': size, 'base': iso.base},
                          'Disc image OK: %.1f GB, ma2.xbe matches the supported version.' % (size / GB))
    finally:
        iso.close()

# ---------------------------------------------------------------------------- install folder

class Finding:
    def __init__(self, level, code, text, fix=None):
        self.level, self.code, self.text, self.fix = level, code, text, fix        # level: error | warn | info
    def __repr__(self):
        return 'Finding(%s, %s)' % (self.level, self.code)

class FolderReport:
    def __init__(self):
        self.findings = []; self.free = None; self.existing = 'none'; self.stamp = None; self.state = None; self.suggest = None
    @property
    def ok(self):
        return not any(f.level == 'error' for f in self.findings)
    def codes(self):
        return [f.code for f in self.findings]
    def first_error(self):
        return next((f for f in self.findings if f.level == 'error'), None)

def _under(path, base):
    if not base: return False
    try:
        p, b = os.path.normcase(os.path.abspath(path)), os.path.normcase(os.path.abspath(base))
    except (OSError, ValueError):
        return False
    return p == b or p.startswith(b.rstrip('\\') + '\\')

def default_install_folder():
    """C:\\Games\\MechAssault 2 (on the system drive), else %LOCALAPPDATA%\\Programs\\MechAssault 2 if that can't be created."""
    drive = os.environ.get('SystemDrive', 'C:')
    games = Path(drive + '\\Games')
    try:
        if games.is_dir() or not os.path.lexists(games):
            probe = games if games.is_dir() else Path(drive + '\\')
            tmp = probe / ('.ma2probe%d' % os.getpid())
            tmp.write_bytes(b'x'); tmp.unlink()
            return str(games / 'MechAssault 2')
    except OSError:
        pass
    base = os.environ.get('LOCALAPPDATA') or str(Path.home() / 'AppData' / 'Local')
    return str(Path(base) / 'Programs' / 'MechAssault 2')

def _nearest_existing(p):
    p = Path(p)
    while not p.exists() and p.parent != p: p = p.parent
    return p

def drive_type(path):
    try:
        root = os.path.splitdrive(os.path.abspath(path))[0] + '\\'
        return ctypes.windll.kernel32.GetDriveTypeW(root)       # 0/1 unknown, 2 removable, 3 fixed, 4 network, 5 cd-rom
    except Exception:
        return 3

def volume_fs(path):
    """The file system of the volume holding `path` ('NTFS', 'ReFS', 'FAT32', 'exFAT', ...), or '' if unknown."""
    try:
        buf = ctypes.create_unicode_buffer(64)
        root = os.path.splitdrive(os.path.abspath(str(_nearest_existing(path))))[0] + '\\'
        if ctypes.windll.kernel32.GetVolumeInformationW(root, None, 0, None, None, None, buf, 64): return buf.value
    except Exception:
        pass
    return ''

def long_paths_enabled():
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'SYSTEM\CurrentControlSet\Control\FileSystem') as k:
            return bool(winreg.QueryValueEx(k, 'LongPathsEnabled')[0])
    except OSError:
        return False

def user_folder(path):
    """'Documents', 'Desktop' or 'Downloads' when `path` is inside one (Controlled Folder Access guards them; they often sync), else ''."""
    home = Path(os.environ.get('USERPROFILE') or Path.home())
    for name in ('Documents', 'Desktop', 'Downloads'):
        if _under(str(path), str(home / name)): return name
    return ''

def check_folder(text, need_bytes=0, enhanced=False, kit=None):
    """Everything worth saying about an install folder, as a FolderReport (never raises)."""
    r = FolderReport()
    raw = clean_path(text)
    add = lambda level, code, msg, fix=None: r.findings.append(Finding(level, code, msg, fix))
    if not raw:
        add('error', 'empty', 'Choose an install folder.'); return r
    if raw.startswith('\\\\') or raw.startswith('//'):
        add('error', 'unc', 'The C++ tools cannot build in a network folder (\\\\server\\share). Choose a folder on this PC, e.g. C:\\Games\\MechAssault 2.'); return r
    if not is_absolute(raw):
        add('error', 'relative', 'Enter a full folder path, e.g. C:\\Games\\MechAssault 2'); return r
    p = Path(raw)
    if re.search(r'[<>"|?*]', raw[2:]):
        add('error', 'badchars', 'A folder name can\'t contain < > " | ? *'); return r
    if p.parent == p:
        add('error', 'driveroot', 'Choose a folder on the drive, not the drive itself. Try %s\\MechAssault 2.' % raw.rstrip('\\')); return r
    pf = [os.environ.get(v) for v in ('ProgramFiles', 'ProgramFiles(x86)', 'ProgramW6432', 'SystemRoot', 'windir')]
    if any(_under(raw, b) for b in pf):
        add('error', 'protected', 'Windows protects this folder; the installer can\'t write there. Try C:\\Games\\MechAssault 2.'); return r
    temp = tempfile.gettempdir()
    if _under(raw, temp) or 'Temp1_' in raw or (kit and _under(raw, kit)):
        add('error', 'temp', 'This folder is temporary: Windows or the installer deletes it. Choose a normal folder such as C:\\Games\\MechAssault 2.'); return r
    ty = drive_type(raw)
    fs =volume_fs(raw).upper()
    if fs.startswith('FAT'):
        add('error', 'fat', 'This drive uses %s, which cannot hold files over 4 GB or the build. Choose an NTFS drive (most internal drives).' % fs); return r
    if fs == 'EXFAT':
        add('warn', 'exfat', 'This drive uses exFAT: the install works, but NTFS (most internal drives) is more reliable.')
    if user_folder(raw):
        add('warn', 'userfolder', 'Your %s folder can be guarded by Controlled Folder Access or synced, which can block the build or the game. '
            'A folder such as C:\\Games\\MechAssault 2 is better.' % user_folder(raw))
    if ty == 4:
        add('warn', 'network', 'Network folders are slow and may fail during the build.')
    elif ty == 5:
        add('error', 'cdrom', 'That is a CD/DVD drive. Choose a folder on a hard drive.'); return r
    elif ty in (0, 1) and not Path(os.path.splitdrive(raw)[0] + '\\').exists():
        add('error', 'nodrive', 'That drive doesn\'t exist.'); return r
    elif ty == 2:
        add('warn', 'removable', 'This is a removable drive. The build is slow and long; an internal drive is better.')
    od = [os.environ.get(v) for v in ('OneDrive', 'OneDriveConsumer', 'OneDriveCommercial')]
    if any(_under(raw, b) for b in od) or re.search(r'(?i)[\\/]OneDrive(\s-\s[^\\/]*)?([\\/]|$)', raw):
        add('warn', 'onedrive', 'This folder syncs to OneDrive; 4 GB of game files would upload. Choose a folder outside OneDrive, e.g. C:\\Games\\MechAssault 2.')
    if (len(raw) > 150 and not long_paths_enabled()) or (enhanced and len(raw) > 80):
        add('warn', 'long', 'Long folder paths can break the compiler%s. Choose a shorter path.' % (' and the AI tools' if enhanced else ''))
    if not plain_path(os.path.normpath(raw)):
        add('info', 'tempbuild', 'Fine. The compiler uses a temporary folder such as C:\\MA2RecompBuild-xxxx during the build, removed afterwards.')
    # what is already there
    if p.exists():
        if not p.is_dir():
            add('error', 'notdir', 'That is a file, not a folder.'); return r
        state = read_state(p)
        if state is not None:
            r.existing = 'unfinished'; r.state = state
        elif is_install(p):
            r.existing = 'install'; r.stamp = read_stamp(p)
        elif any(p.iterdir()):
            r.existing = 'other'
            r.suggest = str(p / 'MechAssault 2')
            add('error', 'notempty', 'This folder already has other files. Choose an empty folder or a new name.', r.suggest)
        else:
            r.existing = 'empty'
    # writable?
    anchor = _nearest_existing(p)
    try:
        tmp = anchor / ('.ma2probe%d' % os.getpid())
        tmp.write_bytes(b'x'); tmp.unlink()
    except OSError:
        add('error', 'readonly', 'Can\'t write to this folder (Access denied). Choose a folder you can write to, e.g. C:\\Games\\MechAssault 2.')
        return r
    try:
        r.free = shutil.disk_usage(anchor).free
    except OSError:
        r.free = None
    if r.free is not None and need_bytes and r.free < need_bytes * 1.1:
        add('error', 'space', 'Not enough space: needs %.1f GB, %.1f GB free on %s.' % (
            need_bytes * 1.1 / GB, r.free / GB, os.path.splitdrive(raw)[0] or raw[:2]))
    return r

# ---------------------------------------------------------------------------- this PC

def windows_info():
    v = sys.getwindowsversion()
    return {'build': v.build, 'name': 'Windows 11' if v.build >= 22000 else 'Windows 10', 'x64': sys.maxsize > 2 ** 32,
            'ok': v.major >= 10 and v.build >= 18362 and sys.maxsize > 2 ** 32}

MACHINES = {0x8664: 'x64', 0xAA64: 'ARM64', 0x14C: 'x86'}

def native_arch():
    """The PC's own processor architecture ('x64', 'ARM64', ...), also when this x64 Python runs emulated on ARM; '' if unknown."""
    try:
        k = ctypes.windll.kernel32
        proc, native = ctypes.c_ushort(), ctypes.c_ushort()
        if k.IsWow64Process2(ctypes.c_void_p(k.GetCurrentProcess()), ctypes.byref(proc), ctypes.byref(native)):
            return MACHINES.get(native.value, '0x%04X' % native.value)
    except Exception:
        pass
    return ''

def insider_build():
    """True when the PC takes Windows Insider (preview) builds."""
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'SOFTWARE\Microsoft\WindowsSelfHost\Applicability') as k:
            return bool(str(winreg.QueryValueEx(k, 'BranchName')[0]).strip())
    except Exception:
        return False

def platform_note():
    """One plain line when this Windows is one the release was not verified on (Windows on ARM, an Insider build), else ''.
    Informational only (logged, in diagnostics); it never stops an install."""
    try:
        arch, insider = native_arch(), insider_build()
    except Exception:
        return ''
    parts = []
    if arch and arch != 'x64': parts.append('Windows on %s (the game runs as x64 under emulation)' % arch)
    if insider: parts.append('a Windows Insider preview build (%d)' % sys.getwindowsversion().build)
    return ('This PC runs %s: not verified with this release; the install continues normally.' % ' and '.join(parts)) if parts else ''

def memory_info():
    class S(ctypes.Structure):
        _fields_ = [('length', ctypes.c_ulong), ('load', ctypes.c_ulong), ('total', ctypes.c_ulonglong), ('avail', ctypes.c_ulonglong),
                    ('pt', ctypes.c_ulonglong), ('pa', ctypes.c_ulonglong), ('vt', ctypes.c_ulonglong), ('va', ctypes.c_ulonglong),
                    ('ve', ctypes.c_ulonglong)]
    s = S(); s.length = ctypes.sizeof(S)
    if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(s)): return s.total, s.avail
    return 16 * GB, 8 * GB

def build_jobs(cpus, avail, total=None):
    """Parallel compiles: CPU threads, capped by total RAM (2.5 GB each) and by what is free now (1.2 GB each); at most 2 on a PC
    with less than 12 GB (8 GB PCs got 3 and could run out of memory; a 12 GB PC reports about 11.8 GB, so the line is 11.5)."""
    jobs = max(1, min(cpus or 4, int((total or avail * 2) / (2.5 * GB)), int(avail / (1.2 * GB))))
    return min(jobs, 2) if (total or avail * 2) < 11.5 * GB else jobs

def estimate_minutes(cpus, avail, total=None):
    """Minutes for a typical install. Measured on 10 parallel compiles: analyse 85 s, translate 335 s, build 540 s."""
    jobs = build_jobs(cpus, avail, total)
    return max(5, int(round((85 + 225 + 110 + 60 + 540 * 10.0 / jobs) / 60.0)))

def on_battery():
    class P(ctypes.Structure):
        _fields_ = [('ac', ctypes.c_ubyte), ('flag', ctypes.c_ubyte), ('pct', ctypes.c_ubyte), ('saver', ctypes.c_ubyte),
                    ('life', ctypes.c_ulong), ('full', ctypes.c_ulong)]
    p = P()
    return bool(ctypes.windll.kernel32.GetSystemPowerStatus(ctypes.byref(p))) and p.ac == 0 and p.flag != 128

def power_status():
    """{'ac': 'on'|'off'|'unknown', 'battery': True|False|None (a battery is present: the game's own laptop test, pc_storage.c),
    'percent': 0-100|None, 'saver': True|False}, from GetSystemPowerStatus; None if Windows does not answer. Diagnostics and the
    installer's gentler default only."""
    class P(ctypes.Structure):
        _fields_ = [('ac', ctypes.c_ubyte), ('flag', ctypes.c_ubyte), ('pct', ctypes.c_ubyte), ('saver', ctypes.c_ubyte),
                    ('life', ctypes.c_ulong), ('full', ctypes.c_ulong)]
    p = P()
    if not ctypes.windll.kernel32.GetSystemPowerStatus(ctypes.byref(p)): return None
    return {'ac': {0: 'off', 1: 'on'}.get(p.ac, 'unknown'), 'battery': None if p.flag == 255 else p.flag != 128,
            'percent': None if p.pct == 255 else int(p.pct), 'saver': p.saver == 1}

def cpu_model():
    """The processor's name as Windows reports it (ProcessorNameString), else platform.processor(), else ''."""
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'HARDWARE\DESCRIPTION\System\CentralProcessor\0') as k:
            return ' '.join(str(winreg.QueryValueEx(k, 'ProcessorNameString')[0]).split())
    except Exception:
        try:
            import platform
            return platform.processor() or ''
        except Exception:
            return ''

def memory_status():
    """{'total', 'avail', 'commit_limit', 'commit_free', 'load'} in bytes / percent (GlobalMemoryStatusEx), or None."""
    class S(ctypes.Structure):
        _fields_ = [('length', ctypes.c_ulong), ('load', ctypes.c_ulong), ('total', ctypes.c_ulonglong), ('avail', ctypes.c_ulonglong),
                    ('pt', ctypes.c_ulonglong), ('pa', ctypes.c_ulonglong), ('vt', ctypes.c_ulonglong), ('va', ctypes.c_ulonglong),
                    ('ve', ctypes.c_ulonglong)]
    s = S(); s.length = ctypes.sizeof(S)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(s)): return None
    return {'total': s.total, 'avail': s.avail, 'commit_limit': s.pt, 'commit_free': s.pa, 'load': s.load}

def installed_memory():
    """The RAM physically installed (bytes; a 16 GB PC says exactly 16 GB, unlike memory_info's usable total), or None when Windows
    cannot tell (some VMs): then memory_info's total stands in. Decides the 1.1.6 pre-set build settings (Installer.preseed_units)."""
    try:
        kb = ctypes.c_ulonglong(0)
        if ctypes.windll.kernel32.GetPhysicallyInstalledSystemMemory(ctypes.byref(kb)) and kb.value:
            return kb.value * 1024
    except Exception:
        pass
    return None

def smart_app_control():
    """'enforce', 'eval', 'off' or None (not present), from the Code Integrity policy key."""
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'SYSTEM\CurrentControlSet\Control\CI\Policy') as k:
            v = winreg.QueryValueEx(k, 'VerifiedAndReputablePolicyState')[0]
        return {1: 'enforce', 2: 'eval', 0: 'off'}.get(v)
    except OSError:
        return None

SAC_NOTICE = ("Windows Smart App Control may block programs built on this PC, including this game. It can't be turned back on once off, "
              "so check before installing.")

def sac_notice():
    """B2 (silent): the ONE plain notice to show before the long build, only when Smart App Control is enforcing; else None.
    Antivirus and Controlled Folder Access are never shown (security_facts() puts them in diagnostics). Errors: None."""
    try:
        return SAC_NOTICE if smart_app_control() == 'enforce' else None
    except Exception:
        return None

def d3d11_ok():
    """True/False: does a hardware D3D11 device with feature level 11.0 come up? None if the probe itself failed."""
    try:
        d3d = ctypes.WinDLL('d3d11')
        levels = (ctypes.c_uint * 1)(0xB000)
        dev, ctx, got = ctypes.c_void_p(), ctypes.c_void_p(), ctypes.c_uint()
        hr = d3d.D3D11CreateDevice(None, 1, None, 0, levels, 1, 7, ctypes.byref(dev), ctypes.byref(got), ctypes.byref(ctx))
        for o in (ctx, dev):                     # release: IUnknown::Release is vtable slot 2
            if o.value:
                vt = ctypes.cast(ctypes.cast(o, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
                ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vt[2])(o)
        return hr >= 0 and got.value >= 0xB000
    except Exception:
        return None

VENDOR_IDS = {0x10DE: 'nvidia', 0x1002: 'amd', 0x1022: 'amd', 0x8086: 'intel'}

def dxgi_adapters():
    """The real graphics adapters from DXGI (what Direct3D and Vulkan drivers see): [{'name', 'vram' (dedicated bytes), 'vendor',
    'vendor_id', 'device_id', 'source': 'dxgi'}], software / Basic Render adapters left out. Raises on any problem (the caller falls back)."""
    class LUID(ctypes.Structure):
        _fields_ = [('low', ctypes.c_ulong), ('high', ctypes.c_long)]
    class DESC1(ctypes.Structure):
        _fields_ = [('Description', ctypes.c_wchar * 128), ('VendorId', ctypes.c_uint), ('DeviceId', ctypes.c_uint), ('SubSysId', ctypes.c_uint),
                    ('Revision', ctypes.c_uint), ('DedicatedVideoMemory', ctypes.c_size_t), ('DedicatedSystemMemory', ctypes.c_size_t),
                    ('SharedSystemMemory', ctypes.c_size_t), ('AdapterLuid', LUID), ('Flags', ctypes.c_uint)]
    class GUID(ctypes.Structure):
        _fields_ = [('a', ctypes.c_ulong), ('b', ctypes.c_ushort), ('c', ctypes.c_ushort), ('d', ctypes.c_ubyte * 8)]
    iid = GUID(0x770AAE78, 0xF26F, 0x4DBA, (ctypes.c_ubyte * 8)(0xA8, 0x29, 0x25, 0x3C, 0x83, 0xD1, 0xB3, 0x87))   # IDXGIFactory1
    dxgi = ctypes.WinDLL('dxgi')
    factory = ctypes.c_void_p()
    hr = dxgi.CreateDXGIFactory1(ctypes.byref(iid), ctypes.byref(factory))
    if hr < 0 or not factory.value: raise OSError('CreateDXGIFactory1 0x%08X' % (hr & 0xFFFFFFFF))
    def call(obj, index, restype, argtypes, *args):
        vt = ctypes.cast(ctypes.cast(obj, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
        return ctypes.WINFUNCTYPE(restype, ctypes.c_void_p, *argtypes)(vt[index])(obj, *args)
    out = []
    try:
        for i in range(16):
            adapter = ctypes.c_void_p()
            hr = call(factory, 12, ctypes.c_long, [ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p)], i, ctypes.byref(adapter))   # EnumAdapters1
            if hr < 0 or not adapter.value: break             # DXGI_ERROR_NOT_FOUND: no more adapters
            try:
                d = DESC1()
                if call(adapter, 10, ctypes.c_long, [ctypes.POINTER(DESC1)], ctypes.byref(d)) >= 0:              # GetDesc1
                    if d.Flags & 2 or d.VendorId == 0x1414: continue  # DXGI_ADAPTER_FLAG_SOFTWARE, Microsoft Basic Render
                    if re.search(r'(?i)basic (display|render)|remote|virtual|hyper-v|parsec', d.Description): continue
                    out.append({'name': d.Description, 'vram': int(d.DedicatedVideoMemory), 'vendor': VENDOR_IDS.get(d.VendorId, 'other'),
                                'vendor_id': '%04X' % d.VendorId, 'device_id': '%04X' % d.DeviceId, 'source': 'dxgi'})
            finally:
                call(adapter, 2, ctypes.c_ulong, [])            # Release
    finally:
        call(factory, 2, ctypes.c_ulong, [])
    return out

def detect_gpus():
    """[{'name','vram'(bytes),'vendor', ...}]: DXGI's real adapters (dedicated memory, vendor ids; right on hybrid laptops), plus any
    registry entry of a vendor DXGI did not list; the registry alone (the 1.1.3 way) when DXGI fails or lists nothing. nvidia-smi refines."""
    reg = _registry_gpus()
    try:
        dx = dxgi_adapters()
    except Exception:
        dx = []
    gpus = (dx + [g for g in reg if g['vendor'] not in {d['vendor'] for d in dx}]) if dx else reg
    for g in gpus:                                            # the driver version (diagnostics), from the registry entry of that name
        r = next((x for x in reg if x['name'] == g['name'] and x.get('driver')), None)
        if r and not g.get('driver'): g['driver'] = r['driver']
    return _nvidia_smi(gpus)

def _registry_gpus():
    """The 1.1.3 list: display adapters from the registry (any vendor, no admin)."""
    gpus = []
    try:
        import winreg
        cls = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}')
        for i in range(64):
            try:
                sub = winreg.EnumKey(cls, i)
            except OSError:
                break
            if not sub.isdigit(): continue
            try:
                k = winreg.OpenKey(cls, sub)
                name = winreg.QueryValueEx(k, 'DriverDesc')[0]
            except OSError:
                continue
            vram = 0
            for value in ('HardwareInformation.qwMemorySize', 'HardwareInformation.MemorySize'):
                try:
                    v = winreg.QueryValueEx(k, value)[0]
                    vram = int.from_bytes(v, 'little') if isinstance(v, (bytes, bytearray)) else int(v)
                    break
                except (OSError, ValueError):
                    continue
            if re.search(r'(?i)basic (display|render)|remote|virtual|hyper-v|parsec', name): continue
            low = name.lower()
            vendor = 'nvidia' if 'nvidia' in low or 'geforce' in low else 'amd' if 'amd' in low or 'radeon' in low else \
                     'intel' if 'intel' in low else 'other'
            g = {'name': name, 'vram': vram, 'vendor': vendor}
            try: g['driver'] = str(winreg.QueryValueEx(k, 'DriverVersion')[0])
            except OSError: pass
            gpus.append(g)
    except Exception:
        pass
    return gpus

def _nvidia_smi(gpus):
    try:
        import subprocess
        smi = shutil.which('nvidia-smi') or r'C:\Windows\System32\nvidia-smi.exe'
        r = subprocess.run([smi, '--query-gpu=name,memory.total', '--format=csv,noheader,nounits'], capture_output=True, text=True,
                           timeout=20, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        if r.returncode == 0:
            for line in r.stdout.strip().splitlines():
                name, _, mib = line.rpartition(',')
                name = name.strip()
                vram = int(float(mib) * 1024 * 1024)
                match = next((g for g in gpus if g['vendor'] == 'nvidia' and (g['name'] == name or not g.get('smi'))), None)
                if match: match.update(name=name, vram=vram, smi=True)
                else: gpus.append({'name': name, 'vram': vram, 'vendor': 'nvidia', 'smi': True})
    except Exception:
        pass
    return gpus

def recommend_textures(gpus):
    """Advice only (the default stays Original). {'rec': 'original'|'2x'|'4x', 'can': bool (the enhanced textures can be made),
    'videos': bool (the enhanced cutscenes can be made), 'engine': 'pytorch'|'vulkan', 'reason', 'gpu'}
    Without an NVIDIA GPU the textures and the cutscenes are made through Vulkan (realesrgan-ncnn-vulkan) on any other graphics card."""
    nv = [g for g in gpus if g['vendor'] == 'nvidia']
    if not nv:
        other = max(gpus, key=lambda g: g['vram'], default=None)
        if other:
            why = ('%s: the enhanced textures and cutscenes use your GPU through Vulkan (new; not yet tested on AMD or Intel cards). '
                   'The cutscenes are made frame by frame, so they are very slow.' % other['name'])
            return {'rec': 'original', 'can': True, 'videos': True, 'engine': 'vulkan', 'reason': why, 'gpu': other['name']}
        return {'rec': 'original', 'can': False, 'reason': 'No graphics card was identified; enhanced content needs an NVIDIA GPU with 8 GB or more.',
                'gpu': None}
    best = max(nv, key=lambda g: g['vram'])
    gb = best['vram'] / GB
    label = '%s, %.0f GB' % (best['name'], gb)
    if gb >= 11.5:
        return {'rec': '4x', 'can': True, 'reason': '%s: can generate 4x enhanced textures.' % label, 'gpu': best['name'], 'vram': best['vram']}
    if gb >= 7.5:
        return {'rec': '2x', 'can': True, 'reason': '%s: can generate 2x enhanced textures.' % label, 'gpu': best['name'], 'vram': best['vram']}
    return {'rec': 'original', 'can': False, 'reason': '%s: fine for the game; enhanced content needs 8 GB or more.' % label,
            'gpu': best['name'], 'vram': best['vram']}

def internet_ok(timeout=4):
    """Can this PC reach Microsoft? A raw connection first; behind an HTTP proxy that fails although downloads work, so then try urllib,
    which follows the system proxy. Any answer counts (even an HTTP error page)."""
    import socket
    try:
        socket.create_connection(('aka.ms', 443), timeout=timeout).close(); return True
    except OSError:
        pass
    try:
        import urllib.error, urllib.request
        urllib.request.urlopen('https://aka.ms', timeout=timeout).close(); return True
    except urllib.error.HTTPError:
        return True
    except Exception:
        return False

# ---------------------------------------------------------------------------- settings merge

def _ini_keys(text):
    """{section: {key: line}} plus the order of sections, from ini text (comments ignored)."""
    out, order, sec = {}, [], None
    for line in text.splitlines():
        s = line.strip()
        if not s or s[0] in ';#': continue
        m = re.match(r'^\[(.+)\]$', s)
        if m: sec = m.group(1); out.setdefault(sec, {}); order.append(sec) if sec not in order else None; continue
        if sec is not None and '=' in s: out[sec].setdefault(s.split('=', 1)[0].strip().lower(), line.rstrip())
    return out, order

def merge_ini(defaults_text, live_text):
    """Add-only merge: (new live text, [added 'Section.Key']). Existing values, comments and order are untouched."""
    dkeys, dorder = _ini_keys(defaults_text)
    lkeys, _ = _ini_keys(live_text)
    nl = '\r\n' if '\r\n' in live_text else '\n'
    lines = live_text.splitlines()
    added = []
    for sec in dorder:
        missing = [(k, line) for k, line in dkeys[sec].items() if k not in lkeys.get(sec, {})]
        if not missing: continue
        if sec in lkeys:       # insert after the last line of that section
            start = next(i for i, l in enumerate(lines) if l.strip().lower() == '[%s]' % sec.lower())
            end = start + 1
            while end < len(lines) and not re.match(r'^\s*\[.+\]\s*$', lines[end]): end += 1
            while end > start + 1 and not lines[end - 1].strip(): end -= 1
            lines[end:end] = [line for _, line in missing]
        else:
            if lines and lines[-1].strip(): lines.append('')
            lines += ['[%s]' % sec] + [line for _, line in missing]
        added += ['%s.%s' % (sec, k) for k, _ in missing]
    return nl.join(lines) + nl, added

def set_ini_value(text, key, value):
    """Set `key=value` where it already exists (every occurrence)."""
    return re.sub(r'(?im)^(\s*%s\s*=).*$' % re.escape(key), lambda m: '%s%s' % (m.group(1), value), text)

# ---------------------------------------------------------------------------- support bundle

def short(path, n=52):
    """A long path shortened in the middle for display."""
    s = str(path)
    return s if len(s) <= n else s[:n // 2 - 1] + '\u2026' + s[-(n // 2):]

def fmt_duration(seconds):
    seconds = int(seconds)
    return '%d h %02d min' % (seconds // 3600, seconds % 3600 // 60) if seconds >= 3600 else \
           '%d min %02d s' % (seconds // 60, seconds % 60) if seconds >= 60 else '%d s' % seconds

REDACT = [   # (pattern, replacement): network host names, URL queries and anything that looks like a credential
    (re.compile(r'\\\\(?![?.]\\)[^\\/\s"\'<>|]+(?=\\)'), r'\\\\<host>'),                     # \\server\share -> \\<host>\share
    (re.compile(r'(?i)\b((?:https?|ftp)://)[^/\s:@"\']+:[^/\s@"\']+@'), r'\1<hidden>@'),           # user:password@ in a URL
    (re.compile(r'(?i)\b((?:https?|ftp)://[^\s?#"\']+)\?[^\s"\'#]*'), r'\1?<query>'),
    (re.compile(r'(?i)\b(authorization|proxy-authorization)\s*:\s*\S+(\s+\S+)?'), r'\1: <hidden>'),
    (re.compile(r'(?i)\b(password|passwd|pwd|token|secret|api[_-]?key|access[_-]?key|client[_-]?secret|sig)\s*[=:]\s*[^\s&;,"\']+'), r'\1=<hidden>'),
]

def redact(text):
    """Replace this user's name and profile folder, network host names, URL query strings and anything that looks like a
    credential, so a pasted log or a diagnostics zip doesn't carry them. Disc certificate keys
    and the studio build path (printed by older installers' xbe_parser step) are dropped too."""
    text = re.sub(r'(?im)^[ \t]*(LAN Key|Signature Key|Alternate Signature Keys?|Debug Path)[ \t]*:.*$', '  [line removed]', text)
    for var in ('LOCALAPPDATA', 'APPDATA', 'USERPROFILE'):      # longest first: both live under the profile
        value = (os.environ.get(var) or '').rstrip('\\/')
        if len(value) > 3:
            text = re.sub(re.escape(value) + r'(?=$|[\\/"\s\'])', lambda m, t='%' + var + '%': t, text, flags=re.I)
    # any other profile folder (another account, a moved LOCALAPPDATA): keep the drive, drop the name
    text = re.sub(r'(?i)\b([A-Z]:[\\/]Users[\\/])(?!Public(?=[\\/"]|$)|<user>)[^\\/"\r\n]+', lambda m: m.group(1) + '<user>', text)
    for var in ('USERNAME', 'COMPUTERNAME', 'USERDOMAIN'):      # 1.1.6: the machine (and domain) name too, as tools/privacy_scan.py checks
        value = os.environ.get(var)
        if value and len(value) > 2:
            text = re.sub(r'(?<![\w%])' + re.escape(value) + r'(?![\w%])', lambda m, t='%' + var + '%': t, text, flags=re.I)
    for pattern, repl in REDACT: text = pattern.sub(repl, text)       # network hosts, URL queries, credentials
    text = EMAIL.sub('<email>', text)                                   # 1.1.6: e-mail addresses (after user:password@ above)
    return text

EMAIL = re.compile(r'[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+\.[A-Za-z0-9.-]*[A-Za-z]{2,}')

# ---------------------------------------------------------------------------- binary scrub (1.1.6: dumps in a sent report)

def private_names():
    """The values a binary file in a report must not carry, longest first: the profile folders, the user, machine and domain names."""
    out = []
    for var in ('LOCALAPPDATA', 'APPDATA', 'USERPROFILE'):
        v = (os.environ.get(var) or '').rstrip('\\/')
        if len(v) > 3: out.append(v)
    for var in ('USERNAME', 'COMPUTERNAME', 'USERDOMAIN'):
        v = os.environ.get(var) or ''
        if len(v) > 2: out.append(v)
    seen, uniq = set(), []
    for v in sorted(out, key=len, reverse=True):
        if v.lower() not in seen: seen.add(v.lower()); uniq.append(v)
    return uniq

def _ci_pattern(value, encoding, bounded=False):
    """A bytes regex for `value` in `encoding`, case-insensitive per character (works for non-ASCII letters and UTF-16LE)."""
    parts = []
    for ch in value:
        alts = {ch, ch.lower(), ch.upper()}
        enc = []
        for a in sorted(alts):
            try: b = a.encode(encoding)
            except (UnicodeEncodeError, LookupError): continue
            if len(a) == 1 and b not in enc: enc.append(b)
        if not enc: return None
        parts.append(b'(?:' + b'|'.join(re.escape(b) for b in enc) + b')')
    body = b''.join(parts)
    if bounded:                    # a name, not a path: only as a whole word (user32.dll stays readable for a user named "user")
        if encoding == 'utf-16-le': body = rb'(?<![A-Za-z0-9]\x00)' + body + rb'(?![A-Za-z0-9]\x00)'
        else: body = rb'(?<![A-Za-z0-9])' + body + rb'(?![A-Za-z0-9])'
    return re.compile(body)

EMAIL_UTF16 = re.compile(rb'(?:[A-Za-z0-9._%+-]\x00)+@\x00(?:[A-Za-z0-9-]\x00)+\.\x00(?:[A-Za-z0-9.-]\x00)*(?:[A-Za-z]\x00){2,}')
EMAIL_BYTES = re.compile(EMAIL.pattern.encode('ascii'))

# any profile folder name after X:\Users\ (8.3 short names such as JOHNSM~1, another account), except Public / Default / All Users
_USERS_SKIP = ('public', 'default', 'all users', 'xxx')
USERS_BYTES = re.compile(rb'(?i)([A-Z]:[\\/]Users[\\/])([^\\/\x00"<>|*?\r\n]{1,64}?)(?=[\\/\x00"])')
USERS_UTF16 = re.compile(rb'(?i)([A-Z]\x00:\x00[\\/]\x00U\x00s\x00e\x00r\x00s\x00[\\/]\x00)((?:(?![\\/"]\x00)[^\x00]\x00|[^\x00][^\x00]){1,64}?)(?=[\\/"]\x00|\x00\x00)')

def _same_length(n, wide):
    return (b'x\x00' * (n // 2) + b'x' * (n % 2)) if wide else b'x' * n

def scrub_bytes(data, names=None):
    """Replace the user name, machine name and profile folders (UTF-8, the ANSI code page and UTF-16LE, any case), any other
    X:\\Users\\<name> folder name and e-mail addresses in binary data with the same number of 'x' bytes (UTF-16: 'x' characters), so
    every offset in a crash dump stays valid. Names count only as whole words (a user called "user" leaves user32.dll alone).
    Returns (new bytes, number of replacements)."""
    data = bytes(data); count = 0
    def sub(rx, data, wide, users=False):
        def repl(m):
            nonlocal count
            if users:
                name = m.group(2).decode('utf-16-le' if wide else 'latin-1', 'replace').lower()
                if name in _USERS_SKIP or set(name) <= {'x', '%'}: return m.group(0)
                count += 1                 # '%' first: reads as a placeholder (privacy_scan skips Users\%...); same length
                return m.group(1) + (b'%\x00' if wide else b'%') + _same_length(len(m.group(2)) - (2 if wide else 1), wide)
            count += 1
            return _same_length(m.end() - m.start(), wide)
        return rx.sub(repl, data)
    for value in (private_names() if names is None else names):
        path = '\\' in value or '/' in value
        for enc in ('utf-16-le', 'utf-8', 'mbcs'):
            try: rx = _ci_pattern(value, enc, bounded=not path)
            except Exception: rx = None
            if rx is not None: data = sub(rx, data, enc == 'utf-16-le')
    data = sub(USERS_UTF16, data, True, users=True)
    data = sub(USERS_BYTES, data, False, users=True)
    data = sub(EMAIL_UTF16, data, True)
    data = sub(EMAIL_BYTES, data, False)
    return data, count

def support_bundle(lines, stage='', hide_user=True, version='', kit_id='', gpus=None, vs=''):
    w = windows_info(); total, avail = memory_info()
    head = ['Report problems at: %s' % support_where(), 'MechAssault 2 Recomp %s (build %s)' % (version, kit_id), 'Failed stage: %s' % (stage or '-'),
            'Windows build %s, %d CPU threads, %.0f GB RAM' % (w['build'], os.cpu_count() or 0, total / GB),
            'GPU: %s' % ('; '.join('%s %.0f GB' % (g['name'], g['vram'] / GB) for g in gpus) if gpus else '-'),
            'C++ tools: %s' % (vs or '-'), '', 'Last log lines:'] + list(lines)[-80:]
    text = '\n'.join(head)
    return redact(text) if hide_user else text
def _screen_size():
    try:
        u = ctypes.windll.user32
        return '%dx%d' % (u.GetSystemMetrics(0), u.GetSystemMetrics(1))
    except Exception:
        return 'unknown'

def diagnostics_extras():
    """What the two optional boxes of Collect diagnostics could add: {'dump': newest crash or hang dump or None, 'botrec': newest bot recording or None}."""
    logs = logs_dir(); out = {'dump': None, 'botrec': None}
    if not logs.is_dir(): return out
    newest = lambda files: max((f for f in files if f.is_file()), key=lambda f: f.stat().st_mtime, default=None)
    try:
        out['dump'] = newest(list(logs.glob('crash-*.dmp')) + list(logs.glob('hang-*.dmp')))
        out['botrec'] = newest(list(logs.glob('*.botrec')))
    except OSError:
        pass
    return out

_SECURITY = None

def security_facts():
    """{'antivirus': [names], 'cfa': 'on'|'off'|'audit'|'unknown', 'sac': ...} for diagnostics only (never shown as a dashboard, never
    blocking). Antivirus names from Windows Security Center (a hidden PowerShell CIM query, 10 s at most); cached per run."""
    global _SECURITY
    if _SECURITY is not None: return _SECURITY
    out = {'antivirus': [], 'cfa': 'unknown', 'sac': 'unknown'}
    try:
        import subprocess
        r = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command',
                            'Get-CimInstance -Namespace root/SecurityCenter2 -ClassName AntiVirusProduct | ForEach-Object { $_.displayName }'],
                           capture_output=True, text=True, timeout=10, stdin=subprocess.DEVNULL, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        out['antivirus'] = [l.strip() for l in r.stdout.splitlines() if l.strip()][:5]
    except Exception:
        pass
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'SOFTWARE\Microsoft\Windows Defender\Windows Defender Exploit Guard\Controlled Folder Access') as k:
            out['cfa'] = {0: 'off', 1: 'on', 2: 'audit'}.get(winreg.QueryValueEx(k, 'EnableControlledFolderAccess')[0], 'unknown')
    except Exception:
        pass
    try: out['sac'] = smart_app_control() or 'not present'
    except Exception: pass
    _SECURITY = out
    return out

def _sdk_version():
    try:
        base = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Windows Kits' / '10' / 'Lib'
        vers = sorted((d.name for d in base.iterdir() if (d / 'um' / 'x64' / 'kernel32.lib').is_file()),
                      key=lambda v: tuple(int(x) for x in re.findall(r'\d+', v)))
        return vers[-1] if vers else 'none'
    except Exception:
        return 'unknown'

def diagnostics_facts(dest, state=None, gpus=None):
    """The 1.1.4 lines of system.txt: session, architecture, platform note, SDK, drive headroom (install, system, TEMP), adapters with
    driver and ids, security facts, recent downloads, and the run's history (retries, recoveries, rollbacks, tool exit codes).
    Each part is optional: one that fails is left out (it can never stop a zip from being written)."""
    lines = []
    def part(fn):
        try: fn()
        except Exception as e: lines.append('(%s: %s)' % (getattr(fn, '__name__', 'part'), type(e).__name__))
    def basics():
        lines.append('Session: %s' % session_id())
        lines.append('Processor: %s' % (native_arch() or 'unknown'))
        note = platform_note()
        if note: lines.append('Platform: %s' % note)
        lines.append('Windows SDK: %s' % _sdk_version())
    def drives():
        seen = []
        for p in (dest, os.environ.get('SystemDrive', 'C:') + '\\', os.environ.get('TEMP') or ''):
            if not p: continue
            d = os.path.splitdrive(os.path.abspath(str(p)))[0].upper()
            if d in seen: continue
            seen.append(d)
            try: lines.append('Drive %s: %.1f GB free (%s)' % (d, shutil.disk_usage(str(_nearest_existing(p))).free / GB, volume_fs(p) or '?'))
            except OSError: lines.append('Drive %s: not readable' % d)
    def adapters():
        for g in (gpus if gpus is not None else detect_gpus()):
            lines.append('Adapter: %s, %.1f GB, vendor %s%s%s' % (g.get('name'), (g.get('vram') or 0) / GB, g.get('vendor_id') or g.get('vendor'),
                         ' device %s' % g['device_id'] if g.get('device_id') else '', ', driver %s' % g['driver'] if g.get('driver') else ''))
    def security():
        s = security_facts()
        lines.append('Security: antivirus %s; Controlled Folder Access %s; Smart App Control %s' % (', '.join(s['antivirus']) or 'unknown', s['cfa'], s['sac']))
    def downloads():
        f = logs_dir() / 'downloads.log'
        if f.is_file():
            for l in f.read_text(encoding='utf-8', errors='replace').splitlines()[-15:]: lines.append('Download: %s' % l)
    def history():
        for e in ((state or {}).get('events') or [])[-40:] if isinstance(state, dict) else []: lines.append('Event: %s' % e)
        low = ((state or {}).get('lowmem') or {}).get('units') if isinstance(state, dict) else None
        if low:                                    # 1.1.5 low-memory build: which files got which lighter settings
            lighter = ['%s %s' % (u, ' '.join(f for f in fl if f != '/cgthreads1')) for u, fl in sorted(low.items()) if len(fl) > 1]
            lines.append('Low-memory build: %d file(s) with /cgthreads1%s' % (len(low), '; lighter: ' + ', '.join(lighter[:20]) if lighter else ''))
        else:                                      # 1.1.6: a finished install has no state.json any more: its stamp records it
            rec = (read_stamp(dest) or {}).get('low_memory_build') if dest else None
            if isinstance(rec, dict):
                lighter = rec.get('lighter_optimisation') or {}
                lines.append('Low-memory build: %s file(s) with /cgthreads1%s (from %s)' % (rec.get('one_codegen_thread', '?'),
                             '; lighter: ' + ', '.join('%s %s' % kv for kv in sorted(lighter.items())[:20]) if lighter else '', STAMP))
    def hardware():                                # 1.1.6: CPU model, free memory and commit, power state
        lines.append('CPU: %s, %d threads' % (cpu_model() or 'unknown', os.cpu_count() or 0))
        m = memory_status()
        if m: lines.append('Memory: %.1f GB, %.1f GB free (%d%% in use); commit limit %.1f GB, %.1f GB free' % (
            m['total'] / GB, m['avail'] / GB, m['load'], m['commit_limit'] / GB, m['commit_free'] / GB))
        inst = installed_memory()
        if inst: lines.append('Installed RAM: %.0f GB' % (inst / GB))
        p = power_status()
        if p: lines.append('Power: mains %s; battery %s%s; battery saver %s' % (p['ac'], {True: 'present', False: 'none', None: 'unknown'}[p['battery']],
                           ' (%d%%)' % p['percent'] if p['battery'] and p['percent'] is not None else '', 'on' if p['saver'] else 'off'))
    def unclean():                                 # 1.1.6 power-loss marker (install.py): the last run ended without a stop
        if not isinstance(state, dict): return
        u = state.get('last_unclean') or ({'step': state['in_progress'], 'time': state.get('in_progress_at', '')} if state.get('in_progress') else None)
        if isinstance(u, dict) and u.get('step'):
            lines.append('Last run: ended without a stop (power loss, shutdown or a closed window) during %s%s'
                         % (u['step'], ' (started %s)' % u['time'] if u.get('time') else ''))
    for fn in (basics, hardware, drives, adapters, security, downloads, history, unclean): part(fn)
    return lines

SUMMARY_SETTINGS = (('Display', ('WindowWidth', 'WindowHeight', 'Fullscreen', 'VSync', 'FrameLimit', 'TextureQuality', 'FMVQuality',
                                 'PreloadShaders')), ('Audio', ('Enabled', 'Threads')), ('Bots', ('BotRecorder',)))

def _ini_values(path):
    """{(section, key): value} for the keys report_summary names (plain values only)."""
    out, sec = {}, ''
    try: text = Path(path).read_text(encoding='utf-8', errors='replace')
    except OSError: return out
    for line in text.splitlines():
        line = line.strip()
        if line.startswith('[') and line.endswith(']'): sec = line[1:-1].strip(); continue
        if '=' in line and not line.startswith((';', '#')):
            k, v = line.split('=', 1); out[(sec, k.strip())] = v.strip()[:40]
    return out

def _game_log_lines(log, patterns=('[HEALTH]', '[CRASH]', '[HANG]', '[LAUNCH]')):
    """The newest line of each kind in a game log (read from the end, at most 4 MB)."""
    found = {}
    try:
        with open(log, 'rb') as f:
            f.seek(0, 2); size = f.tell(); f.seek(max(0, size - (4 << 20)))
            text = f.read().decode('utf-8', 'replace')
    except OSError:
        return found
    for line in reversed(text.splitlines()):
        for p in patterns:
            if p not in found and p in line: found[p] = line.strip()[:240]
        if len(found) == len(patterns): break
    return found

def report_summary(dest, state=None, version='', kit_id='', gpus=None, stage='', failure='', kind=''):
    """About 15 plain lines for a problem report (the issue text): version and build, Windows, CPU, memory, power, graphics, the
    install record, a few settings, the failure, the newest [HEALTH] / [CRASH] / [HANG] lines of the newest game log. Every line goes
    through redact(); a part that fails is left out."""
    out = []
    def part(fn):
        try: fn()
        except Exception as e: out.append('(%s: %s)' % (getattr(fn, '__name__', 'part'), type(e).__name__))
    dest = Path(dest) if dest else None
    try: stamp = read_stamp(dest) if dest else None
    except Exception: stamp = None
    def ident():
        out.append('MechAssault 2 Recomp %s (build %s)%s' % (version or '-', kit_id or '-', ' | report: %s' % kind if kind else ''))
        if isinstance(stamp, dict): out.append('Installed: %s (kit %s)' % (stamp.get('public_version') or stamp.get('version') or '?', stamp.get('kit_id') or '?'))
        else: out.append('Installed: nothing finished in the install folder')
    def system():
        w = windows_info()
        out.append('Windows build %s (%s)%s' % (w.get('build'), native_arch() or '?', '' if w.get('ok', True) else ', not supported'))
        out.append('CPU: %s, %d threads' % (cpu_model() or 'unknown', os.cpu_count() or 0))
        m = memory_status() or {}
        if m: out.append('Memory: %.1f GB, %.1f GB free; commit limit %.1f GB, %.1f GB free' % (m['total'] / GB, m['avail'] / GB, m['commit_limit'] / GB, m['commit_free'] / GB))
        p = power_status()
        if p: out.append('Power: mains %s, battery %s%s' % (p['ac'], {True: 'present', False: 'none', None: 'unknown'}[p['battery']], ', saver on' if p['saver'] else ''))
    def graphics():
        gs = gpus if gpus is not None else detect_gpus()
        out.append('GPU: %s' % ('; '.join('%s %.0f GB%s' % (g.get('name'), (g.get('vram') or 0) / GB, ', driver %s' % g['driver'] if g.get('driver') else '') for g in gs) or '-'))
    def build():
        rec = (stamp or {}).get('low_memory_build') if isinstance(stamp, dict) else None
        low = ((state or {}).get('lowmem') or {}).get('units') if isinstance(state, dict) else None
        if low: out.append('Low-memory build: %d file(s) so far (unfinished install)' % len(low))
        elif isinstance(rec, dict): out.append('Low-memory build: %s file(s) with /cgthreads1, lighter: %s' % (rec.get('one_codegen_thread'), ', '.join(sorted(rec.get('lighter_optimisation') or {})) or 'none'))
        else: out.append('Low-memory build: no')
    def settings():
        vals = _ini_values(local_dir() / 'pc_settings.ini')
        if not vals: out.append('Settings: none yet (the game has not been started)'); return
        for sec, keys in SUMMARY_SETTINGS:
            got = ['%s=%s' % (k, vals[(sec, k)]) for k in keys if (sec, k) in vals]
            if got: out.append('[%s] %s' % (sec, ' '.join(got)))
    def failed():
        if failure or stage: out.append('Failure: %s%s' % (('%s: ' % stage) if stage else '', str(failure).split('\n')[0][:300]))
        if isinstance(state, dict):
            done = [s for s in (state.get('done') or []) if isinstance(s, str) and s != 'check']
            out.append('Unfinished install: %d step(s) done, last step %s' % (len(done), state.get('last_step') or '-'))
            u = state.get('last_unclean') or ({'step': state['in_progress']} if state.get('in_progress') else None)
            if isinstance(u, dict) and u.get('step'): out.append('Last run: ended without a stop during %s' % u['step'])
    def game():
        logs = logs_dir()
        own = ('install-', 'installer-ui-', 'buildtools-', 'stub-', 'downloads')
        cands = sorted((f for f in logs.glob('*.log') if not f.name.startswith(own) and f.is_file()), key=lambda f: f.stat().st_mtime, reverse=True) if logs.is_dir() else []
        if not cands: out.append('Game log: none'); return
        found = _game_log_lines(cands[0])
        out.append('Game log: %s' % cands[0].name)
        for p in ('[HEALTH]', '[CRASH]', '[HANG]'):
            if p in found: out.append(found[p])
    for fn in (ident, system, graphics, build, settings, failed, game): part(fn)
    return [redact(l) for l in out]

MB = 1024 * 1024
DUMP_LIMIT = 50 * MB             # Collect diagnostics (saved on this PC)
REPORT_DUMP_LIMIT = 15 * MB      # a sent report
REPORT_MAX_BYTES = 20 * MB       # a sent report's zip, at most (the relay takes 25 MB)

def collect_diagnostics(dest, out_dir, version='', kit_id='', gpus=None, include_dump=False, include_botrec=False, stage='', failure='', log_lines=None,
                        scrub=True, dump_limit=DUMP_LIMIT, max_bytes=None, text='', name_prefix='MA2-diagnostics'):
    r"""Write MA2-diagnostics-<time>.zip into out_dir with what a developer needs and nothing else (the window saves it in
    %LOCALAPPDATA%\MechAssault2\Diagnostics, the command line's --diagnostics on the Desktop): recomp-install.json, the live pc_settings.ini,
    the newest three game logs, the newest installer log(s), system.txt (the support-bundle header, PC facts, graphics cards, screen size,
    the state of an unfinished install). Every text file goes through redact() (user, machine and domain names, profile folders, e-mail
    addresses). Only if asked: the newest crash or hang dump up to `dump_limit` (1.1.6: with `scrub`, the user, machine and profile
    names in it are overwritten with the same number of x bytes; if that fails the dump is left out) and the newest bot recording
    (compressed: it cannot be scrubbed, so a sent report never asks for it). `text`: the player's own words (what-happened.txt).
    `max_bytes`: a zip larger than this drops the bot recording, then the dump, and says so in system.txt.
    Works with nothing installed (dest missing or empty): then the zip holds system.txt and whatever logs and settings exist, and system.txt says what was not found.
    A failed install passes `stage` (the step that failed), `failure` (the reason) and `log_lines` (the last lines the window showed).
    Returns {'path', 'members', 'notes', 'sizes' (member -> bytes before compression), 'dropped' (members left out for size)}."""
    dest = Path(dest); out_dir = Path(out_dir)
    logs = logs_dir(); notes = []
    newest = lambda files, n=1: sorted((f for f in files if f.is_file()), key=lambda f: f.stat().st_mtime, reverse=True)[:n]
    own = ('install-', 'installer-ui-', 'buildtools-', 'stub-', 'downloads')    # downloads.log: summarised in system.txt
    text_members, file_members, bin_members = {}, {}, {}
    def add_text(arc, path):
        try: text_members[arc] = redact(Path(path).read_text(encoding='utf-8', errors='replace'))
        except OSError as e: notes.append('%s could not be read (%s)' % (arc, e.strerror or e))
    if (dest / STAMP).is_file(): add_text(STAMP, dest / STAMP)
    else: notes.append('%s was not found in %s (nothing finished is installed there)' % (STAMP, dest))
    live = local_dir() / 'pc_settings.ini'
    if live.is_file(): add_text('pc_settings.ini', live)
    else: notes.append('no pc_settings.ini yet (the game has not been started)')
    game_logs = newest([f for f in logs.glob('*.log') if not f.name.startswith(own)], 3) if logs.is_dir() else []
    for f in game_logs: add_text('logs/' + f.name, f)
    if not game_logs: notes.append('no game logs found in %s' % logs)
    for pattern in ('install-*.log', 'installer-ui-*.log', 'buildtools-*.log', 'stub-*.log'):   # buildtools: the Systems page; stub: the exe's own start log
        for f in newest(list(logs.glob(pattern))) if logs.is_dir() else []: add_text('logs/' + f.name, f)
    if include_dump and logs.is_dir():
        dumps = newest(list(logs.glob('crash-*.dmp')) + list(logs.glob('hang-*.dmp')))
        if not dumps: notes.append('a dump was asked for but none exists')
        elif dumps[0].stat().st_size > dump_limit: notes.append('%s skipped: larger than %d MB' % (dumps[0].name, dump_limit // MB))
        elif scrub:
            try:
                data, n = scrub_bytes(Path(dumps[0]).read_bytes())
                bin_members['dumps/' + dumps[0].name] = data
                notes.append('%s: %d private name(s) or folder(s) overwritten (same length)' % (dumps[0].name, n))
            except Exception as e:                 # never send what could not be checked
                notes.append('%s left out: it could not be checked for private data (%s)' % (dumps[0].name, type(e).__name__))
        else: file_members['dumps/' + dumps[0].name] = dumps[0]
    if include_botrec and logs.is_dir():
        rec = newest(list(logs.glob('*.botrec')))
        if rec: file_members['botrec/' + rec[0].name] = rec[0]
        else: notes.append('a bot recording was asked for but none exists')
    if text and str(text).strip():
        text_members['what-happened.txt'] = redact(str(text).strip()[:2000]) + '\n'
    state = read_state(dest) if dest.is_dir() else None          # an unfinished install: how far it got (the disc path inside is not copied)
    known = read_json(local_dir() / 'installer.json'); known = known if isinstance(known, dict) else {}
    vs = (state or {}).get('toolchain') if isinstance(state, dict) else None
    head = support_bundle([], stage or (isinstance(state, dict) and state.get('last_title')) or '', True, version, kit_id,
                          gpus if gpus is not None else detect_gpus(), vs or known.get('toolchain') or '').split('\n\nLast log lines:')[0]
    try: facts = diagnostics_facts(dest, state, gpus)               # 1.1.4 facts (+ 1.1.6 lines); any failure leaves them out
    except Exception: facts = []
    def system_txt():
        members = sorted(list(text_members) + list(file_members) + list(bin_members))
        lines = [head, 'Screen: %s' % _screen_size(), 'Install folder: %s' % redact(str(dest)), 'Included: %s' % ', '.join([m for m in members if m != 'system.txt'] + ['system.txt'])]
        if failure: lines.append('Failure: %s' % redact(str(failure)))
        if isinstance(state, dict):
            done = [s for s in (state.get('done') or []) if isinstance(s, str) and s != 'check']
            lines.append('Unfinished install: %d step(s) finished (%s); last step: %s' % (len(done), ', '.join(done) or '-', redact(str(state.get('last_title') or 'unknown'))))
        lines.extend('Note: %s' % n for n in notes)
        lines.extend(facts)
        if log_lines: lines.extend(['', 'Last log lines:'] + [redact(str(l)) for l in list(log_lines)[-80:]])
        return redact('\n'.join(lines) + '\n')            # the notes name folders under the profile too
    import zipfile
    name = '%s-%s.zip' % (name_prefix, time.strftime('%Y%m%d-%H%M%S'))
    def write(folder):
        text_members['system.txt'] = system_txt()
        folder.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(folder / name, 'w', zipfile.ZIP_DEFLATED) as z:
            for arc, t in sorted(text_members.items()): z.writestr(arc, t.encode('utf-8'))
            for arc, data in sorted(bin_members.items()): z.writestr(arc, data)
            for arc, f in sorted(file_members.items()): z.write(f, arc)
        return folder / name
    try:
        zip_path = write(out_dir)
    except PermissionError:                     # Controlled Folder Access (or similar) guards the Desktop: our own folder instead
        try: (out_dir / name).unlink()
        except OSError: pass
        notes.append('the Desktop could not be written to (Controlled Folder Access?), so the zip was saved in %s' % redact(str(local_dir() / 'Diagnostics')))
        zip_path = write(local_dir() / 'Diagnostics')
    dropped = []
    if max_bytes:                               # a report has a size limit: the bot recording goes first, then the dump
        for prefix in ('botrec/', 'dumps/'):
            if zip_path.stat().st_size <= max_bytes: break
            for d in (file_members, bin_members):
                for arc in [a for a in d if a.startswith(prefix)]:
                    del d[arc]; dropped.append(arc)
                    notes.append('%s left out: the report would be larger than %d MB' % (arc, max_bytes // MB))
            if dropped: zip_path = write(zip_path.parent)
    sizes = {arc: len(t.encode('utf-8')) for arc, t in text_members.items()}
    sizes.update({arc: len(b) for arc, b in bin_members.items()})
    for arc, f in file_members.items():
        try: sizes[arc] = Path(f).stat().st_size
        except OSError: sizes[arc] = 0
    return {'path': zip_path, 'members': sorted(list(text_members) + list(file_members) + list(bin_members)), 'notes': notes,
            'sizes': sizes, 'dropped': dropped}

