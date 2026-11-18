"""MechAssault 2 Recomp installer: from your own MechAssault 2 disc image to the PC release.

This runs inside "MechAssault 2 Recomp Installer.exe", the single-file installer, which carries this
kit (a Python runtime, toolkit/, source/, patches/, assets/, defaults/, enhancer/) and unpacks it
while it runs. No game data files are included: the game's program, art, sound and levels come from
your own disc. Graphics shaders are compiled on your PC as you play;
none come with the installer. Everything else is made on your PC from your disc, with the xboxrecomp
toolkit, the way the PC release itself was made:

   1 check      Microsoft C++ Build Tools (installed on request if missing), free space; GPU for options
   2 disc       read ma2.xbe from the image and check it is the supported MechAssault 2
   3 extract    unpack the whole image into <install>/game_files
   4 analyze    xboxrecomp: XBE parse, disassembly, function identification, ABI analysis
   5 recompile  xboxrecomp: translate all 25,850 functions to C (src/recomp/gen)
   6 generate   the two bringup generators (callbacks, codec fast paths)
   7 patch      the PC release's fixes and features in the generated code (patches/), every file
                checked before and after against the release's SHA-256
   8 build      CMake + Ninja + MSVC: the game executable from source/ and the generated code
                (first, FFmpeg for the cutscenes is downloaded and checked file by file: it is not
                inside the installer; see ffmpeg_fetch.py)
   9 assets     pc_assets: the PC menu overlay built from the disc's common.mgf (checked against
                the release), the shader seed and level shader lists
  10 install    MechAssault 2.exe, FFmpeg and MSVC runtime DLLs, licenses, controls, defaults
  11 textures   optional: Enhancer 2x and/or 4x texture packs (NVIDIA GPU with CUDA, or any other graphics
                card through Vulkan with realesrgan-ncnn-vulkan, downloaded and checked; hours)
  12 videos     optional: Enhancer 2x cutscene videos (CUDA GPU, many hours)
  13 finish     remove the work folder: the install folder holds only the game

The work folder (<install>/_work) keeps each finished step, so an interrupted install resumes when
run again with the same folders. Running it again on a finished install adds optional content.

  "MechAssault 2 Recomp Installer.exe" --xiso "MechAssault 2.xiso" --dest "C:/Games/MechAssault 2"
        [--textures 2x,4x] [--videos] [--install-build-tools] [--jobs N] [--keep-work] [--allow-cpu]
        [--update] [--shortcuts desktop,start,editor] [--no-merge-settings] [--switch-textures] [--low-priority]
        [--ffmpeg <ffmpeg-9.0.2-full_build-shared.zip, for a PC without internet>]
  --update      update an existing install in place (no disc needed when its game_files are intact); keeps settings
  --upscaler    the Vulkan texture upscaler, if this PC has no internet: realesrgan-ncnn-vulkan-20220424-windows.zip,
                a folder with it extracted, or a URL (MA2_TEXTURE_ENGINE=vulkan|pytorch picks the texture upscaler)
  Exit code 0: installed. 1: stopped (an update puts the previous version back). 2: the game is installed, but optional
  content (enhanced textures or videos) failed; running the installer again resumes only that.
  Paths may be relative to the folder you run it from, and may be quoted.
  (from cmd.exe use: start /wait "" "MechAssault 2 Recomp Installer.exe" ...)
"""
import argparse, ctypes, hashlib, json, os, re, shutil, subprocess, sys, threading, time
from pathlib import Path

HERE = Path(__file__).resolve().parent            # installer/
KIT = HERE.parent                                 # the unpacked kit
sys.path.insert(0, str(HERE))
import precheck                                   # noqa: E402  (shared checks: disc image, folder, settings merge)
import ffmpeg_fetch                               # noqa: E402  (FFmpeg: downloaded on this PC, every file checked)
import upscaler_fetch                             # noqa: E402  (the Vulkan texture upscaler: downloaded, every file checked)
RELEASE = json.loads((HERE / 'data' / 'release.json').read_text(encoding='utf-8'))
VERSION = RELEASE.get('date', 'dev')              # the release date string (KITSRC/VERSION line 2); install_relation compares it
PUBLIC_VERSION = RELEASE.get('version', 'dev')    # the public version (KITSRC/VERSION line 1), e.g. 1.0.2
KIT_ID = KIT.name.split('MA2RecompInstaller-', 1)[1] if KIT.name.startswith('MA2RecompInstaller-') else 'dev'
BUILD = KIT_ID[:8]

def build_label():
    """'MechAssault 2 Recomp 1.0.2 (build 1a2b3c4d)': the one name the windows, logs and documents use for this release."""
    return 'MechAssault 2 Recomp %s (build %s)' % (PUBLIC_VERSION, BUILD)
BELOW_NORMAL = 0x00004000
PROJECT = 'MechAssault2'                          # project folder inside the toolkit copy
FFMPEG = 'ffmpeg-9.0.2-full_build-shared'
# xbe_parser prints the disc's certificate keys and the studio's build path: never logged (logs get posted)
SECRET_LINE = re.compile(r'^\s*(LAN Key|Signature Key|Alternate Signature Keys?|Debug Path)\s*:', re.I)
FFMPEG_DLLS = ['avcodec-63.dll', 'avformat-63.dll', 'avutil-61.dll', 'swresample-7.dll', 'swscale-10.dll']
GB = 1024 ** 3
# Microsoft's Build Tools bootstrappers: VS 2026 (the PC release's compiler, MSVC 19.5x), then VS 2022.
BUILD_TOOLS_URLS = ['https://aka.ms/vs/18/stable/vs_buildtools.exe', 'https://aka.ms/vs/17/release/vs_buildtools.exe']
# The bootstrapper's own exit codes (and Windows' for a refused start), each with its own advice.
BUILD_TOOLS_EXIT = {
    1602: ('The Build Tools installation was cancelled.', 'cancelled'),
    5004: ('The Build Tools installation was cancelled.', 'cancelled'),
    1618: ('Another installation is running on this PC (Windows Update or another installer). Wait until it finishes, then try again.', 'compiler'),
    1001: ('The Visual Studio Installer is already running. Close it, then try again.', 'compiler'),
    740: ('The Build Tools installer needs administrator approval, and it was not given.', 'compiler'),
    5: ('The Build Tools installer needs administrator approval, and it was not given (access denied).', 'compiler'),
    5003: ('The Build Tools installer could not download its files. Check the internet connection, then try again.', 'network'),
    5007: ("Microsoft's Build Tools say this PC does not meet their requirements.", 'compiler'),
    8010: ("Microsoft's Build Tools say this version of Windows is not supported.", 'compiler'),
}
REBOOT = ("Microsoft's C++ Build Tools are installed, but Windows must restart before they can be used. Restart the PC, then open "
          'the installer again and press Resume (or Install).')

def build_tools_urls(build=None):
    """Windows 10 (build < 22000) gets only the VS 2022 Build Tools: VS 2026 does not list Windows 10 as supported."""
    build = build if build is not None else sys.getwindowsversion().build
    return BUILD_TOOLS_URLS[1:] if build < 22000 else BUILD_TOOLS_URLS

def microsoft_signed(path):
    """WinVerifyTrust (Authenticode chain, no prompts) on `path`, then the signer certificate's organisation must be Microsoft
    Corporation. No PowerShell (an apostrophe in TEMP broke the old check)."""
    wt, crypt = ctypes.WinDLL('wintrust'), ctypes.WinDLL('crypt32')
    class FILEINFO(ctypes.Structure):
        _fields_ = [('cbStruct', ctypes.c_ulong), ('pcwszFilePath', ctypes.c_wchar_p), ('hFile', ctypes.c_void_p), ('pgKnownSubject', ctypes.c_void_p)]
    class DATA(ctypes.Structure):
        _fields_ = [('cbStruct', ctypes.c_ulong), ('pPolicyCallbackData', ctypes.c_void_p), ('pSIPClientData', ctypes.c_void_p),
                    ('dwUIChoice', ctypes.c_ulong), ('fdwRevocationChecks', ctypes.c_ulong), ('dwUnionChoice', ctypes.c_ulong),
                    ('pFile', ctypes.POINTER(FILEINFO)), ('dwStateAction', ctypes.c_ulong), ('hWVTStateData', ctypes.c_void_p),
                    ('pwszURLReference', ctypes.c_wchar_p), ('dwProvFlags', ctypes.c_ulong), ('dwUIContext', ctypes.c_ulong), ('pSignatureSettings', ctypes.c_void_p)]
    class PCERT(ctypes.Structure):
        _fields_ = [('cbStruct', ctypes.c_ulong), ('pCert', ctypes.c_void_p)]
    class SGNR(ctypes.Structure):
        _fields_ = [('cbStruct', ctypes.c_ulong), ('ftVerifyAsOf', ctypes.c_ulong * 2), ('csCertChain', ctypes.c_ulong), ('pasCertChain', ctypes.POINTER(PCERT))]
    action = _guid('{00AAC56B-CD44-11D0-8CC2-00C04FC295EE}')                   # WINTRUST_ACTION_GENERIC_VERIFY_V2
    fi = FILEINFO(ctypes.sizeof(FILEINFO), str(path), None, None)
    data = DATA(cbStruct=ctypes.sizeof(DATA), dwUIChoice=2, dwUnionChoice=1, pFile=ctypes.pointer(fi), dwStateAction=1)   # WTD_UI_NONE, FILE, VERIFY
    wt.WinVerifyTrust.argtypes = [ctypes.c_void_p, ctypes.POINTER(_GUID), ctypes.POINTER(DATA)]; wt.WinVerifyTrust.restype = ctypes.c_long
    wt.WTHelperProvDataFromStateData.argtypes = [ctypes.c_void_p]; wt.WTHelperProvDataFromStateData.restype = ctypes.c_void_p
    wt.WTHelperGetProvSignerFromChain.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    wt.WTHelperGetProvSignerFromChain.restype = ctypes.POINTER(SGNR)
    crypt.CertGetNameStringW.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_char_p, ctypes.c_wchar_p, ctypes.c_ulong]
    org = ''
    ok = wt.WinVerifyTrust(None, ctypes.byref(action), ctypes.byref(data)) == 0
    try:
        if ok:
            sg = wt.WTHelperGetProvSignerFromChain(wt.WTHelperProvDataFromStateData(data.hWVTStateData), 0, 0, 0)
            if sg and sg.contents.csCertChain:
                buf = ctypes.create_unicode_buffer(256)                      # CERT_NAME_ATTR_TYPE, szOID_ORGANIZATION_NAME
                crypt.CertGetNameStringW(sg.contents.pasCertChain[0].pCert, 3, 0, b'2.5.4.10', buf, 256); org = buf.value
    finally:
        data.dwStateAction = 2; wt.WinVerifyTrust(None, ctypes.byref(action), ctypes.byref(data))      # WTD_STATEACTION_CLOSE
    return ok and org == 'Microsoft Corporation'
# Inherited variables that inject options into cl.exe / link.exe (set by other developer tools): never passed to the game build.
BUILD_ENV_DROP = ('CL', '_CL_', 'LINK', '_LINK_')
BUILD_TOOLS_ARGS = ['--passive', '--wait', '--norestart', '--nocache',
                    '--add', 'Microsoft.VisualStudio.Workload.VCTools',
                    '--add', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                    '--add', 'Microsoft.VisualStudio.Component.VC.CMake.Project',
                    '--add', 'Microsoft.VisualStudio.Component.VC.Redist.14.Latest',   # the runtime DLLs copied beside the game
                    '--includeRecommended']

class Stop(Exception):
    """A problem the user can act on; the message says what. `kind` picks the wizard's failure advice:
    cancelled, disk_full, compiler, build_failed, access_denied, network, ffmpeg_mismatch, disc, gpu, out_of_memory, unexpected,
    compile_error (the compiler or linker named the error), and extras (the game is installed; only optional content failed or was cancelled; `cause` is the original kind)."""
    def __init__(self, message, kind='unexpected', code=None, cause=None):
        super().__init__(message)
        self.kind, self.code, self.cause = kind, code, cause

# Optional content: its failure never undoes or invalidates the game install.
OPTIONAL_STEPS = ('textures-2x', 'textures-4x', 'videos')
OPTIONAL_NAMES = {'textures-2x': 'The 2x enhanced textures', 'textures-4x': 'The 4x enhanced textures', 'videos': 'The enhanced cutscene videos'}
NO_CONSOLE = getattr(subprocess, 'CREATE_NO_WINDOW', 0) | getattr(subprocess, 'CREATE_NEW_PROCESS_GROUP', 0)   # no window to close, no shared Ctrl+C

ENHANCER_HINT = ('Antivirus software may be blocking Python, which makes the enhanced content: allow the install folder in it, then try again. '
                 'The game itself is installed and not affected.')

NTSTATUS = {
    0xC000013A: 'stopped from outside: its window was closed or security software ended it',
    0x40010004: 'ended from outside (another program or security software stopped it)',
    0xC0000005: 'crashed (access violation)',
    0xC0000409: 'crashed (stack buffer overrun)',
    0xC00000FD: 'crashed (stack overflow)',
    0xC0000017: 'ran out of memory',
    0xC000012D: 'ran out of memory (the page file is too small)',
    0xC0000135: 'could not start: a DLL it needs is missing',
    0xC0000142: 'could not start: a DLL failed to initialise (security software can cause this)',
    0xC0000022: 'was refused access (security software can cause this)',
}

def exit_meaning(code):
    """A readable meaning for a Windows process exit code that is an NTSTATUS (3221225786 = 0xC000013A), else ''."""
    try:
        code = int(code) & 0xFFFFFFFF
    except (TypeError, ValueError):
        return ''
    if code in NTSTATUS: return NTSTATUS[code]
    if code >= 0xC0000000: return 'ended with Windows error status 0x%08X' % code
    return ''

def exit_text(code):
    """'exit code 3221225786 = 0xC000013A: stopped from outside: ...' or 'exit code 1'."""
    meaning = exit_meaning(code)
    return 'exit code %d = 0x%08X: %s' % (int(code) & 0xFFFFFFFF, int(code) & 0xFFFFFFFF, meaning) if meaning else 'exit code %d' % code

def explain_codes(text):
    """Meanings for NTSTATUS exit codes quoted in a tool's own output (e.g. pip's 'returned non-zero exit status 3221225786')."""
    out = []
    for m in re.finditer(r'(?i)exit (?:status|code)[ :=]*(-?\d{6,11})\b', text):
        meaning = exit_meaning(m.group(1))
        if meaning and meaning not in out: out.append(meaning)
    return out

def extras_message(name, reason, cancelled=False):
    """The failure text when only optional content failed: the game itself is installed and playable."""
    what = OPTIONAL_NAMES.get(name, 'The enhanced content')
    if cancelled:
        return ('MechAssault 2 is installed. %s were not made: the install was stopped. You can make them later with Resume or '
                'Add enhanced content.' % what)
    return ('MechAssault 2 is installed. %s could not be made: %s You can try again later with Resume or Add enhanced content.'
            % (what, str(reason).strip()))

COMPILER_ERROR = re.compile(r'(?i)\b(?:fatal )?error (?:LNK|C|RC|CVT)\d{4}\b|\bCMake Error\b')

def compiler_error(tail):
    """The first compiler / linker / CMake error line in a tool's output, else ''."""
    for line in tail:
        if COMPILER_ERROR.search(line): return line.strip()
    return ''

# Numeric codes first (WinError, errno, NTSTATUS, MSVC / LNK): they read the same on every Windows language. The English text below is the fallback.
CODE_KINDS = [
    ('out_of_memory', r'\b(?:C1060|C1076|C3859|LNK1102)\b|\[WinError (?:8|14|1455)\]|\[Errno 12\]|0xC0000017|0xC000012D'),
    ('disk_full', r'\[WinError (?:39|112)\]|\[Errno 28\]|\bLNK1106\b'),
    ('access_denied', r'\[WinError (?:5|32|33|225|1224|4551)\]|\[Errno 13\]|\b(?:LNK1104|LNK1168|C1041)\b|0xC0000022'),
    ('network', r'\[WinError (?:10051|10053|10054|10060|10061|10065|11001|11002|11004)\]|\[Errno 1100[124]\]|HTTP Error \d{3}'),
]

def classify(tail, title=''):
    """The failure kind for a failed tool, from the end of its output: numeric codes first, then English words."""
    text = '\n'.join(tail)
    for kind, pattern in CODE_KINDS:
        if re.search(pattern, text) and not (kind == 'network' and 'CMake + Ninja' in title): return kind
    if re.search(r'(?i)C1060|LNK1102|out of (heap )?memory|not enough memory|std::bad_alloc', text): return 'out_of_memory'
    if re.search(r'(?i)no space left|not enough space on the disk|There is not enough space|disk full|C1083.*write|LNK1106', text): return 'disk_full'
    if re.search(r'(?i)access is denied|PermissionError|LNK1104|LNK1168|being used by another process|error C1041|Errno 13|WinError 5', text): return 'access_denied'
    if re.search(r'(?i)does not support this graphics card', text): return 'gpu'
    if 'CMake + Ninja' not in title and re.search(r'(?i)URLError|getaddrinfo|timed out|ConnectionReset|HTTP Error|Could not find a version|No matching distribution|Could not download', text): return 'network'
    if 'CMake + Ninja' in title and compiler_error(tail): return 'compile_error'     # the compiler or linker said what is wrong
    if 'CMake + Ninja' in title or re.search(r'(?i)ninja: build stopped|FAILED:', text): return 'build_failed'
    if re.search(r'(?i)xiso|XisoError', title + text): return 'disc'
    return 'unexpected'
def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''): h.update(block)
    return h.hexdigest()

def ram_bytes():
    return precheck.memory_info()[0]

is_install = precheck.is_install

def install_relation(dest):
    """How the installer in this exe relates to the install in `dest`: none, older (update available), same, newer.
    An install made before the version stamp existed counts as older."""
    if not is_install(dest): return 'none'
    stamp = precheck.read_stamp(dest)
    if not stamp: return 'older'
    if stamp.get('kit_id') == KIT_ID: return 'same'
    return 'newer' if str(stamp.get('version', '')) > VERSION else 'older'

# ---------------------------------------------------------------------------- environment

NO_COMPILER = ('The game is compiled on this PC with Microsoft\'s free C++ Build Tools, which are not installed. '
               'Let the installer install them (on the Systems page, or pass --install-build-tools), '
               'or install Visual Studio 2022 or newer with "Desktop development with C++" yourself.')

VC_X64 = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
# What a Visual Studio install can lack, as (words for the Systems page, the component the Visual Studio Installer adds).
VS_PARTS = {'vcvars': ('the C++ x64 compiler (MSVC)', VC_X64),
            'cmake': ('C++ CMake tools for Windows', 'Microsoft.VisualStudio.Component.VC.CMake.Project'),
            'sdk': ('a Windows SDK', 'Microsoft.VisualStudio.Component.Windows11SDK.22621')}

def _ver(text):
    return tuple(int(x) for x in re.findall(r'\d+', str(text))[:4])

def windows_sdk():
    """windows.h of the newest Windows 10/11 SDK that also has the x64 kernel32.lib and rc.exe the game's link needs, else None."""
    base = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Windows Kits' / '10'
    try:
        vers = sorted((d.name for d in (base / 'Lib').iterdir() if (d / 'um' / 'x64' / 'kernel32.lib').is_file()
                       and (base / 'bin' / d.name / 'x64' / 'rc.exe').is_file() and (base / 'Include' / d.name / 'um' / 'windows.h').is_file()),
                      key=_ver, reverse=True)
    except OSError:
        return None
    return base / 'Include' / vers[0] / 'um' / 'windows.h' if vers else None

def vs_instances():
    """Every VS 2022-or-newer instance (stable and preview, complete or not) that has the x64 C++ tools, as vswhere's JSON."""
    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    if not vswhere.is_file(): return []
    found = {}
    for extra in ([], ['-prerelease']):
        try:
            r = subprocess.run([str(vswhere), '-all', '-products', '*', '-requires', VC_X64, '-version', '[17.0,)', '-format', 'json', '-utf8'] + extra,
                               capture_output=True, timeout=60, stdin=subprocess.DEVNULL, creationflags=NO_CONSOLE)
            for i in json.loads(r.stdout.decode('utf-8', errors='replace') or '[]'):
                if i.get('installationPath'): found[os.path.normcase(i['installationPath'])] = i
        except (OSError, ValueError, subprocess.TimeoutExpired):
            pass
    return list(found.values())

def find_visual_studio(choose=None):
    """(vcvars64.bat, cmake.exe, ninja.exe, VS folder) of the newest COMPLETE VS/Build Tools instance with vcvars64.bat, cl.exe, CMake,
    Ninja and a Windows SDK (stable releases before previews), or of the installation folder `choose`. Raises Stop naming what is missing."""
    cands = [{'installationPath': str(choose)}] if choose else vs_instances()
    if not cands: raise Stop(NO_COMPILER, 'compiler', 'missing')
    cands.sort(key=lambda i: (i.get('isComplete', True) is not False, not i.get('isPrerelease'), _ver(i.get('installationVersion', '0'))), reverse=True)
    first = None
    for i in cands:
        vs = Path(i['installationPath'])
        vcvars = vs / 'VC/Auxiliary/Build/vcvars64.bat'
        cmake_dir = vs / 'Common7/IDE/CommonExtensions/Microsoft/CMake'
        cmake, ninja = cmake_dir / 'CMake/bin/cmake.exe', cmake_dir / 'Ninja/ninja.exe'
        if not cmake.is_file(): cmake = Path(shutil.which('cmake') or '')
        if not ninja.is_file(): ninja = Path(shutil.which('ninja') or '')
        missing = []
        if i.get('isComplete') is False or not vcvars.is_file() or not next((vs / 'VC/Tools/MSVC').glob('*/bin/Hostx64/x64/cl.exe'), None):
            missing.append('vcvars')
        if not cmake.is_file() or not ninja.is_file(): missing.append('cmake')
        if not windows_sdk(): missing.append('sdk')
        if not missing: return vcvars, cmake, ninja, vs
        first = first or (vs, missing)
    vs, missing = first
    e = Stop('Visual Studio C++ in %s is incomplete: it lacks %s. In the Visual Studio Installer, add %s.' % (
        vs, ' and '.join(VS_PARTS[m][0] for m in missing), ', '.join('"%s"' % VS_PARTS[m][0] for m in missing)), 'compiler', missing[0])
    e.vs, e.missing = vs, missing
    raise e

def compiler_state(choose=None):
    """{'state': ready|incomplete|missing, 'vs', 'name', 'detail', 'code', 'missing' (codes), 'add' (VS components)} for the Systems page."""
    try:
        _, _, _, vs = find_visual_studio(choose)
    except Stop as e:
        miss = getattr(e, 'missing', [])
        return {'state': 'missing' if e.code == 'missing' else 'incomplete', 'detail': str(e), 'code': e.code, 'name': '',
                'vs': str(e.vs) if getattr(e, 'vs', None) else None, 'missing': miss, 'add': [VS_PARTS[m][1] for m in miss]}
    name = ''
    try:
        vsw = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
        name = subprocess.run([str(vsw), '-path', str(vs), '-property', 'displayName'], capture_output=True, text=True,
                              creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0)).stdout.strip()
    except OSError:
        pass
    return {'state': 'ready', 'vs': str(vs), 'name': name or Path(str(vs)).name, 'detail': '', 'code': None}

def open_vs_installer(vs=None, add=None):
    """Start the Visual Studio Installer (its own window) to add a missing component. True if it started."""
    exe = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vs_installer.exe'
    if not exe.is_file(): return False
    args = [str(exe)]
    if vs: args += ['modify', '--installPath', str(vs)] + [x for a in (add or [VS_PARTS['cmake'][1]]) for x in ('--add', a)]
    subprocess.Popen(args)
    return True

def repair_venv_home(cfg, python_dir):
    """A venv made by an earlier run points at that run's temporary Python (gone since). Point `home` at the Python running now.
    True if the file was changed."""
    cfg = Path(cfg)
    try:
        lines = cfg.read_text(encoding='utf-8').splitlines()
    except OSError:
        return False
    changed = False
    for n, line in enumerate(lines):
        key, _, value = line.partition('=')
        if key.strip().lower() == 'home' and value.strip() and not Path(value.strip()).exists():
            lines[n] = 'home = %s' % python_dir; changed = True
    if changed: cfg.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return changed

def console_python(exe):
    """The console python.exe beside a pythonw.exe (the installer window runs under pythonw.exe). A windowless parent's console
    children (venv, pip) each get a new visible console window from Windows; closing it stops them (0xC000013A)."""
    p = Path(str(exe))
    if p.name.lower() == 'pythonw.exe' and (p.parent / 'python.exe').is_file(): return str(p.parent / 'python.exe')
    return str(exe)

def enhancer_python():
    """A Python 3.11-3.13 (64-bit) for the Enhancer's PyTorch: the installer's own (3.13), else py -3.13..3.11. Never pythonw.exe
    when its python.exe exists (console_python)."""
    return console_python(_enhancer_python())

def _enhancer_python():
    if (3, 11) <= sys.version_info[:2] <= (3, 13) and sys.maxsize > 2 ** 32: return sys.executable
    for v in ('3.13', '3.12', '3.11'):
        try:
            r = subprocess.run(['py', '-' + v, '-c', 'import sys,struct;print(sys.executable);print(struct.calcsize("P"))'],
                               capture_output=True, text=True, timeout=30, stdin=subprocess.DEVNULL, creationflags=NO_CONSOLE)
        except (OSError, subprocess.TimeoutExpired):
            break
        lines = r.stdout.split()
        if r.returncode == 0 and len(lines) == 2 and lines[1] == '8': return lines[0]
    if (3, 11) <= sys.version_info[:2] <= (3, 13): return sys.executable
    raise Stop('The enhanced content needs Python 3.11, 3.12 or 3.13 (64-bit) for PyTorch; install Python 3.13 '
               'from python.org and run the installer again.')

def nvidia_gpu():
    smi = shutil.which('nvidia-smi') or r'C:\Windows\System32\nvidia-smi.exe'
    try:
        r = subprocess.run([smi, '--query-gpu=name,memory.total', '--format=csv,noheader'], capture_output=True, text=True, timeout=30,
                           stdin=subprocess.DEVNULL, creationflags=NO_CONSOLE)
        return r.stdout.strip().splitlines()[0] if r.returncode == 0 and r.stdout.strip() else None
    except (OSError, subprocess.TimeoutExpired):
        return None

def texture_engine(nvidia, gpus, allow_cpu=False, force=''):
    """Which upscaler makes the enhanced textures: 'pytorch' (an NVIDIA GPU with CUDA; or the CPU with --allow-cpu),
    'vulkan' (realesrgan-ncnn-vulkan on any other graphics card: AMD, Intel, ...), or None (no graphics card found).
    `force` (MA2_TEXTURE_ENGINE) picks one outright."""
    force = (force or '').strip().lower()
    if force in ('pytorch', 'vulkan'): return force
    if nvidia: return 'pytorch'
    if gpus: return 'vulkan'
    return 'pytorch' if allow_cpu else None

VULKAN_NOTE = 'new in 1.1.2; not yet tested on AMD or Intel cards'
VIDEO_VULKAN_NOTE = 'new in 1.1.3; not yet tested on AMD or Intel cards'
VULKAN_NOTICE = 'realesrgan-ncnn-vulkan.txt'      # enhancer/LICENSES: installed only when the Vulkan upscaler made textures

def needed_space(opts, done, engine='pytorch', video_engine=None):
    """Bytes still to be written (game files and build) plus each option's output and temporary files.
    engine: the texture upscaler, video_engine: the cutscene one (default: the same); 'vulkan' needs no PyTorch."""
    need = 0
    if 'extract' not in done: need += 3.0 * GB
    if 'build' not in done: need += 1.5 * GB
    if '2x' in opts['textures'] and 'textures-2x' not in done: need += 4 * GB
    if '4x' in opts['textures'] and 'textures-4x' not in done: need += 9 * GB
    if (opts['textures'] or opts['videos']) and not ({'textures-2x', 'textures-4x', 'videos'} & done):
        torch = (opts['textures'] and engine != 'vulkan') or (opts['videos'] and (video_engine or engine) != 'vulkan')
        need += 6 * GB if torch else 0.5 * GB      # PyTorch; the Vulkan tool and NumPy/Pillow
    if opts['videos'] and 'videos' not in done: need += 25 * GB
    return need

# ---------------------------------------------------------------------------- Build Tools, shortcuts

def install_build_tools(work, log, cancelled=lambda: False):
    """Download Microsoft's Build Tools bootstrapper, check its Microsoft signature, and run it
    (it asks for administrator approval and shows its own progress)."""
    import urllib.request
    work = Path(work); work.mkdir(parents=True, exist_ok=True)
    need = 8 * GB
    sysdrive = os.environ.get('SystemDrive', 'C:') + '\\'
    try:
        free = shutil.disk_usage(sysdrive).free
    except OSError:
        free = need
    if free < need:
        raise Stop('Microsoft\'s C++ Build Tools need about 8 GB free on %s (%.1f GB free). Free up space there and try again.'
                   % (sysdrive, free / GB), 'disk_full')
    boot = work / 'vs_buildtools.exe'
    for url in build_tools_urls():
        try:
            log('  downloading Microsoft C++ Build Tools installer: %s' % url)
            req = urllib.request.Request(url, headers={'User-Agent': 'MechAssault2-Recomp-Installer'})
            with urllib.request.urlopen(req, timeout=120) as r, open(boot, 'wb') as f: shutil.copyfileobj(r, f)
            break
        except OSError as e:
            log('    not available (%s)' % e)
    else:
        raise Stop('Could not download the Microsoft C++ Build Tools (no internet connection?). ' + NO_COMPILER, 'network')
    if not microsoft_signed(boot):
        raise Stop('The downloaded Build Tools installer is not signed by Microsoft; it was not run.', 'network')
    log('  installing Microsoft C++ Build Tools (about 6 GB; approve the administrator prompt; '
        'their own window shows progress, 10-30 minutes)')

    class SEI(ctypes.Structure):
        _fields_ = [('cbSize', ctypes.c_ulong), ('fMask', ctypes.c_ulong), ('hwnd', ctypes.c_void_p),
                    ('lpVerb', ctypes.c_wchar_p), ('lpFile', ctypes.c_wchar_p), ('lpParameters', ctypes.c_wchar_p),
                    ('lpDirectory', ctypes.c_wchar_p), ('nShow', ctypes.c_int), ('hInstApp', ctypes.c_void_p),
                    ('lpIDList', ctypes.c_void_p), ('lpClass', ctypes.c_wchar_p), ('hkeyClass', ctypes.c_void_p),
                    ('dwHotKey', ctypes.c_ulong), ('hIcon', ctypes.c_void_p), ('hProcess', ctypes.c_void_p)]
    info = SEI(); info.cbSize = ctypes.sizeof(SEI); info.fMask = 0x40      # SEE_MASK_NOCLOSEPROCESS
    info.lpVerb = 'runas'; info.lpFile = str(boot); info.nShow = 1
    info.lpParameters = subprocess.list2cmdline(BUILD_TOOLS_ARGS)
    shell32 = ctypes.WinDLL('shell32', use_last_error=True); kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    shell32.ShellExecuteExW.argtypes = [ctypes.POINTER(SEI)]; shell32.ShellExecuteExW.restype = ctypes.c_int
    kernel32.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]; kernel32.WaitForSingleObject.restype = ctypes.c_ulong
    kernel32.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]; kernel32.GetExitCodeProcess.restype = ctypes.c_int
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
    if not shell32.ShellExecuteExW(ctypes.byref(info)) or not info.hProcess:
        err = ctypes.get_last_error()
        if err == 1223: raise Stop('The Build Tools installation was cancelled at the administrator prompt (Windows asks for approval; '
                                   'choose Yes to install them). ' + NO_COMPILER, 'compiler', 'uac')
        raise Stop('The Build Tools installer could not be started (Windows error %d). ' % err + NO_COMPILER, 'compiler')
    try:                                          # the PC stays awake while the Build Tools install (as during the install steps)
        kernel32.SetThreadExecutionState(0x80000001)
        while True:
            w = kernel32.WaitForSingleObject(info.hProcess, 1000)
            if w == 0: break                                              # WAIT_OBJECT_0: it ended
            if w != 0x102: raise Stop('Lost track of the Build Tools installer (Windows error %d).' % ctypes.get_last_error(), 'compiler')
            if cancelled(): raise Stop('Cancelled (the Build Tools installer keeps running on its own).', 'cancelled')
        code = ctypes.c_ulong()
        if not kernel32.GetExitCodeProcess(info.hProcess, ctypes.byref(code)):
            raise Stop('The Build Tools installer ended, but its result could not be read (Windows error %d).' % ctypes.get_last_error(), 'compiler')
    finally:
        kernel32.CloseHandle(info.hProcess); kernel32.SetThreadExecutionState(0x80000000)
    boot.unlink(missing_ok=True)
    code = code.value
    log('  Build Tools installer exit code %d' % code)
    if code in (3010, 1641):
        mark_build_tools_installed(); log('  ' + REBOOT)
        raise Stop(REBOOT, 'compiler', 'reboot')
    if code != 0:
        text, kind = BUILD_TOOLS_EXIT.get(code, ('The Microsoft C++ Build Tools installation did not finish (%s).' % exit_text(code), 'compiler'))
        raise Stop('%s (Build Tools exit code %d.) ' % (text, code) + ('' if kind == 'cancelled' else NO_COMPILER), kind, code)
    log('  Microsoft C++ Build Tools installed')
    mark_build_tools_installed()

class _GUID(ctypes.Structure):
    _fields_ = [('a', ctypes.c_ulong), ('b', ctypes.c_ushort), ('c', ctypes.c_ushort), ('d', ctypes.c_ubyte * 8)]

def _guid(text):
    g = _GUID(); ctypes.windll.ole32.CLSIDFromString(text, ctypes.byref(g)); return g

def _vcall(obj, index, argtypes, *args):
    vtbl = ctypes.cast(obj, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p)))[0]
    return ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, *argtypes)(vtbl[index])(obj, *args)

def create_shortcut(lnk, target, workdir, description='', icon=None):
    """Write a .lnk with IShellLinkW (no script host). Raises OSError on failure."""
    ole32 = ctypes.windll.ole32
    ole32.CoInitialize(None)
    link, persist = ctypes.c_void_p(), ctypes.c_void_p()
    try:
        hr = ole32.CoCreateInstance(ctypes.byref(_guid('{00021401-0000-0000-C000-000000000046}')), None, 1,
                                    ctypes.byref(_guid('{000214F9-0000-0000-C000-000000000046}')), ctypes.byref(link))
        if hr < 0 or not link.value: raise OSError('CoCreateInstance failed (0x%08X)' % (hr & 0xFFFFFFFF))
        W = ctypes.c_wchar_p
        _vcall(link, 20, [W], str(target))                        # SetPath
        _vcall(link, 9, [W], str(workdir))                        # SetWorkingDirectory
        if description: _vcall(link, 7, [W], description)         # SetDescription
        _vcall(link, 17, [W, ctypes.c_int], str(icon or target), 0)   # SetIconLocation
        hr = _vcall(link, 0, [ctypes.POINTER(_GUID), ctypes.POINTER(ctypes.c_void_p)],
                    ctypes.byref(_guid('{0000010B-0000-0000-C000-000000000046}')), ctypes.byref(persist))
        if hr < 0: raise OSError('no IPersistFile')
        Path(lnk).parent.mkdir(parents=True, exist_ok=True)
        hr = _vcall(persist, 6, [W, ctypes.c_int], str(lnk), 1)   # IPersistFile::Save
        if hr < 0: raise OSError('could not save %s (0x%08X)' % (lnk, hr & 0xFFFFFFFF))
    finally:
        for o in (persist, link):
            if o.value: _vcall(o, 2, [])
        ole32.CoUninitialize()

def known_folder(which):
    """The Desktop or Start-menu Programs folder (follows OneDrive redirection); MA2_SHORTCUT_DIR overrides it for tests."""
    override = os.environ.get('MA2_SHORTCUT_DIR')
    if override: return Path(override) / which
    ids = {'desktop': '{B4BFCC3A-DB2C-424C-B029-7FE99A87C641}', 'start': '{A77F5D77-2E2B-44C3-A6A2-ABA601054A51}'}
    out = ctypes.c_wchar_p()
    if ctypes.windll.shell32.SHGetKnownFolderPath(ctypes.byref(_guid(ids[which])), 0, None, ctypes.byref(out)) != 0:
        raise OSError('no %s folder' % which)
    path = out.value; ctypes.windll.ole32.CoTaskMemFree(out)
    return Path(path)

# ---------------------------------------------------------------------------- runner

# ---------------------------------------------------------------------------- uninstall

def documents_dir():
    """The user's Documents folder (MA2_DOCUMENTS_DIR overrides it for tests)."""
    override = os.environ.get('MA2_DOCUMENTS_DIR')
    if override: return Path(override)
    try:
        out = ctypes.c_wchar_p()
        if ctypes.windll.shell32.SHGetKnownFolderPath(ctypes.byref(_guid('{FDD39AD0-238F-46AF-ADB4-6C85480369C7}')), 0, None, ctypes.byref(out)) == 0:
            path = out.value; ctypes.windll.ole32.CoTaskMemFree(out); return Path(path)
    except Exception:
        pass
    return Path.home() / 'Documents'

def scenarios_dir():
    r"""The player's own scenarios, Documents\MechAssault 2\Scenarios. The uninstaller never deletes them."""
    return documents_dir() / 'MechAssault 2' / 'Scenarios'

def _is_link(path):
    """A symbolic link or a junction: removed as a link, never followed."""
    try:
        return os.path.islink(path) or bool(getattr(os.path, 'isjunction', lambda p: False)(path))
    except OSError:
        return False

def safe_rmtree(path, failed):
    """Delete a folder tree. Links and junctions inside it are removed as links (what they point to is never touched).
    Anything that cannot be deleted is appended to `failed` as (path, reason); the rest is still deleted."""
    path = Path(path)
    try:
        entries = list(os.scandir(path))
    except OSError as e:
        failed.append((str(path), e.strerror or str(e))); return
    for entry in entries:
        p = entry.path
        try:
            if _is_link(p):
                try: os.unlink(p)
                except OSError: os.rmdir(p)
            elif entry.is_dir(follow_symlinks=False):
                safe_rmtree(p, failed)
            else:
                try: os.unlink(p)
                except PermissionError:
                    os.chmod(p, 0o700); os.unlink(p)
        except OSError as e:
            failed.append((str(p), e.strerror or str(e)))
    try:
        os.rmdir(path)
    except OSError as e:
        if not any(f[0].startswith(str(path)) for f in failed): failed.append((str(path), e.strerror or str(e)))

def mark_build_tools_installed():
    r"""installer.json (in %LOCALAPPDATA%\MechAssault2) records that this installer installed Microsoft's Build Tools, so Uninstall can say so."""
    try:
        d = precheck.local_dir(); d.mkdir(parents=True, exist_ok=True)
        j = precheck.read_json(d / 'installer.json'); j = j if isinstance(j, dict) else {}
        j['build_tools_installed_by_us'] = True
        (d / 'installer.json').write_text(json.dumps(j, indent=1), encoding='utf-8')
    except OSError:
        pass

def uninstall_plan(dest, delete_user_data=False):
    r"""What Uninstall would delete in `dest`, as {'dest', 'shortcuts', 'caches', 'user_data'} (paths; nothing is touched), or raises Stop if
    `dest` is not an install of ours or deleting it could take something that is not ours with it."""
    dest = Path(precheck.clean_path(str(dest))).resolve()
    if not is_install(dest):
        raise Stop('%s is not a MechAssault 2 Recomp install, so nothing was deleted.' % dest)
    local = precheck.local_dir()
    for keep, what in ((scenarios_dir(), 'your scenarios folder (Documents\\MechAssault 2\\Scenarios)'), (local, 'your settings and saves folder'),
                       (documents_dir(), 'your Documents folder'), (Path.home(), 'your user folder')):
        if precheck._under(keep, dest):
            raise Stop('%s contains %s, so nothing was deleted. Delete the game files by hand.' % (dest, what))
    if precheck._under(dest, scenarios_dir()):
        raise Stop('%s is inside your scenarios folder, so nothing was deleted. Delete the game files by hand.' % dest)
    if dest.parent == dest:
        raise Stop('%s is a drive, not an install folder; nothing was deleted.' % dest)
    stamp = precheck.read_stamp(dest) or {}
    ours = ('MechAssault 2 (Recomp).lnk', 'MA2 Scenario Editor.lnk')
    shortcuts = [Path(s) for s in stamp.get('shortcuts', []) if isinstance(s, str) and Path(s).name in ours and Path(s).is_file()]
    caches = [p for p in (local / 'ShaderCache', local / 'NavCache', Path(os.environ.get('LOCALAPPDATA') or Path.home() / 'AppData' / 'Local') / 'xboxrecomp' / 'shader-cache')
              if p.exists()]
    return {'dest': dest, 'shortcuts': shortcuts, 'caches': caches, 'user_data': local if delete_user_data and local.exists() else None}

def uninstall(dest, delete_user_data=False, log=print):
    r"""Remove an install made by this installer: its shortcuts, the install folder (only because is_install(dest) is true), the shader and
    route caches and, if asked, %LOCALAPPDATA%\MechAssault2 (settings, saves, logs). Documents\MechAssault 2\Scenarios is never touched.
    Returns {'removed': [...], 'failed': [(path, reason)], 'text': the closing message}; raises Stop before deleting anything if `dest` is not ours."""
    plan = uninstall_plan(dest, delete_user_data)
    dest = plan['dest']
    local = precheck.local_dir()
    j = precheck.read_json(local / 'installer.json'); j = j if isinstance(j, dict) else {}
    build_tools = bool(j.get('build_tools_installed_by_us'))
    removed, failed = [], []
    for lnk in plan['shortcuts']:
        try:
            lnk.unlink(); removed.append(str(lnk)); log('  removed shortcut %s' % lnk)
        except OSError as e:
            failed.append((str(lnk), e.strerror or str(e)))
    log('  removing the install folder %s' % dest)
    before = len(failed)
    safe_rmtree(dest, failed)
    if len(failed) == before: removed.append(str(dest))
    for cache in plan['caches']:
        log('  removing %s' % cache)
        n = len(failed); safe_rmtree(cache, failed)
        if len(failed) == n: removed.append(str(cache))
    if not delete_user_data and j.get('dest') and os.path.normcase(str(j['dest'])) == os.path.normcase(str(dest)):
        j.pop('dest', None)                                              # forget this folder; keep the rest (the Build Tools note)
        try: (local / 'installer.json').write_text(json.dumps(j, indent=1), encoding='utf-8')
        except OSError: pass
    if plan['user_data'] is not None:
        log('  removing your settings and saves in %s' % local)
        n = len(failed); safe_rmtree(local, failed)
        if len(failed) == n: removed.append(str(local))
    lines = []
    if failed:
        shown = '; '.join('%s (%s)' % f for f in failed[:3])
        lines.append('Not everything could be deleted: %s. Close MechAssault 2 and the Scenario Editor (and anything using the folder), then run Uninstall again.' % shown)
    else:
        lines.append('MechAssault 2 was removed from %s.' % dest)
    lines.append('Your settings and saves were %s.' % ('deleted' if plan['user_data'] is not None and not failed else 'kept in %s' % local))
    lines.append('Your own scenarios in %s were not touched.' % scenarios_dir())
    if build_tools:
        lines.append("Microsoft's C++ Build Tools were installed by this installer and are not removed. Remove them in Windows Settings > Apps if you no longer need them.")
    text = '\n'.join(lines)
    for line in lines: log(line)
    return {'removed': removed, 'failed': failed, 'text': text}

BUILD_PREFIX = 'MA2RecompBuild-'
plain_path = precheck.plain_path                 # can the C++ build run under this path (a whitelist of characters)

def build_bases(anchor):
    """Where build_root() may put an ASCII-path build folder, in order: the install drive's root, ProgramData, Public."""
    return [Path(anchor), Path(os.environ.get('ProgramData', 'C:/ProgramData')), Path(os.environ.get('PUBLIC', 'C:/Users/Public'))]

def remove_build_folder(folder, owner=None):
    """Remove one MA2RecompBuild-* folder made by this installer. It must carry our owner.txt (and name `owner`, when given):
    nothing else is ever deleted. True if it is gone."""
    folder = Path(folder)
    if not folder.name.startswith(BUILD_PREFIX) or not folder.is_dir(): return False
    try:
        who = (folder / 'owner.txt').read_text(encoding='utf-8').strip()
    except (OSError, ValueError):
        return False
    if owner is not None and os.path.normcase(who) != os.path.normcase(str(owner)): return False
    shutil.rmtree(folder, ignore_errors=True)
    return not folder.exists()

def clean_stale_build_folders(bases):
    """Build folders whose install folder no longer exists (the user deleted it by hand) are removed. Returns what was removed."""
    gone = []
    for base in bases:
        try:
            found = sorted(Path(base).glob(BUILD_PREFIX + '*'))
        except OSError:
            continue
        for f in found:
            try:
                who = (f / 'owner.txt').read_text(encoding='utf-8').strip()
            except (OSError, ValueError):
                continue                                        # not ours (or unreadable): leave it alone
            if who and not Path(who).exists() and remove_build_folder(f): gone.append(f)
    return gone

LOCK_CODES = (5, 32, 33)          # access denied / sharing / lock violation: Defender or the indexer holding a file for a moment
BLOCKED_CODES = {225: 'Windows reported a virus or unwanted software in it and blocked it (WinError 225)',
                 4551: 'Smart App Control blocked it (WinError 4551)'}

def retry_io(fn, what, log=lambda s: None, owned=None):
    """Run fn() (a copy, replace, rename, unlink or rmtree of an installer-owned file); on WinError 5/32/33 wait and try again,
    0.2 s doubling up to 5 s (about 11 s in all). `owned`: a staged file of ours whose read-only flag may be cleared. WinError 225
    (threat found) and 4551 (Smart App Control) stop at once as Stop('access_denied'); every retry is logged with its code."""
    delay = 0.2
    while True:
        try:
            return fn()
        except OSError as e:
            code = getattr(e, 'winerror', None)
            if code in BLOCKED_CODES:
                log('  %s: %s' % (what, BLOCKED_CODES[code]))
                raise Stop('Could not %s: %s. Allow the install folder in your antivirus (or turn Smart App Control off), then try again.'
                           % (what, BLOCKED_CODES[code]), 'access_denied', code) from e
            if code not in LOCK_CODES or delay > 5: raise
            log('  (%s: WinError %d, the file is busy; trying again in %.1f s)' % (what, code, delay))
            if code == 5 and owned is not None:
                try: os.chmod(owned, 0o666)
                except OSError: pass
            time.sleep(delay); delay = min(delay * 2, 5) if delay < 5 else 6

def _rmtree_owned(path):
    def clear(fn, p, exc):
        os.chmod(p, 0o666); fn(p)
    kw = {'onexc': clear} if sys.version_info >= (3, 12) else {'onerror': clear}
    shutil.rmtree(path, **kw)

def put_back(pairs, log=lambda s: None):
    """Rename each kept <file>.previous back over the new file: the previous version again. Returns the pairs that could NOT
    be put back (they stay recorded, so a later run tries again; their .previous file is never deleted)."""
    failed = []
    for orig, bak in pairs:
        orig, bak = Path(orig), Path(bak)
        if not bak.exists(): continue
        try:
            if orig.is_dir(): retry_io(lambda: shutil.rmtree(orig), 'remove %s' % orig.name, log)
            elif orig.exists(): retry_io(orig.unlink, 'remove %s' % orig.name, log, owned=orig)
            retry_io(lambda: bak.rename(orig), 'restore %s' % orig.name, log)
        except (OSError, Stop) as e:
            failed.append([str(orig), str(bak)])
            log('  could not restore %s (%s)' % (orig.name, getattr(e, 'winerror', None) or e.strerror or e))
    return failed

def start_over(dest):
    """Delete the unfinished work of an install in `dest`: its _work folder and, if the build ran in an ASCII-path folder elsewhere, that one too.
    The extracted game files and everything else in the install folder are kept."""
    dest = Path(dest)
    state = precheck.read_state(dest) or {}
    if state.get('previous') and not state.get('core_done'):     # an update stopped mid-install (crash, power cut): the old game back first
        put_back(state['previous'])
    shutil.rmtree(dest / '_work', ignore_errors=True)
    if state.get('build_root'): remove_build_folder(state['build_root'], owner=dest)

class Installer:
    def __init__(self, xiso, dest, textures=(), videos=False, jobs=None, keep_work=False, allow_cpu=False,
                 log=print, step=lambda *args: None, install_build_tools=False, vs=None, update=False,
                 shortcuts=(), keep_awake=True, low_priority=False, merge_settings=True, switch_textures=False):
        # step(index, total, title, name, skipped) is called as each engine step starts (or is skipped as already done)
        self.xiso = Path(precheck.clean_path(str(xiso))).resolve() if xiso else None      # a disc image, or a folder copied from the disc
        if self.xiso and self.xiso.is_file() and self.xiso.name.lower() == RELEASE['xbe'].lower(): self.xiso = self.xiso.parent     # ma2.xbe itself: its folder
        self.dest = Path(precheck.clean_path(str(dest))).resolve()
        self.update, self.shortcuts = update, tuple(shortcuts)
        self.keep_awake, self.low_priority = keep_awake, low_priority
        self.merge_settings, self.switch_textures = merge_settings, switch_textures
        self.session_log = None; self.logfile = None; self._session = None; self.stage = ''
        self.failed_step = None; self.on_plan = lambda names: None
        self.work = self.dest / '_work'
        self.opts = {'textures': sorted(set(textures)), 'videos': bool(videos)}
        self.jobs, self.keep_work, self.allow_cpu = jobs, keep_work, allow_cpu
        self.until = None                         # maintainers: stop after this step (--until)
        self.install_build_tools, self.vs_choice = install_build_tools, vs
        self.log, self.on_step = log, step
        self.proc = None; self.cancelled = False
        self.state = {}; self.notes = []; self.game_ok = False; self.old_stamp = None

    # -- plumbing
    def cancel(self):
        self.cancelled = True
        p = self.proc
        if p and p.poll() is None:
            subprocess.run(['taskkill', '/T', '/F', '/PID', str(p.pid)], capture_output=True)

    def save(self):
        self.work.mkdir(parents=True, exist_ok=True)
        tmp = self.work / 'state.json.tmp'
        tmp.write_text(json.dumps(self.state, indent=1), encoding='utf-8'); tmp.replace(self.work / 'state.json')

    def done(self, name):
        self.state.setdefault('done', [])
        if name not in self.state['done']: self.state['done'].append(name)
        self.save()

    def run_cmd(self, args, cwd=None, env=None, quiet_pattern=None, title=None, hint=None):
        """Run a tool, streaming its output to the log (and the install log file). The tool gets no console window a user could close and
        no shared Ctrl+C (its own process group). `hint` is added to the failure message when the tool was stopped from outside."""
        if self.cancelled: raise Stop('Cancelled.', 'cancelled')
        if title: self.log('> ' + title)
        flags = NO_CONSOLE | (BELOW_NORMAL if self.low_priority else 0)
        self.proc = subprocess.Popen([str(a) for a in args], cwd=str(cwd) if cwd else None, env=env,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                     creationflags=flags)
        tail = []
        for raw in self.proc.stdout:
            line = raw.decode('utf-8', errors='replace').rstrip()
            if SECRET_LINE.search(line): continue
            tail = (tail + [line])[-40:]
            if not quiet_pattern or not re.search(quiet_pattern, line): self.log('  ' + line)
        code = self.proc.wait(); self.proc = None
        if self.cancelled: raise Stop('Cancelled.', 'cancelled')
        if code:
            meanings = [m for m in [exit_meaning(code)] + explain_codes('\n'.join(tail)) if m]
            meanings = [m for i, m in enumerate(meanings) if m not in meanings[:i]]
            for m in meanings: self.log('  (the tool %s)' % m)
            extra = ''
            if meanings and not exit_meaning(code): extra = ' A program it started %s.' % meanings[0]
            if meanings and hint: extra += ' ' + hint
            kind = classify(tail, title or '')
            if kind == 'compile_error': extra += ' The first error: %s' % compiler_error(tail)[:300]
            raise Stop('%s failed (%s).%s The last lines are above; the full log is %s.' % (
                title or Path(str(args[0])).name, exit_text(code), extra, self.session_log or self.work / 'install.log'),
                kind, code=code)

    def toolkit_env(self):
        env = dict(os.environ)
        env['PYTHONPATH'] = os.pathsep.join([str(self.work / 'toolkit'), str(KIT / 'toolkit' / 'vendor')])
        env['PYTHONHASHSEED'] = '0'; env['PYTHONDONTWRITEBYTECODE'] = '1'; env['PYTHONIOENCODING'] = 'utf-8'
        env['PYTHONUTF8'] = '1'
        return env

    # -- logs
    def _start_logs(self):
        r"""Log from the very first line to %LOCALAPPDATA%\MechAssault2\Logs\install-<time>.log (kept whatever happens)
        and, once the work folder exists, to <work>\install.log (resume)."""
        outer = self.log
        self.session_log = None
        try:
            d = precheck.logs_dir(); d.mkdir(parents=True, exist_ok=True)
            old = sorted(d.glob('install-*.log'), key=lambda f: f.stat().st_mtime)
            for f in old[:-19]:
                try: f.unlink()
                except OSError: pass
            self.session_log = d / ('install-%s.log' % time.strftime('%Y%m%d-%H%M%S'))
            self._session = open(self.session_log, 'a', encoding='utf-8')
        except OSError:
            self.session_log = None; self._session = None
        def log(line):
            shown = precheck.redact(line)            # no user name or profile folder in a log file (they get posted)
            for f in (self._session, self.logfile):
                if f is not None and not f.closed:
                    try: f.write(shown + '\n'); f.flush()
                    except OSError: pass
            outer(line)
        self._outer = outer; self.log = log

    def _stop_logs(self):
        self.log = self._outer
        for f in (self.logfile, self._session):
            if f is not None and not f.closed: f.close()

    # -- previous files (an update keeps the old game until the new one is complete)
    def keep_previous(self, path):
        path = Path(path)
        if not self.state.get('update') or not path.exists(): return
        bak = path.with_name(path.name + '.previous')
        if bak.exists(): return                   # an interrupted earlier attempt already kept the original
        pair, pairs = [str(path), str(bak)], self.state.setdefault('previous', [])
        if pair not in pairs: pairs.append(pair)
        self.save()                               # recorded BEFORE the rename: a crash in between can never lose the backup
        try:
            retry_io(lambda: path.rename(bak), 'replace %s' % path.name, self.log)
        except (OSError, Stop) as e:
            pairs.remove(pair); self.save()
            if isinstance(e, Stop): raise
            raise Stop('Can\'t replace %s (%s). Close MechAssault 2 and the Scenario Editor, then try again.' % (path.name, e.strerror), 'access_denied')

    def restore_previous(self):
        """Put the old game back after a failed or cancelled update."""
        pairs = self.state.get('previous') or []
        if not pairs: return
        failed = put_back(pairs, self.log)
        self.state['previous'] = failed               # what could not be put back stays recorded: the next run tries again
        self.state['done'] = [s for s in self.state.get('done', []) if s not in ('assets', 'install')]
        self.save()
        if failed:
            self.log('  the previous version was only partly restored: %d file(s) could not be put back (%s). Close MechAssault 2 and the '
                     'Scenario Editor, then run the installer again: it finishes the install or tries the restore again.'
                     % (len(failed), ', '.join(Path(o).name for o, _ in failed[:3])))
            self.notes.append('The previous version was only partly restored; run the installer again.')
        else:
            self.log('  the previous version was put back')

    def commit_previous(self):
        for orig, bak in self.state.get('previous') or []:
            bak = Path(bak)
            try:
                retry_io(lambda: _rmtree_owned(bak) if bak.is_dir() else bak.unlink(missing_ok=True), 'delete %s' % bak.name, self.log, owned=bak)
            except (OSError, Stop) as e:
                self.log('  (the old %s could not be deleted: %s; you can delete it)' % (bak.name, e))
        self.state['previous'] = []; self.save()

    def game_installed(self):
        """Is the game itself installed and final in this run (only optional content can still be missing)?"""
        return bool(self.state.get('addons_only') or self.state.get('core_done'))

    def finalize_core(self):
        """The game steps through 'install' are done: make the new game final before any optional step runs. The old version's
        .previous files go, the settings and the install stamp are written as for a finished install, and state['core_done'] makes a
        later Resume run only the optional steps. A failed or cancelled optional step can then never put the old game back."""
        self.log('  the game is installed; it stays installed whatever happens to the optional content')
        self.settle(core=True)

    def settle(self, core=False):
        """Settings, the stamp and the end of an update: run once the game is installed (finalize_core, core=True) and again by finish
        (idempotent). The stamp is written and core_done saved BEFORE the old version's .previous files are deleted."""
        ini = self.dest / 'pc_settings.ini'
        q = 2 if (self.dest / 'game_files/texture_packs/4x/index.json').is_file() else \
            1 if (self.dest / 'game_files/texture_packs/2x/index.json').is_file() else 0
        v = 1 if (self.dest / 'game_files/Movie/Enhanced/2x').is_dir() else 0
        if getattr(self, 'ini_ours', None) is None and ini.is_file():
            # This run did not copy the ini (add-on or resumed run): it is ours while it still matches what the stamp recorded.
            recorded = (precheck.read_stamp(self.dest) or {}).get('defaults_sha256')
            self.ini_ours = recorded is None or recorded == sha256(ini)
        if ini.is_file() and getattr(self, 'ini_ours', True):
            # First-launch defaults: the best installed textures and the enhanced videos when present.
            text = ini.read_text(encoding='utf-8')
            new = re.sub(r'(?m)^TextureQuality=.*$', 'TextureQuality=%d' % q, text)
            new = re.sub(r'(?m)^FMVQuality=.*$', 'FMVQuality=%d' % v, new)
            if new != text: ini.write_text(new, encoding='utf-8')
        if self.state.get('update') or self.switch_textures:
            self.update_live_settings(q, v)
        self.write_stamp(ini)
        if core: self.state['core_done'] = True; self.save()
        self.commit_previous()

    # -- steps
    def game_files_ok(self):
        xbe = self.dest / 'game_files' / RELEASE['xbe']
        try:
            return xbe.is_file() and sha256(xbe) == RELEASE['xbe_sha256']
        except OSError:
            return False

    def need_disc(self):
        """Does this run read the disc image? Not when only adding content, nor when updating over intact game files."""
        if self.game_installed() or 'extract' in self.state.get('done', []): return False
        return not (self.state.get('update') and self.game_ok)

    def steps(self):
        core = [('check', 'Checking requirements', self.check)]
        if not self.game_installed():                 # a Resume after the game was made final runs only the optional steps
            if self.state.get('update') and self.game_ok:
                core += [('verify', 'Checking the installed game files', self.verify)]
            else:
                core += [('disc', 'Checking the disc image', self.disc),
                         ('extract', 'Extracting game files', self.extract)]
            core += [('analyze', 'Analysing the game program', self.analyze),
                     ('recompile', 'Translating the game program to C', self.recompile),
                     ('generate', 'Generating helper code', self.generate),
                     ('patch', 'Applying the PC fixes and features', self.patch),
                     ('build', 'Compiling the game', self.build),
                     ('assets', 'Building the PC menus', self.assets),
                     ('install', 'Installing the game files', self.install)]
        for scale in self.opts['textures']:
            core.append(('textures-' + scale, 'Making %s enhanced textures' % scale,
                         lambda s=scale: self.textures(s)))
        if self.opts['videos']:
            core.append(('videos', 'Making the enhanced cutscene videos', self.videos))
        core.append(('finish', 'Finishing', self.finish))
        return core

    def step_names(self):
        return [name for name, _, _ in self.steps()]

    def run(self):
        self._start_logs()
        try:
            self._run()
        finally:
            self._stop_logs()

    def _run(self):
        state_file = self.work / 'state.json'
        created = not self.dest.exists()
        self.log('%s installer, release %s, kit %s, %s' % (build_label(), VERSION, KIT_ID, time.strftime('%Y-%m-%d %H:%M')))
        self.log('  log file: %s' % self.session_log)
        self.old_stamp = precheck.read_stamp(self.dest)
        try:
            for f in clean_stale_build_folders(build_bases(self.work.anchor)): self.log('  removed an old build folder: %s' % f)
        except Exception as e:
            self.log('  (old build folders were not checked: %s)' % e)
        if state_file.is_file():
            try:
                self.state = json.loads(state_file.read_text(encoding='utf-8'))
            except ValueError:
                self.state = {'done': []}
            self.log('Resuming the install in %s (finished steps are kept).' % self.dest)
            if not self.xiso and self.state.get('xiso'): self.xiso = Path(self.state['xiso'])
            if not self.game_installed() and (self.state.get('kit_id') != KIT_ID or
                                                      self.state.get('release') != RELEASE['release_executable_sha256']):
                keep = {'check', 'disc', 'extract', 'verify', 'textures-2x', 'textures-4x', 'videos'}
                dropped = [s for s in self.state.get('done', []) if s not in keep]
                if dropped:
                    self.state['done'] = [s for s in self.state.get('done', []) if s in keep]
                    self.log('This installer is newer than the one that started this install. Your extracted disc files are kept; '
                             'the build starts again (about 15 minutes).')
        else:
            if not created and any(p.name != '_work' for p in self.dest.iterdir()) and not is_install(self.dest):
                raise Stop('The install folder %s is not empty. Choose an empty folder (or an existing '
                           'MechAssault 2 Recomp install to add optional content).' % self.dest)
            existing = is_install(self.dest)
            if self.update and not existing:
                raise Stop('There is no MechAssault 2 Recomp install in %s to update.' % self.dest)
            relation = install_relation(self.dest)
            if existing and not self.update and relation in ('older', 'same'):
                self.log('  An existing install was found (%s). Adding optional content only; run with --update to update the game.'
                         % ('older version' if relation == 'older' else 'same version'))
            self.state = {'addons_only': existing and not self.update, 'update': bool(existing and self.update), 'done': []}
        self.state.update(xiso=str(self.xiso) if self.xiso else None, kit_id=KIT_ID, version=VERSION,
                          release=RELEASE['release_executable_sha256'])
        self.state.setdefault('started', time.strftime('%Y-%m-%dT%H:%M:%S'))
        self.game_ok = bool(self.state.get('update')) and self.game_files_ok()
        try:
            self.dest.mkdir(parents=True, exist_ok=True)
            self.work.mkdir(exist_ok=True)
        except OSError as e:
            raise Stop('Cannot create %s (%s). Choose a folder you can write to, e.g. C:\\Games\\MechAssault 2.'
                       % (self.dest, e.strerror or e), 'access_denied')
        self.logfile = open(self.work / 'install.log', 'a', encoding='utf-8')
        self.state['options'] = self.opts
        self.save()
        self.remember_install()
        try:
            self.run_steps()
        except BaseException as e:
            self.log('INSTALL STOPPED at "%s" (%s): %s' % (self.stage, getattr(e, 'kind', type(e).__name__), e))
            if self.game_installed():
                # Only optional content failed or was cancelled (or the finishing touches): the game stays installed at this version.
                # Never roll back; the stamp is already written; state.json stays so Resume runs only the optional steps.
                if isinstance(e, Stop) and self.stage in OPTIONAL_STEPS:
                    msg = extras_message(self.stage, e, cancelled=(e.kind == 'cancelled'))
                    self.log(msg)
                    raise Stop(msg, 'extras', code=e.code, cause=e.kind) from e
                raise
            if self.state.get('update'):
                self.restore_previous()
                if self.state.get('previous') and isinstance(e, Stop):      # never claim the old version is back when it is not
                    raise Stop('%s The previous version was only partly restored (%d file(s) could not be put back): close MechAssault 2 '
                               'and the Scenario Editor, then run the installer again.' % (e, len(self.state['previous'])),
                               e.kind, code=e.code) from e
            if isinstance(e, Stop) and not self.state.get('done') and not self.state.get('update') and e.code != 'reboot':   # (a restart: Resume continues)
                self.logfile.close()              # stopped before anything was made: leave no trace in the install folder
                shutil.rmtree(self.work, ignore_errors=True)
                if created and not any(self.dest.iterdir()): self.dest.rmdir()
            raise

    def remember_install(self, **extra):
        r"""%LOCALAPPDATA%\MechAssault2\installer.json names this folder, so the next run can offer Resume or Update.
        Other keys in the file (build_tools_installed_by_us) are kept."""
        try:
            d = precheck.local_dir(); d.mkdir(parents=True, exist_ok=True)
            j = precheck.read_json(d / 'installer.json'); j = j if isinstance(j, dict) else {}
            j.update(dest=str(self.dest), kit_id=KIT_ID, version=VERSION, public_version=PUBLIC_VERSION, time=time.strftime('%Y-%m-%dT%H:%M:%S'), **extra)
            (d / 'installer.json').write_text(json.dumps(j, indent=1), encoding='utf-8')
        except OSError:
            pass

    def run_steps(self):
        if self.keep_awake:
            try: ctypes.windll.kernel32.SetThreadExecutionState(0x80000001)      # ES_CONTINUOUS | ES_SYSTEM_REQUIRED
            except Exception: pass
        try:
            self.stale_done()
            steps = self.steps()
            self.on_plan([n for n, _, _ in steps])
            for index, (name, title, fn) in enumerate(steps, 1):
                if self.cancelled: raise Stop('Cancelled.', 'cancelled')
                if (name in OPTIONAL_STEPS or name == 'finish') and not self.game_installed() and 'install' in self.state.get('done', []):
                    self.stage = 'install'; self.finalize_core()          # before any optional step: the new game is final from here on
                self.stage = name
                self.state['last_title'], self.state['last_step'] = title, name      # Collect diagnostics names the step that failed
                skipped = (name in self.state['done'] and name not in ('check', 'finish')) or not self.wanted(name)
                self.on_step(index, len(steps), title, name, skipped)
                if skipped:
                    self.log('[%d/%d] %s: already done' % (index, len(steps), title)); continue
                self.log('[%d/%d] %s' % (index, len(steps), title))
                t0 = time.monotonic(); fn()
                self.log('    done in %s' % duration(time.monotonic() - t0))
                if name not in ('check', 'finish'): self.done(name)
                if name == self.until:
                    self.log('Stopped after "%s" as requested (--until); the work folder is %s.' % (name, self.work)); return
        finally:
            if self.keep_awake:
                try: ctypes.windll.kernel32.SetThreadExecutionState(0x80000000)
                except Exception: pass

    def stale_done(self):
        """Steps marked done whose key output is gone (quarantined by antivirus, a folder cleaned by hand) run again instead of being
        skipped. Returns their names."""
        if self.game_installed(): return []
        done = self.state.get('done', [])
        outputs = {'extract': lambda: self.dest / 'game_files' / RELEASE['xbe'],
                   'assets': lambda: self.dest / 'pc_assets',
                   'install': lambda: self.dest / 'MechAssault 2.exe'}
        if 'install' not in done: outputs['build'] = lambda: self.build_root() / 'build' / 'mechassault2_recomp.exe'
        stale = []
        for name in done:
            try:
                if name in outputs and not outputs[name]().exists(): stale.append(name)
            except (OSError, Stop):
                pass
        if stale:
            self.state['done'] = [n for n in done if n not in stale]; self.save()
            for n in stale: self.log('  "%s" was marked done, but its result is missing; it runs again' % n)
        return stale

    def check(self):
        if sys.version_info < (3, 10) or sys.maxsize < 2 ** 32:
            raise Stop('The installer needs 64-bit Python 3.10 or newer.', 'compiler')   # only when run outside the exe
        if self.need_disc() and (not self.xiso or not self.xiso.exists()):
            raise Stop('Choose your MechAssault 2 disc image (.xiso/.iso), or the folder copied from the disc.', 'disc')
        if not self.game_installed():
            try:
                self.vcvars, self.cmake, self.ninja, self.vs = find_visual_studio(self.vs_choice)
            except Stop:
                if not self.install_build_tools or self.vs_choice: raise
                self.get_build_tools()
                self.vcvars, self.cmake, self.ninja, self.vs = find_visual_studio()
            self.log('  C++ compiler: %s' % self.vs)
            self.state['toolchain'] = str(self.vs); self.save()                 # named in Collect diagnostics
            self.remember_install(toolchain=str(self.vs))
            if 'install' not in self.state.get('done', []):
                self.ffmpeg_dir()                  # FFmpeg now: a download problem shows in the first minute, not after the translation
            self.log('  CMake: %s\n  Ninja: %s' % (self.cmake, self.ninja))
        if self.opts['textures'] or self.opts['videos']:
            try:
                self.epy = enhancer_python()
            except Stop as e:
                self.skip_extras(('textures', 'videos'), e)
        if self.opts['textures'] or self.opts['videos']:
            self.log('  Enhancer Python: %s' % self.epy)
            gpu = nvidia_gpu()
            if gpu: self.log('  GPU: %s' % gpu)
            others = [] if gpu else precheck.detect_gpus()
            if self.opts['videos']:                   # NVIDIA: RealBasicVSR (PyTorch); any other card: Real-ESRGAN through Vulkan
                self.vid_engine = texture_engine(gpu, others, self.allow_cpu, os.environ.get('MA2_VIDEO_ENGINE'))
                if not self.vid_engine:
                    self.skip_extras(('videos',), Stop('The enhanced cutscene videos need a graphics card (an NVIDIA GPU with CUDA, or any '
                                                       'graphics card with Vulkan), and none was found. Untick that option; the game runs fine '
                                                       'with the original videos.', 'compiler'))
                elif self.vid_engine == 'vulkan':
                    card = gpu or ', '.join(g['name'] for g in others) or "this PC's graphics card"
                    self.log('  Cutscenes: through Vulkan on %s, frame by frame (realesrgan-ncnn-vulkan; %s; very slow)' % (card, VIDEO_VULKAN_NOTE))
            if self.opts['textures']:
                self.tex_engine = texture_engine(gpu, others, self.allow_cpu, os.environ.get('MA2_TEXTURE_ENGINE'))
                if not self.tex_engine:
                    self.skip_extras(('textures',), Stop('The enhanced textures need a graphics card (an NVIDIA GPU with CUDA, or any graphics card '
                                                         'with Vulkan), and none was found. Untick that option; the game runs fine with the original textures.', 'compiler'))
                if self.tex_engine == 'vulkan':
                    card = gpu or ', '.join(g['name'] for g in others) or "this PC's graphics card"
                    self.log('  Textures: through Vulkan on %s (realesrgan-ncnn-vulkan; %s)' % (card, VULKAN_NOTE))
            if not gpu and self.allow_cpu and 'pytorch' in (getattr(self, 'vid_engine', None), getattr(self, 'tex_engine', None)):
                self.log('  No NVIDIA GPU: enhancement runs on the CPU (extremely slow).')
        if precheck.smart_app_control() == 'enforce':
            self.log('  WARNING: Smart App Control is on. It may block the game this installer builds, because the build is not signed. '
                     'Turning Smart App Control off is a one-way Windows setting; the installer never changes it.')
        done = set(self.state.get('done', []))
        if self.game_ok: done.add('extract')
        if self.game_installed(): done |= {'extract', 'build'}
        if not self.game_installed():
            if str(self.dest).startswith('\\\\'):
                raise Stop('The C++ tools cannot build in a network folder (%s). Install to a folder on this PC, e.g. C:\\Games\\MechAssault 2.' % self.dest, 'access_denied')
            fs = precheck.volume_fs(self.dest).upper()
            if fs.startswith('FAT'):
                raise Stop('%s uses %s, which cannot hold the game build (files over 4 GB). Install to an NTFS drive.' % (self.dest.anchor, fs), 'disk_full')
        free = shutil.disk_usage(self.dest).free
        engine = getattr(self, 'tex_engine', None) or 'pytorch'
        need = needed_space(self.opts, done, engine, getattr(self, 'vid_engine', None) or 'pytorch')
        self.log('  Free space on the install drive: %.1f GB (needs about %.1f GB)' % (free / GB, need / GB))
        core = needed_space({'textures': [], 'videos': False}, done)
        if free < need and free >= core and not self.game_installed() and (self.opts['textures'] or self.opts['videos']):
            self.skip_extras(('textures', 'videos'), Stop('Not enough free space for the enhanced content: %.1f GB free, about %.1f GB needed on %s.'
                                                        % (free / GB, need / GB, self.dest.anchor), 'disk_full'))
            need = core
        if free < need:
            raise Stop('Not enough free space: %.1f GB free, about %.1f GB needed on %s.' % (free / GB, need / GB, self.dest.anchor), 'disk_full')
        temp = Path(os.environ.get('TEMP') or self.dest)        # pip and the AI tools unpack into TEMP (usually C:)
        if (self.opts['textures'] or self.opts['videos']) and temp.anchor.lower() != self.dest.anchor.lower():
            try: tfree = shutil.disk_usage(temp).free
            except OSError: tfree = 2 * GB
            if tfree < 2 * GB: self.log('  WARNING: only %.1f GB free on %s (the temporary folder); the enhanced content setup needs about 2 GB there.'
                                        % (tfree / GB, temp.anchor))

    def skip_extras(self, which, e):
        """An optional item's prerequisite failed: untick it and go on with the game (never stop the core install for it). An add-ons-only
        run (the game is already installed) stops, since the extras are all it does."""
        if self.game_installed(): raise e
        dropped = [w for w in which if (self.opts['textures'] if w == 'textures' else self.opts['videos'])]
        if not dropped: return
        for w in dropped: self.opts[w] = [] if w == 'textures' else False
        self.state['options'] = self.opts; self.save()
        msg = 'The %s will not be made: %s The game is installed without them; add them later with Add enhanced content.' % (
            ' and '.join('enhanced ' + ('textures' if w == 'textures' else 'cutscene videos') for w in dropped), str(e).strip())
        self.log('  ' + msg); self.notes.append(msg)

    def wanted(self, name):
        """Is this optional step still asked for (skip_extras may have unticked it after the plan was made)?"""
        if name == 'videos': return bool(self.opts['videos'])
        return name.split('-')[-1] in self.opts['textures'] if name.startswith('textures-') else True

    def get_build_tools(self):
        install_build_tools(self.work, self.log, lambda: self.cancelled)

    def ensure_toolkit(self):
        tk = self.work / 'toolkit'
        if not (tk / 'tools').is_dir():
            if tk.exists(): shutil.rmtree(tk)
            shutil.copytree(KIT / 'toolkit' / 'tools', tk / 'tools')
            shutil.copytree(KIT / 'toolkit' / 'templates', tk / 'templates')
        return tk

    def disc(self):
        res = precheck.validate_image(self.xiso, RELEASE)
        if not res.ok:
            raise Stop(res.message, 'disc')
        self.log('  %s: %s' % (self.xiso.name, res.message))
        self.log('  ma2.xbe matches the supported MechAssault 2 release.')

    def copy_folder(self, src, dst):
        """Copy a folder made from the disc (instead of unpacking an image), with a progress line every 200 MB."""
        self.log('> copy the game files from the folder %s' % src)
        files = [(p, p.relative_to(src)) for p in sorted(Path(src).rglob('*')) if p.is_file()]
        total = sum(p.stat().st_size for p, _ in files); done = 0; next_report = 200 * 1024 * 1024
        for p, rel in files:
            if self.cancelled: raise Stop('Cancelled.', 'cancelled')
            target = Path(dst) / rel; target.parent.mkdir(parents=True, exist_ok=True)
            try:
                shutil.copyfile(p, target)
            except OSError as e:
                raise Stop('Could not copy %s from the folder (%s). Is it a complete copy of the disc?' % (rel, e.strerror or e), 'disc')
            done += p.stat().st_size
            if done >= next_report:
                self.log('  copied %.1f of %.1f GB' % (done / GB, total / GB)); next_report += 200 * 1024 * 1024
        self.log('  copied %d files (%.1f GB)' % (len(files), total / GB))

    def verify(self):
        self.log('  game_files/ma2.xbe matches the supported release; no disc image needed.')
        self.verify_disc_files()

    def verify_disc_files(self, manifest=None):
        """Every file of the disc must be in game_files with the right size and SHA-256 (installer/data/disc-manifest.json, made from the
        reference image). Sizes first, then hashes with a progress line every 200 MB. A damaged dump stops here, with the files named,
        instead of failing later in the build as "something unexpected"."""
        if manifest is None:
            mf = HERE / 'data' / 'disc-manifest.json'
            if not mf.is_file():
                self.log("  (this installer has no list of the disc's files; only ma2.xbe was checked)"); return
            manifest = json.loads(mf.read_text(encoding='utf-8'))
        root = self.dest / 'game_files'
        bad, todo = [], []
        for rel, (size, digest) in sorted(manifest.items()):
            f = root / rel
            try:
                if not f.is_file() or f.stat().st_size != size: bad.append(rel)
                else: todo.append((rel, size, digest))
            except OSError:
                bad.append(rel)
        total = sum(s for _, s, _ in todo); done = 0; next_report = 200 * 1024 * 1024
        self.log('  checking %d disc files (%.1f GB)' % (len(todo), total / GB))
        for rel, size, digest in todo:
            if self.cancelled: raise Stop('Cancelled.', 'cancelled')
            h = hashlib.sha256()
            try:
                with open(root / rel, 'rb') as fh:
                    for block in iter(lambda: fh.read(1 << 20), b''):
                        h.update(block); done += len(block)
                        if done >= next_report:
                            self.log('  checked %.1f of %.1f GB' % (done / GB, total / GB)); next_report += 200 * 1024 * 1024
            except OSError:
                bad.append(rel); continue
            if h.hexdigest() != digest: bad.append(rel)
        if bad:
            self.log('  damaged or missing: %s' % ', '.join(bad[:50]))
            raise Stop('These files from your disc image are damaged: %s. Dump the disc again (clean it first) and choose the new image.'
                       % (', '.join(bad[:5]) + (' (and %d more)' % (len(bad) - 5) if len(bad) > 5 else '')), 'disc')
        self.log('  all %d disc files match the supported disc.' % len(manifest))

    def extract(self):
        tk = self.ensure_toolkit()
        out = self.dest / 'game_files'
        if self.xiso.is_dir():
            if self.xiso.resolve() != out.resolve():
                if out.exists(): shutil.rmtree(out)
                self.copy_folder(self.xiso, out)
        else:
            if out.exists(): shutil.rmtree(out)
            self.run_cmd([sys.executable, '-m', 'tools.xiso', 'unpack', self.xiso, '-o', out], cwd=tk,
                         env=self.toolkit_env(), title='unpack the disc image')
        if sha256(out / RELEASE['xbe']) != RELEASE['xbe_sha256']: raise Stop('Extracted ma2.xbe does not match.', 'disc')
        self.verify_disc_files()

    def project(self):
        return self.work / 'toolkit' / PROJECT

    def analyze(self):
        tk = self.ensure_toolkit()
        proj = self.project()
        (proj / 'game_files').mkdir(parents=True, exist_ok=True)
        xbe = proj / 'game_files' / RELEASE['xbe']
        shutil.copy2(self.dest / 'game_files' / RELEASE['xbe'], xbe)
        env = self.toolkit_env()
        self.run_cmd([sys.executable, '-m', 'tools.xbe_parser', xbe, '--json', proj / 'game_files' / 'ma2_analysis.json'],
                     cwd=tk, env=env, title='xbe_parser', quiet_pattern=r'^\s+#\d+\s')
        self.run_cmd([sys.executable, '-m', 'tools.disasm', xbe], cwd=tk, env=env, title='disasm (about a minute)')
        self.run_cmd([sys.executable, '-m', 'tools.func_id', xbe], cwd=tk, env=env, title='func_id')
        self.run_cmd([sys.executable, '-m', 'tools.abi_analysis', xbe], cwd=tk, env=env, title='abi_analysis')

    def recompile(self):
        tk = self.ensure_toolkit()
        gen = self.project() / 'src' / 'recomp' / 'gen'
        if gen.exists(): shutil.rmtree(gen)
        self.run_cmd([sys.executable, '-m', 'tools.recomp', self.project() / 'game_files' / RELEASE['xbe'], '--all',
                      '--split', '250', '--game-name', 'MechAssault2', '--gen-dir', gen], cwd=tk, env=self.toolkit_env(),
                     title='recomp (a few minutes)', quiet_pattern=r'^\s+\d+\s+\w+\s+e\.g\.')

    def generate(self):
        bring = self.project() / 'bringup'
        if bring.exists(): shutil.rmtree(bring)
        shutil.copytree(KIT / 'source' / 'game' / 'bringup', bring)
        (bring / 'originals').mkdir(); (bring / 'regression').mkdir()
        gen = self.project() / 'src' / 'recomp' / 'gen'
        for f in gen.glob('recomp_bringup*'): f.unlink()
        for f in gen.glob('recomp_fastpaths.c'): f.unlink()
        env = self.toolkit_env()
        self.run_cmd([sys.executable, bring / 'recover_callbacks.py'], cwd=self.work / 'toolkit', env=env, title='recover_callbacks')
        self.run_cmd([sys.executable, bring / 'optimize_leafs.py'], cwd=self.work / 'toolkit', env=env, title='optimize_leafs')

    def patch(self):
        sys.path.insert(0, str(HERE))
        import gen_patch
        gen = self.project() / 'src' / 'recomp' / 'gen'
        if self.state.get('patch_started') and gen_patch.base_mismatches(gen, KIT / 'patches'):
            self.log('  An earlier run stopped while applying the PC fixes, so the generated code is part-patched. '
                     'Translating the game program again first (a few minutes).')
            self.recompile(); self.generate()
        self.state['patch_started'] = True; self.save()
        try:
            gen_patch.apply(gen, KIT / 'patches', log=lambda s: self.log('  ' + s))
        except (SystemExit, ValueError) as e:
            raise Stop(str(e))
        self.fastcode(gen)
        self.state.pop('patch_started', None)

    def fastcode(self, gen):
        """FastCode (2026-10-31): fast copies of the hot recompiled functions, made here from the patched
        code by tools/fastcode.py (the release ships the tool, not the copies). Every file it writes is
        checked against the release's SHA-256."""
        fc = KIT / 'patches' / 'fastcode'
        if not (fc / 'fastcode.json').is_file(): return
        spec = json.loads((fc / 'fastcode.json').read_text(encoding='utf-8'))
        for name in spec['lf']:                    # fastcode.py reads LF text; the toolkit writes CRLF
            p = gen / name
            p.write_bytes(p.read_bytes().replace(b'\r\n', b'\n'))
        self.run_cmd([sys.executable, '-I', fc / 'fastcode.py', gen] + spec['whole'] + ['--hot', fc / spec['hot']]
                     + [a for n in spec.get('force', []) for a in ('--force', n)],
                     cwd=gen, title='fastcode', quiet_pattern=r'^\d{4} fast=')
        if spec.get('post'):                       # the release's hand edits made after FastCode (strict diffs)
            import gen_patch
            try:
                gen_patch.apply(gen, fc / spec['post'], log=lambda s: self.log('  post-FastCode: ' + s))
            except (SystemExit, ValueError) as e:
                raise Stop(str(e))
        wrong =[f for f, h in spec['final'].items() if not (gen / f).is_file() or sha256(gen / f) != h]
        if wrong:
            raise Stop('The FastCode files do not match the release (%s).' % ', '.join(wrong[:5]))
        self.log('  FastCode copies written: %d files, all verified.' % len(spec['final']))

    def ffmpeg_dir(self):
        """The FFmpeg build for cutscene playback and the video Enhancer. The installer does not carry it: it is downloaded
        on this PC into the work folder and every file is checked against data/ffmpeg.json first (ffmpeg_fetch.py).
        A kit that carries a copy whose files all check out (a maintainer's) uses that copy."""
        if getattr(self, '_ffmpeg', None): return self._ffmpeg
        kit_copy = KIT / 'source' / 'game' / 'vendor' / FFMPEG
        if kit_copy.is_dir() and not ffmpeg_fetch.verify(kit_copy):
            self._ffmpeg = kit_copy; return kit_copy
        exe = os.environ.get('MA2_INSTALLER_EXE')
        look = [self.dest] + ([Path(exe).parent] if exe else [])     # a zip downloaded on another PC
        try:
            self._ffmpeg = ffmpeg_fetch.ensure(self.work / 'deps' / FFMPEG, self.log, lambda: self.cancelled, look)
        except ffmpeg_fetch.Cancelled:
            raise Stop('Cancelled.', 'cancelled')
        except ffmpeg_fetch.MismatchError as e:
            raise Stop(str(e), 'ffmpeg_mismatch')
        except ffmpeg_fetch.FetchError as e:
            raise Stop(str(e), 'network')
        return self._ffmpeg

    def build_root(self):
        """Where the C++ build runs: the work folder, or -- when its path is not plain (see plain_path) -- a folder with a
        plain path on the same drive. MSVC's linker reads its response file in the ANSI code page: a build under C:/Users/José
        stopped at the link, unable to open 'JosÃ©/.../avcodec.lib'; and link.exe splits options at commas: a disc-dump folder
        'MechAssault 2 - Lone Wolf (World) (En,Ja,Fr)' stopped it with LNK1117 (syntax error in option 'MANIFESTINPUT:...').
        The same folder on every run (an interrupted install resumes); removed with the work folder."""
        if plain_path(self.work): return self.work
        tag = BUILD_PREFIX + hashlib.sha256(str(self.work).encode('utf-8')).hexdigest()[:8]
        for base in build_bases(self.work.anchor):
            root = base / tag
            if not plain_path(root): continue
            try:
                root.mkdir(parents=True, exist_ok=True)
                return root
            except OSError:
                continue
        raise Stop('No folder with a plain path (letters, digits, spaces, - _ .) could be created for the C++ build. Install to a '
                   'folder such as C:\\Games\\MechAssault 2.')

    def build(self):
        ffmpeg = self.ffmpeg_dir()                 # first: a download problem shows before the long compile
        if not hasattr(self, 'vcvars'): self.vcvars, self.cmake, self.ninja, self.vs = find_visual_studio(self.vs_choice)
        root = self.build_root()
        if root != self.work:
            self.log('  building in %s (the install path has characters the C++ tools cannot take)' % root)
            (root / 'owner.txt').write_text(str(self.dest), encoding='utf-8')            # which install this folder belongs to (stale ones are cleaned up)
            self.state['build_root'] = str(root); self.save()
        src = root / 'src'
        if src.exists(): shutil.rmtree(src)
        self.log('  copying the source tree')
        shutil.copytree(KIT / 'source' / 'runtime', src / 'runtime')
        shutil.copytree(KIT / 'source' / 'game', src / 'game', ignore=shutil.ignore_patterns('bringup'))
        if not (src / 'game' / 'vendor' / FFMPEG).is_dir(): shutil.copytree(ffmpeg, src / 'game' / 'vendor' / FFMPEG)
        gen = src / 'game' / 'src' / 'recomp' / 'gen'
        if gen.exists(): shutil.rmtree(gen)       # (an empty folder: the packed kit has none)
        shutil.copytree(self.project() / 'src' / 'recomp' / 'gen', gen)
        build = root / 'build'
        if build.exists(): shutil.rmtree(build)
        cpus = os.cpu_count() or 4
        total, avail = precheck.memory_info()
        jobs = self.jobs or precheck.build_jobs(cpus, avail, total)
        if avail < 6 * GB: self.log('  only %.1f GB of memory is free; close other programs if the build fails' % (avail / GB))
        self.log('  %d parallel compiles (%d CPU threads, %.0f GB RAM); usually 5-40 minutes' % (jobs, cpus, total / GB))
        script = root / 'build.cmd'
        script.write_text('\r\n'.join([
            '@echo off',
            'chcp 65001 >nul',                     # the script is UTF-8: paths under an accented user name
            'call "%s" >nul || exit /b 1' % self.vcvars,
            '"%s" -S "%s" -B "%s" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DXBOXRECOMP_DIR=%s" "-DCMAKE_MAKE_PROGRAM=%s" || exit /b 1'
            % (self.cmake, src / 'game', build, src / 'runtime', self.ninja.as_posix()),
            '"%s" -C "%s" -j %d mechassault2_recomp || exit /b 1' % (self.ninja, build, jobs), '']), encoding='utf-8')
        env = dict(os.environ)                     # vcvars64.bat looks for vswhere.exe on PATH
        env['PATH'] = str(Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) /
                          'Microsoft Visual Studio' / 'Installer') + os.pathsep + env.get('PATH', '')
        for v in BUILD_ENV_DROP: env.pop(v, None)
        self.run_cmd(['cmd.exe', '/d', '/c', script], cwd=root, env=env, title='CMake + Ninja')
        if not (build / 'mechassault2_recomp.exe').is_file():
            raise Stop('The build finished, but the game program it made (%s) is gone: antivirus software most likely removed '
                       '(quarantined) it.' % (build / 'mechassault2_recomp.exe'), 'access_denied')

    def assets(self):
        sys.path.insert(0, str(HERE))
        import pc_assets
        out = self.dest / 'pc_assets'
        if self.state.get('update'): self.keep_previous(out)
        if out.exists(): shutil.rmtree(out)
        try:
            pc_assets.build(self.dest, log=lambda s: self.log('  ' + s))
        except (SystemExit, ValueError) as e:
            raise Stop('%s This usually means the disc image is damaged.' % e, 'disc')
        for n in ('shader-seed.pack', 'shader-levels.txt'):          # optional (D-7): without them the game compiles its shaders as it plays
            src = KIT / 'assets' / n
            if src.is_file(): shutil.copy2(src, out / n)
            else: self.log('  (%s is not part of this installer; the game builds its shaders as it plays)' % n)

    def install(self):
        if not hasattr(self, 'vs'): self.vcvars, self.cmake, self.ninja, self.vs = find_visual_studio(self.vs_choice)
        build = self.build_root() / 'build'
        exe = self.dest / 'MechAssault 2.exe'
        self.keep_previous(exe)
        tmp = self.dest / 'MechAssault 2.exe.new'
        self.copy_in(build / 'mechassault2_recomp.exe', tmp)
        retry_io(lambda: tmp.replace(exe), 'replace MechAssault 2.exe', self.log, owned=exe)
        ff = self.ffmpeg_dir() / 'bin'
        for n in FFMPEG_DLLS:
            self.keep_previous(self.dest / n); self.copy_in(ff / n, self.dest / n)
        crt = sorted((self.vs / 'VC' / 'Redist' / 'MSVC').glob('*/x64/Microsoft.VC*.CRT'))
        if crt:
            for dll in crt[-1].glob('*.dll'):
                self.keep_previous(self.dest / dll.name); self.copy_in(dll, self.dest / dll.name)
            self.log('  MSVC runtime from %s' % crt[-1])
        else:
            self.log('  (no MSVC redistributable folder found; the Visual C++ runtime installed on this PC will be used)')
        lic = self.dest / 'Licenses'
        had_vulkan = (lic / 'Enhancer' / VULKAN_NOTICE).is_file()      # textures made through Vulkan earlier: their notice stays
        if lic.exists(): shutil.rmtree(lic)
        shutil.copytree(KIT / 'defaults' / 'Licenses', lic)
        have_tex = any((self.dest / 'game_files' / 'texture_packs' / s / 'index.json').is_file() for s in ('2x', '4x'))
        have_vid = (self.dest / 'game_files' / 'Movie' / 'Enhanced' / '2x').is_dir()
        if have_tex or have_vid: self.install_enhancer_licenses(videos=have_vid, vulkan=had_vulkan and have_tex)
        ffreadme = self.ffmpeg_dir() / 'README.txt'     # the build's own notice: version, configuration, where the source is
        if ffreadme.is_file(): shutil.copy2(ffreadme, lic / 'FFmpeg-README.txt')
        else: self.log('  (the FFmpeg README is missing from the downloaded FFmpeg build)')
        self.install_docs()
        self.install_defaults_ini()
        for f in sorted((KIT / 'editor').glob('*.exe')) if (KIT / 'editor').is_dir() else []:
            self.keep_previous(self.dest / f.name)
            self.copy_in(f, self.dest / f.name)        # MechAssault 2 Scenario Editor
            self.log('  %s' % f.name)
        self.make_shortcuts()

    def copy_in(self, src, dst):
        """Copy one installer-owned file into the install folder, retrying while a scanner holds it (retry_io)."""
        retry_io(lambda: shutil.copy2(src, dst), 'copy %s' % Path(dst).name, self.log, owned=dst)

    def install_docs(self):
        """README.txt, Controls.txt and the Docs folder, with the {{VERSION}}, {{BUILD}} and {{MA2_SUPPORT_URL}} tokens filled in.
        The old PC_CONTROLS.md of earlier versions is removed (Controls.txt replaces it)."""
        src = KIT / 'defaults'
        for n in ('Controls.txt', 'README.txt'):
            if (src / n).is_file(): copy_doc(src / n, self.dest / n)
            else: self.log('  (%s is missing from this installer)' % n)
        old = self.dest / 'PC_CONTROLS.md'
        if (self.dest / 'Controls.txt').is_file() and old.is_file():
            try: old.unlink()
            except OSError: pass
        if (src / 'Docs').is_dir():
            for f in sorted((src / 'Docs').rglob('*')):
                if f.is_file(): copy_doc(f, self.dest / 'Docs' / f.relative_to(src / 'Docs'))

    def install_defaults_ini(self):
        """pc_settings.ini in the install folder holds the first-launch defaults (the game copies it to %LOCALAPPDATA% once).
        On an update, an ini the user changed is kept and the new defaults go beside it as pc_settings.default.ini."""
        src, dst = KIT / 'defaults' / 'pc_settings.ini', self.dest / 'pc_settings.ini'
        self.ini_ours = True
        if self.state.get('update') and dst.is_file():
            stamp = self.old_stamp or {}
            if stamp.get('defaults_sha256') != sha256(dst):
                shutil.copy2(src, self.dest / 'pc_settings.default.ini'); self.ini_ours = False
                self.notes.append('You had changed pc_settings.ini in the install folder, so it was kept. The new defaults are in '
                                  'pc_settings.default.ini beside it.')
                self.log('  pc_settings.ini was edited: kept; new defaults written as pc_settings.default.ini')
                return
        shutil.copy2(src, dst)

    def make_shortcuts(self):
        made = []
        names = {'game': ('MechAssault 2 (Recomp).lnk', self.dest / 'MechAssault 2.exe', 'MechAssault 2 (Recomp)')}
        editor = next(iter(sorted(self.dest.glob('MA2 Scenario Editor*.exe'))), None)
        want = set(self.shortcuts)
        places = [w for w in ('desktop', 'start') if w in want]
        for where in places:
            try:
                base = known_folder(where)
                create_shortcut(base / names['game'][0], names['game'][1], self.dest, names['game'][2])
                made.append(str(base / names['game'][0]))
                if 'editor' in want and editor:
                    create_shortcut(base / 'MA2 Scenario Editor.lnk', editor, self.dest, 'MechAssault 2 Scenario Editor')
                    made.append(str(base / 'MA2 Scenario Editor.lnk'))
            except OSError as e:
                self.log('  (shortcut on the %s not created: %s)' % (where, e))
        if 'editor' in want and editor and not places:
            try:
                base = known_folder('start')
                create_shortcut(base / 'MA2 Scenario Editor.lnk', editor, self.dest, 'MechAssault 2 Scenario Editor')
                made.append(str(base / 'MA2 Scenario Editor.lnk'))
            except OSError as e:
                self.log('  (editor shortcut not created: %s)' % e)
        for m in made: self.log('  shortcut: %s' % m)
        self.state['shortcuts'] = made; self.save()

    # -- optional content (the Enhancer)
    def enhancer(self, engine='pytorch'):
        """The Enhancer in the work folder, set up for `engine`: 'pytorch' (PyTorch with CUDA and the .pth models; also what the
        videos need) or 'vulkan' (textures through realesrgan-ncnn-vulkan: only NumPy, Pillow and DirectXTex)."""
        enh = self.work / 'enhancer'
        if not (enh / 'setup.py').is_file():
            if enh.exists(): shutil.rmtree(enh)
            shutil.copytree(KIT / 'enhancer', enh)
        if not hasattr(self, 'epy'): self.epy = enhancer_python()
        venv = enh / '.venv' / 'Scripts' / 'python.exe'
        if repair_venv_home(enh / '.venv' / 'pyvenv.cfg', Path(self.epy).parent): self.log('  the Enhancer environment pointed at a Python that is gone; repaired')
        setup = self.state.get('enhancer_setup')         # True: the full (PyTorch) setup; 'vulkan': the Vulkan textures setup
        if engine == 'vulkan':
            if not setup or not venv.is_file():
                self.log('  Enhancer setup for the Vulkan upscaler: NumPy, Pillow and DirectXTex (no PyTorch)')
                self.run_cmd([self.epy, enh / 'setup.py', '--engine', 'vulkan'], cwd=enh, title='Enhancer setup', hint=ENHANCER_HINT,
                             quiet_pattern=r'^\s*\S*\s*\d+(\.\d+)? ?[kMG]?B/s|━')
                self.state['enhancer_setup'] = 'vulkan'; self.save()
            return enh, venv
        if setup is not True or not venv.is_file():
            self.log('  Enhancer setup: PyTorch with CUDA, Real-ESRGAN models and DirectXTex (several GB of downloads)')
            self.run_cmd([self.epy, enh / 'setup.py'], cwd=enh, title='Enhancer setup', hint=ENHANCER_HINT, quiet_pattern=r'^\s*\S*\s*\d+(\.\d+)? ?[kMG]?B/s|━')
            self.state['enhancer_setup'] = True; self.save()
        return enh, venv

    def vulkan_dir(self):
        """realesrgan-ncnn-vulkan for the Vulkan textures: downloaded into the work folder, archive and files checked (upscaler_fetch.py)."""
        exe = os.environ.get('MA2_INSTALLER_EXE')
        look = [self.dest] + ([Path(exe).parent] if exe else [])     # the zip downloaded on another PC
        try:
            return upscaler_fetch.ensure(self.work / 'deps' / upscaler_fetch.SPEC['folder'], self.log, lambda: self.cancelled, look)
        except upscaler_fetch.Cancelled:
            raise Stop('Cancelled.', 'cancelled')
        except upscaler_fetch.FetchError as e:
            raise Stop(str(e), 'network')

    def install_enhancer_licenses(self, videos=False, vulkan=False):
        """The AI tools behind the enhanced content have licences that must travel with their output: Licenses/Enhancer/*.txt
        (enhancer/LICENSES), for the videos FMV-NOTICE.txt (enhancer/FMV/NOTICE.txt), and for textures made through Vulkan
        realesrgan-ncnn-vulkan.txt (the tool's MIT licence and the ncnn notice)."""
        dst = self.dest / 'Licenses' / 'Enhancer'
        try:
            dst.mkdir(parents=True, exist_ok=True)
            for f in sorted((KIT / 'enhancer' / 'LICENSES').glob('*')):
                if f.name == VULKAN_NOTICE and not (vulkan or (dst / f.name).is_file()): continue
                if f.is_file(): shutil.copy2(f, dst / f.name)
            notice = KIT / 'enhancer' / 'FMV' / 'NOTICE.txt'
            if videos and notice.is_file(): shutil.copy2(notice, dst / 'FMV-NOTICE.txt')
        except OSError as e:
            self.log('  (could not copy the enhancer licences: %s)' % e)

    def textures(self, scale):
        if (self.dest / 'game_files' / 'texture_packs' / scale / 'index.json').is_file():
            self.log('  the %s texture pack is already installed' % scale); return
        engine = getattr(self, 'tex_engine', None) or 'pytorch'
        if engine == 'pytorch':
            try:
                enh, py = self.enhancer()
            except Stop as e:                            # PyTorch cannot use this card (setup.py says so): Vulkan instead
                if e.kind != 'gpu' or os.environ.get('MA2_TEXTURE_ENGINE'): raise
                self.log('  PyTorch cannot use this graphics card; the textures are made through Vulkan instead (%s)' % VULKAN_NOTE)
                engine = self.tex_engine = 'vulkan'
        if engine == 'vulkan':
            enh, py = self.enhancer('vulkan')
            vulkan = self.vulkan_dir()
        env = dict(os.environ); env['PYTHONIOENCODING'] = 'utf-8'; env['ENHANCER_NO_ZIP'] = '1'
        if not self.state.get('textures_extracted'):
            self.run_cmd([py, enh / 'extract_textures.py', '--all', '--game-dir', self.dest / 'game_files'], cwd=enh, env=env, hint=ENHANCER_HINT,
                         title='extract the original textures from the game archives')
            self.state['textures_extracted'] = True; self.save()
        self.log('  %s textures: every texture through Real-ESRGAN%s, then DDS packing (this takes hours; it resumes if interrupted)'
                 % (scale, ' (Vulkan)' if engine == 'vulkan' else ''))
        args = [py, enh / 'enhance.py', '--scale', scale[0], '--no-preview']
        if engine == 'vulkan': args += ['--engine', 'vulkan', '--vulkan', vulkan]
        self.run_cmd(args, cwd=enh, env=env, hint=ENHANCER_HINT, title='enhance %s' % scale)
        pack = enh / 'packs' / scale
        if not (pack / 'index.json').is_file(): raise Stop('The %s texture pack was not completed.' % scale)
        target = self.dest / 'game_files' / 'texture_packs' / scale
        if target.exists(): shutil.rmtree(target)
        target.mkdir(parents=True)
        shutil.move(str(pack / 'textures'), str(target / 'textures'))
        shutil.copy2(pack / 'index.json', target / 'index.json')
        self.install_enhancer_licenses(videos=(self.dest / 'game_files' / 'Movie' / 'Enhanced' / '2x').is_dir(), vulkan=(engine == 'vulkan'))
        self.log('  installed %s' % target)

    def videos(self):
        movie = self.dest / 'game_files' / 'Movie'
        enhanced = movie / 'Enhanced' / '2x'
        sources = [p for p in movie.rglob('*.bik') if enhanced not in p.parents]
        if enhanced.is_dir() and len(list(enhanced.rglob('*.mkv'))) >= len(sources):
            self.log('  the enhanced cutscene videos are already installed'); return
        engine = getattr(self, 'vid_engine', None) or 'pytorch'
        if engine == 'pytorch':
            try:
                enh, py = self.enhancer()                # PyTorch (also after a Vulkan textures setup)
            except Stop as e:                            # PyTorch cannot use this card (setup.py says so): Vulkan instead
                if e.kind != 'gpu' or os.environ.get('MA2_VIDEO_ENGINE'): raise
                self.log('  PyTorch cannot use this graphics card; the cutscenes are made through Vulkan instead (%s)' % VIDEO_VULKAN_NOTE)
                engine = self.vid_engine = 'vulkan'
        if engine == 'vulkan':
            enh, py = self.enhancer('vulkan')            # NumPy and Pillow only: no PyTorch, no RealBasicVSR model
            vulkan = self.vulkan_dir()                   # the same checked realesrgan-ncnn-vulkan as the textures
        fmv = enh / 'FMV'
        ff = self.ffmpeg_dir() / 'bin'
        config = json.loads((fmv / 'config.json').read_text(encoding='utf-8'))
        config.update(source=str(movie / '60' / 'English' / 'SPIntro.bik'), ffmpeg=str(ff / 'ffmpeg.exe'), ffprobe=str(ff / 'ffprobe.exe'))
        (fmv / 'config.json').write_text(json.dumps(config, indent=2) + '\n', encoding='utf-8')
        env = dict(os.environ); env['PYTHONIOENCODING'] = 'utf-8'
        if engine == 'pytorch' and not self.state.get('fmv_setup'):
            self.run_cmd([py, '-B', fmv / 'setup_fmv.py'], cwd=fmv, env=env, title='FMV setup (RealBasicVSR model)', hint=ENHANCER_HINT)
            self.state['fmv_setup'] = True; self.save()
        self.log('  2x videos: 8 distinct video streams, about 14,700 frames through %s, then encoding (many hours; resumes)'
                 % ('Real-ESRGAN (Vulkan, frame by frame)' if engine == 'vulkan' else 'RealBasicVSR'))
        args = [py, '-B', fmv / 'enhance_all_fmvs.py', '--movie-root', movie, '--output-root', movie / 'Enhanced' / '2x',
                '--metadata-root', self.work / 'fmv-metadata', '--no-zip']
        if engine == 'vulkan': args += ['--engine', 'vulkan', '--vulkan', vulkan]
        self.run_cmd(args, cwd=fmv, env=env, title='enhance all cutscenes 2x', hint=ENHANCER_HINT)
        self.install_enhancer_licenses(videos=True, vulkan=(engine == 'vulkan'))

    def finish(self):
        self.settle()
        try:
            self.log('  install log: %s' % self.session_log)
        except Exception:
            pass
        try:
            root = self.build_root()
        except Stop:
            root = self.work
        # The install is complete: a later run on this folder is an update or add-on run, not a resume. The state file is renamed FIRST, so a
        # work folder that cannot be removed (a scanner holds a file) is never offered as an interrupted install.
        state_file = self.work / 'state.json'
        if state_file.is_file(): state_file.replace(self.work / 'state.finished.json')
        if self.keep_work:
            self.log('  work folder kept: %s' % self.work)
            if root != self.work: self.log('  build folder kept: %s' % root)
            return
        self.logfile.close()
        self.remove_work_folders([self.work] if root == self.work else [self.work, root])

    def remove_work_folders(self, folders):
        left = []
        for folder in folders:
            for attempt in range(5):
                try:
                    shutil.rmtree(folder); break
                except OSError:
                    time.sleep(2)
            if Path(folder).exists(): left.append(folder)
        if left:
            for f in left:
                self.log('  could not remove %s' % f)
                self.notes.append('Could not remove %s; you can delete it.' % f)
        else:
            self.log('  work folder removed')

    def update_live_settings(self, q, v):
        r"""The game reads %LOCALAPPDATA%\MechAssault2\pc_settings.ini. Add the keys this version introduced (never touching an
        existing value, backup first) and, when asked, switch on the textures/videos just added."""
        live = precheck.local_dir() / 'pc_settings.ini'
        if not live.is_file(): return
        text = live.read_text(encoding='utf-8', errors='replace')
        new, added = text, []
        if self.merge_settings and self.state.get('update'):
            new, added = precheck.merge_ini((KIT / 'defaults' / 'pc_settings.ini').read_text(encoding='utf-8'), text)
        switched = []
        if self.switch_textures:
            if q and any(s.startswith('textures-') for s in self.state.get('done', [])):
                new = precheck.set_ini_value(new, 'TextureQuality', q); switched.append('TextureQuality=%d' % q)
            if v and 'videos' in self.state.get('done', []):
                new = precheck.set_ini_value(new, 'FMVQuality', v); switched.append('FMVQuality=%d' % v)
        if new != text:
            bak = live.with_name('pc_settings.ini.bak-%s' % time.strftime('%Y%m%d'))
            if bak.exists(): bak = live.with_name('pc_settings.ini.bak-%s' % time.strftime('%Y%m%d-%H%M%S'))
            shutil.copy2(live, bak)
            live.write_text(new, encoding='utf-8')
            if added:
                self.log('  added %d new setting(s) to %s: %s (backup: %s)' % (len(added), live, ', '.join(added), bak.name))
                self.notes.append('%d new setting(s) were added to your settings file (backup: %s).' % (len(added), bak.name))
            if switched:
                self.log('  switched on: %s' % ', '.join(switched))

    def write_stamp(self, ini):
        old = precheck.read_stamp(self.dest) if self.game_installed() else None      # add-ons, or the game was made final earlier (finalize_core)
        stamp = dict(old or {})
        if not old:
            editor = next(iter(sorted(self.dest.glob('MA2 Scenario Editor*.exe'))), None)
            exe = self.dest / 'MechAssault 2.exe'
            stamp.update(kit_id=KIT_ID, version=VERSION, public_version=PUBLIC_VERSION,
                         release_sha256=RELEASE['release_executable_sha256'],        # the exe the release was made with (provenance)
                         built_sha256=sha256(exe) if exe.is_file() else '',          # the exe this install actually holds
                         editor=sha256(editor)[:8] if editor else '', installed=time.strftime('%Y-%m-%dT%H:%MZ', time.gmtime()))
        if getattr(self, 'ini_ours', None) and ini.is_file(): stamp['defaults_sha256'] = sha256(ini)
        elif self.old_stamp and 'defaults_sha256' in self.old_stamp and getattr(self, 'ini_ours', True) is False:
            stamp['defaults_sha256'] = self.old_stamp['defaults_sha256']
        stamp['options'] = {'textures': [s for s in ('2x', '4x') if (self.dest / 'game_files/texture_packs' / s / 'index.json').is_file()],
                            'videos': (self.dest / 'game_files/Movie/Enhanced/2x').is_dir()}
        if self.state.get('shortcuts'): stamp['shortcuts'] = self.state['shortcuts']
        try:
            (self.dest / precheck.STAMP).write_text(json.dumps(stamp, indent=1), encoding='utf-8')
        except OSError as e:
            self.log('  (could not write %s: %s)' % (precheck.STAMP, e))

def launch_advice(path, e):
    """Plain words for a failed start of the game or the editor (an OSError from launching it), with its Windows code."""
    code = getattr(e, 'winerror', None) or getattr(e, 'errno', None)
    name = Path(str(path)).name
    if code == 4551:
        why = ('Windows Smart App Control blocked it, because the game is built on your PC and is not signed. Smart App Control can only be '
               'turned off (Windows Security > App & browser control); the installer never changes it.')
    elif code in (225, 2):
        why = ('the file was quarantined or removed, most likely by antivirus software. Restore it from the antivirus quarantine and allow the '
               'install folder, or run the installer again to repair the install.')
    elif code == 5:
        why = 'Windows refused access (antivirus software or folder protection). Allow the install folder in it, then try again.'
    else:
        why = str(getattr(e, 'strerror', None) or e)
    return 'Could not start %s (Windows error %s): %s' % (name, code if code is not None else '?', why)

def render_doc(data):
    """Fill the tokens of a shipped document (bytes in, bytes out; line endings untouched): {{VERSION}} the public version,
    {{BUILD}} the first 8 characters of the kit id, {{MA2_SUPPORT_URL}} the support address (D-11: words pointing at the download page until one is set)."""
    for token, value in (('VERSION', PUBLIC_VERSION), ('BUILD', BUILD), ('MA2_SUPPORT_URL', precheck.support_where())):
        data = data.replace(('{{%s}}' % token).encode('ascii'), value.encode('utf-8'))
    return data

def copy_doc(src, dst, render=True):
    Path(dst).parent.mkdir(parents=True, exist_ok=True)
    data = Path(src).read_bytes()
    Path(dst).write_bytes(render_doc(data) if render and Path(src).suffix.lower() in ('.txt', '.md') else data)

def duration(seconds):
    seconds = int(seconds)
    return '%dh %02dm' % (seconds // 3600, seconds % 3600 // 60) if seconds >= 3600 else \
           '%dm %02ds' % (seconds // 60, seconds % 60) if seconds >= 60 else '%ds' % seconds

def run_uninstall(a):
    """The --uninstall command line: shows what would go, deletes only with --yes. Exit 0 done, 1 problem, 2 not confirmed."""
    try:
        plan = uninstall_plan(a.dest, a.delete_user_data)
    except Stop as e:
        print('UNINSTALL STOPPED: %s' % e, file=sys.stderr); return 1
    print('Uninstall MechAssault 2 Recomp from %s:' % plan['dest'])
    for s in plan['shortcuts']: print('  shortcut  %s' % s)
    print('  the install folder  %s' % plan['dest'])
    for c in plan['caches']: print('  cache  %s' % c)
    if plan['user_data']: print('  your settings, saves and logs  %s' % plan['user_data'])
    print('  (never deleted: your scenarios in %s)' % scenarios_dir())
    if not a.yes:
        print('\nNothing was deleted. Add --yes to uninstall.'); return 2
    res = uninstall(a.dest, a.delete_user_data, log=lambda line: print(line, flush=True))
    return 1 if res['failed'] else 0

def main():
    ap = argparse.ArgumentParser(prog='"MechAssault 2 Recomp Installer.exe"', description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--xiso', help='MechAssault 2 disc image (.xiso/.iso)')
    ap.add_argument('--dest', help='install folder (empty, or an existing install to add options); optional with --diagnostics')
    ap.add_argument('--textures', default='', help='enhanced texture packs to generate: 2x, 4x or 2x,4x')
    ap.add_argument('--videos', action='store_true', help='generate the 2x enhanced cutscene videos')
    ap.add_argument('--jobs', type=int, help='parallel compiles (default: from CPU threads and RAM)')
    ap.add_argument('--keep-work', action='store_true', help='keep the _work folder')
    ap.add_argument('--allow-cpu', action='store_true', help='allow enhancement without an NVIDIA GPU (extremely slow)')
    ap.add_argument('--upscaler', metavar='ZIP|FOLDER|URL', help='the Vulkan texture upscaler, if this PC has no internet: the file %s '
                    '(download exactly %s on another PC), a folder with it extracted, or a URL' % (upscaler_fetch.SPEC['archive'], upscaler_fetch.SPEC['urls'][0]))
    ap.add_argument('--install-build-tools', action='store_true',
                    help='if no C++ compiler is installed, download and install Microsoft\'s free C++ Build Tools')
    ap.add_argument('--update', action='store_true', help='update an existing install in place, keeping settings and saves')
    ap.add_argument('--shortcuts', default='', help='create shortcuts: any of desktop, start, editor (comma separated)')
    ap.add_argument('--no-merge-settings', action='store_true', help='on --update, do not add new settings keys to your pc_settings.ini')
    ap.add_argument('--switch-textures', action='store_true', help='switch your settings to the enhanced textures/videos just added')
    ap.add_argument('--low-priority', action='store_true', help='run the compiler at low priority (keeps the PC responsive)')
    ap.add_argument('--ffmpeg', metavar='ZIP|FOLDER|URL', help='FFmpeg for the cutscenes, if this PC has no internet: the file %s '
                    '(download exactly %s on another PC), a folder with it extracted, or a URL of a local mirror' % (ffmpeg_fetch.SPEC['archive'], ffmpeg_fetch.SPEC['urls'][0]))
    ap.add_argument('--uninstall', action='store_true', help='remove the install in --dest: its shortcuts, the folder, the shader and route caches (needs --yes)')
    ap.add_argument('--delete-user-data', action='store_true', help='with --uninstall: also delete your settings and saves (%%LOCALAPPDATA%%\\MechAssault2)')
    ap.add_argument('--diagnostics', action='store_true', help='write MA2-diagnostics-<time>.zip (logs, settings, system facts) to the Desktop for a problem report')
    ap.add_argument('--include-dump', action='store_true', help='with --diagnostics: include the newest crash or hang dump (it can contain folder names and your user name)')
    ap.add_argument('--include-botrec', action='store_true', help='with --diagnostics: include the newest bot match recording')
    ap.add_argument('--yes', action='store_true', help='with --uninstall: do not stop to ask')
    ap.add_argument('--until', help=argparse.SUPPRESS)   # stop after a step, e.g. patch (generated sources only)
    ap.add_argument('--vs', help=argparse.SUPPRESS)      # build with this VS/Build Tools installation folder
    a = ap.parse_args()
    if a.ffmpeg:                                  # a URL is passed as it is; a file or folder as a full path
        os.environ['MA2_FFMPEG_SOURCE'] = (a.ffmpeg.strip() if ffmpeg_fetch.is_url(a.ffmpeg.strip())
                                           else str(Path(precheck.clean_path(a.ffmpeg)).resolve()))
    if a.upscaler:                                # a URL is passed as it is; a file or folder as a full path
        os.environ['MA2_UPSCALER_SOURCE'] = (a.upscaler.strip() if upscaler_fetch.is_url(a.upscaler.strip())
                                             else str(Path(precheck.clean_path(a.upscaler)).resolve()))
    if not a.dest and (a.uninstall or not a.diagnostics): ap.error('the following arguments are required: --dest')
    if a.uninstall:
        sys.exit(run_uninstall(a))
    if a.diagnostics:
        # Works with nothing installed: without --dest it looks at the folder this PC last installed to, else the default folder.
        dest = precheck.clean_path(a.dest) if a.dest else (precheck.remembered_install() or precheck.default_install_folder())
        res = precheck.collect_diagnostics(dest, known_folder('desktop'), PUBLIC_VERSION, BUILD, None, a.include_dump, a.include_botrec)
        print('Diagnostics written to %s' % res['path'])
        for n in res['notes']: print('Note: %s' % n)
        sys.exit(0)
    textures = [t.strip().lower() for t in a.textures.split(',') if t.strip()]
    if any(t not in ('2x', '4x') for t in textures): ap.error('--textures takes 2x, 4x or 2x,4x')
    shortcuts = [s.strip().lower() for s in a.shortcuts.split(',') if s.strip()]
    if any(s not in ('desktop', 'start', 'editor') for s in shortcuts): ap.error('--shortcuts takes desktop, start, editor')
    inst = Installer(a.xiso, a.dest, textures, a.videos, a.jobs, a.keep_work, a.allow_cpu,
                     log=lambda line: print(line, flush=True), install_build_tools=a.install_build_tools, vs=a.vs,
                     update=a.update, shortcuts=shortcuts, merge_settings=not a.no_merge_settings,
                     switch_textures=a.switch_textures, low_priority=a.low_priority)
    inst.until = a.until
    try:
        inst.run()
    except Stop as e:
        if e.kind == 'extras':                     # the game is installed; only optional content failed: exit code 2
            print('\n%s' % e, file=sys.stderr)
            for note in inst.notes: print('Note: %s' % note)
            sys.exit(2)
        print('\nINSTALL STOPPED: %s' % e, file=sys.stderr); sys.exit(1)
    except KeyboardInterrupt:
        inst.cancel(); print('\nCancelled; run again to resume.', file=sys.stderr); sys.exit(1)
    except Exception:
        import traceback; traceback.print_exc()
        print('\nINSTALL STOPPED: unexpected error (above). Running the installer again resumes. The log is %s' % inst.session_log,
              file=sys.stderr); sys.exit(1)
    if not inst.until:
        print('\nMechAssault 2 is installed in %s. Run "MechAssault 2.exe".' % inst.dest)
    for note in inst.notes: print('Note: %s' % note)

if __name__ == '__main__':
    main()
