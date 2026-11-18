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

def check_folder(text, need_bytes=0, enhanced=False, kit=None):
    """Everything worth saying about an install folder, as a FolderReport (never raises)."""
    r = FolderReport()
    raw = clean_path(text)
    add = lambda level, code, msg, fix=None: r.findings.append(Finding(level, code, msg, fix))
    if not raw:
        add('error', 'empty', 'Choose an install folder.'); return r
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
    if raw.startswith('\\\\') or ty == 4:
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
    if len(raw) > 150 or (enhanced and len(raw) > 80):
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

def smart_app_control():
    """'enforce', 'eval', 'off' or None (not present), from the Code Integrity policy key."""
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'SYSTEM\CurrentControlSet\Control\CI\Policy') as k:
            v = winreg.QueryValueEx(k, 'VerifiedAndReputablePolicyState')[0]
        return {1: 'enforce', 2: 'eval', 0: 'off'}.get(v)
    except OSError:
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

def detect_gpus():
    """[{'name','vram'(bytes),'vendor'}] -- registry (any vendor, no admin), refined by nvidia-smi for NVIDIA."""
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
            gpus.append({'name': name, 'vram': vram, 'vendor': vendor})
    except Exception:
        pass
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
    'videos': bool (the enhanced cutscenes can be made: NVIDIA only), 'engine': 'pytorch'|'vulkan', 'reason', 'gpu'}
    Without an NVIDIA GPU the textures are made through Vulkan (realesrgan-ncnn-vulkan) on any other graphics card."""
    nv = [g for g in gpus if g['vendor'] == 'nvidia']
    if not nv:
        other = max(gpus, key=lambda g: g['vram'], default=None)
        if other:
            why = ('%s: the enhanced textures use your GPU through Vulkan (new in 1.1.2; not yet tested on AMD or Intel cards). '
                   'The enhanced cutscenes need an NVIDIA GPU.' % other['name'])
            return {'rec': 'original', 'can': True, 'videos': False, 'engine': 'vulkan', 'reason': why, 'gpu': other['name']}
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

def redact(text):
    """Replace this user's name and profile folder, so a pasted log doesn't carry them. Disc certificate keys
    and the studio build path (printed by older installers' xbe_parser step) are dropped too."""
    text = re.sub(r'(?im)^[ \t]*(LAN Key|Signature Key|Alternate Signature Keys?|Debug Path)[ \t]*:.*$', '  [line removed]', text)
    for var in ('LOCALAPPDATA', 'APPDATA', 'USERPROFILE'):      # longest first: both live under the profile
        value = (os.environ.get(var) or '').rstrip('\\/')
        if len(value) > 3:
            text = re.sub(re.escape(value) + r'(?=$|[\\/"\s\'])', lambda m, t='%' + var + '%': t, text, flags=re.I)
    # any other profile folder (another account, a moved LOCALAPPDATA): keep the drive, drop the name
    text = re.sub(r'(?i)\b([A-Z]:[\\/]Users[\\/])(?!Public(?=[\\/"]|$)|<user>)[^\\/"\r\n]+', lambda m: m.group(1) + '<user>', text)
    value = os.environ.get('USERNAME')
    if value and len(value) > 2:
        text = re.sub(r'(?<![\w%])' + re.escape(value) + r'(?![\w%])', '%USERNAME%', text, flags=re.I)
    return text

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

def collect_diagnostics(dest, out_dir, version='', kit_id='', gpus=None, include_dump=False, include_botrec=False, stage='', failure='', log_lines=None):
    r"""Write MA2-diagnostics-<time>.zip into out_dir (the Desktop) with what a developer needs and nothing else:
    recomp-install.json, the live pc_settings.ini, the newest three game logs, the newest installer log(s), system.txt (the support-bundle header, graphics
    cards, screen size, the state of an unfinished install). Every text file goes through redact() (user name and profile folder). Only if asked: the newest
    crash or hang dump under 50 MB (binary, NOT redacted: it can hold folder names and the Windows user name) and the newest bot recording.
    Works with nothing installed (dest missing or empty): then the zip holds system.txt and whatever logs and settings exist, and system.txt says what was not found.
    A failed install passes `stage` (the step that failed), `failure` (the reason) and `log_lines` (the last lines the window showed).
    Returns {'path', 'members', 'notes'}."""
    dest = Path(dest); out_dir = Path(out_dir)
    logs = logs_dir(); notes = []
    newest = lambda files, n=1: sorted((f for f in files if f.is_file()), key=lambda f: f.stat().st_mtime, reverse=True)[:n]
    own = ('install-', 'installer-ui-', 'buildtools-')
    text_members, file_members = {}, {}
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
    for pattern in ('install-*.log', 'installer-ui-*.log', 'buildtools-*.log'):          # buildtools: the Systems page's Build Tools install
        for f in newest(list(logs.glob(pattern))) if logs.is_dir() else []: add_text('logs/' + f.name, f)
    if include_dump and logs.is_dir():
        dumps = newest(list(logs.glob('crash-*.dmp')) + list(logs.glob('hang-*.dmp')))
        if not dumps: notes.append('a dump was asked for but none exists')
        elif dumps[0].stat().st_size > 50 * 1024 * 1024: notes.append('%s skipped: larger than 50 MB' % dumps[0].name)
        else: file_members['dumps/' + dumps[0].name] = dumps[0]
    if include_botrec and logs.is_dir():
        rec = newest(list(logs.glob('*.botrec')))
        if rec: file_members['botrec/' + rec[0].name] = rec[0]
        else: notes.append('a bot recording was asked for but none exists')
    head = support_bundle([], stage, True, version, kit_id, gpus if gpus is not None else detect_gpus(), '').split('\n\nLast log lines:')[0]
    lines = [head, 'Screen: %s' % _screen_size(), 'Install folder: %s' % redact(str(dest)), 'Included: %s' % ', '.join(sorted(list(text_members) + list(file_members)) + ['system.txt'])]
    if failure: lines.append('Failure: %s' % redact(str(failure)))
    state = read_state(dest) if dest.is_dir() else None          # an unfinished install: how far it got (the disc path inside is not copied)
    if isinstance(state, dict):
        done = [s for s in (state.get('done') or []) if isinstance(s, str) and s != 'check']
        lines.append('Unfinished install: %d step(s) finished (%s); last step: %s' % (len(done), ', '.join(done) or '-', redact(str(state.get('last_title') or 'unknown'))))
    lines += ['Note: %s' % n for n in notes]
    if log_lines: lines += ['', 'Last log lines:'] + [redact(str(l)) for l in list(log_lines)[-80:]]
    text_members['system.txt'] = redact('\n'.join(lines) + '\n')            # the notes name folders under the profile too
    import zipfile
    name = 'MA2-diagnostics-%s.zip' % time.strftime('%Y%m%d-%H%M%S')
    def write(folder):
        folder.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(folder / name, 'w', zipfile.ZIP_DEFLATED) as z:
            for arc, text in sorted(text_members.items()): z.writestr(arc, text.encode('utf-8'))
            for arc, f in sorted(file_members.items()): z.write(f, arc)
        return folder / name
    try:
        zip_path = write(out_dir)
    except PermissionError:                     # Controlled Folder Access (or similar) guards the Desktop: our own folder instead
        try: (out_dir / name).unlink()
        except OSError: pass
        zip_path = write(local_dir() / 'Diagnostics')
        notes.append('the Desktop could not be written to (Controlled Folder Access?), so the zip was saved in %s' % redact(str(zip_path.parent)))
    return {'path': zip_path, 'members': sorted(list(text_members) + list(file_members)), 'notes': notes}

