"""Linux through Wine / Proton: what the installer and its reports need to know, and the C++ toolchain used there.

On Windows nothing here changes anything: wine_version() is None (Windows' ntdll has no wine_get_version) and every
function returns the Windows answer. Under Wine or Proton (Steam Deck, RecompLauncher on Linux, a non-Steam game in
Steam) the installer is the same program; only the compiler differs, because Microsoft's C++ Build Tools do not install
under Wine. The game is then built with the Windows builds of LLVM (clang-cl, lld-link, llvm-rc, llvm-mt) against the
Microsoft C runtime and Windows SDK headers and libraries (fetched with xwin), driven by the same CMake + Ninja build.

The toolchain lives in %LOCALAPPDATA%\\MechAssault2\\Toolchain (inside the Wine prefix), once per prefix:
  llvm\\      clang-cl.exe, lld-link.exe, llvm-rc.exe, llvm-mt.exe, llvm-lib.exe and clang's own headers (lib\\clang\\N)
  xwin\\      crt\\include, crt\\lib\\x86_64, sdk\\include\\{ucrt,um,shared,winrt}, sdk\\lib\\{um,ucrt}\\x86_64
  cmake\\     bin\\cmake.exe and share\\cmake-*
  ninja\\     ninja.exe
toolchain_fetch.py fills it (pinned archives, SHA-256 checked). MA2_WINE_TOOLCHAIN=<folder> uses another folder with the
same layout (tests, players without internet). MA2_TOOLCHAIN=clang forces this toolchain on Windows too (maintainers).
"""
import ctypes, os, re, subprocess
from pathlib import Path

_cache = {}


def wine_version():
    """'9.0', '10.0-…' (Wine's own version string) when this process runs under Wine or Proton, else None. Never raises.
    MA2_TEST_WINE=<version> pretends (tests only)."""
    fake = os.environ.get('MA2_TEST_WINE', '').strip()
    if fake: return None if fake == '0' else fake
    if 'v' in _cache: return _cache['v']
    v = None
    try:
        if os.name == 'nt':
            f = ctypes.WinDLL('ntdll').wine_get_version         # AttributeError on Windows: not exported there
            f.restype = ctypes.c_char_p; f.argtypes = []
            v = (f() or b'').decode('ascii', 'replace') or None
    except Exception:
        v = None
    _cache['v'] = v
    return v


def is_wine():
    return wine_version() is not None


def host_system():
    """('Linux', '6.5.0-…') of the system under Wine (wine_get_host_version), or None. Never raises."""
    if not is_wine(): return None
    if os.environ.get('MA2_TEST_WINE'): return ('Linux', 'test')
    try:
        f = ctypes.WinDLL('ntdll').wine_get_host_version
        sysname, release = ctypes.c_char_p(), ctypes.c_char_p()
        f.argtypes = [ctypes.POINTER(ctypes.c_char_p), ctypes.POINTER(ctypes.c_char_p)]; f.restype = None
        f(ctypes.byref(sysname), ctypes.byref(release))
        return ((sysname.value or b'').decode('ascii', 'replace'), (release.value or b'').decode('ascii', 'replace'))
    except Exception:
        return None


def proton():
    """A short name of the Proton build when one runs this process ('Proton', 'GE-Proton9-20', 'umu'), else ''. From the
    variables Steam, umu-run and Proton set (Wine passes the Linux environment through). Never raises."""
    if not is_wine(): return ''
    env = os.environ
    name = ''
    for k in ('PROTONPATH', 'STEAM_COMPAT_TOOL_PATHS'):
        v = env.get(k, '')
        m = re.search(r'([A-Za-z]*-?Proton[\w.\- ]*)', v.split(':')[0]) if v else None
        if m: name = m.group(1).strip(); break
    if not name and (env.get('STEAM_COMPAT_DATA_PATH') or env.get('SteamAppId') or env.get('SteamGameId')): name = 'Proton'
    if env.get('UMU_ID') or env.get('GAMEID'): name = (name + ' via umu').strip() if name else 'umu'
    return name


def steam_deck():
    """True when Steam says this is a Steam Deck (SteamDeck=1, set by Steam in Game Mode and for games it starts)."""
    return is_wine() and os.environ.get('SteamDeck', '') == '1'


def describe():
    """'Wine 9.0 on Linux 6.6.87' / 'Proton (Wine 9.0) on Linux …, Steam Deck', or '' on Windows. For logs and reports."""
    v = wine_version()
    if not v: return ''
    p = proton()
    s = ('%s (Wine %s)' % (p, v)) if p else 'Wine %s' % v
    h = host_system()
    if h and h[0]: s += ' on %s %s' % (h[0], h[1])
    if steam_deck(): s += ', Steam Deck'
    return s


def host_tag():
    """'wine' under Wine/Proton, else 'windows' (the install stamp's 'host')."""
    return 'wine' if is_wine() else 'windows'


def dos_path(text):
    """A Linux path ('/srv/games/x', what RecompLauncher passes as {install}) as the Windows path Wine maps it to
    ('Z:\\srv\\games\\x'). Anything else, and everything on Windows, comes back unchanged. Never raises."""
    s = text or ''
    if not is_wine() or not s.startswith('/') or s.startswith('//'): return s
    try:
        if not os.environ.get('MA2_TEST_WINE'):
            f = ctypes.WinDLL('kernel32').wine_get_dos_file_name
            f.argtypes = [ctypes.c_char_p]; f.restype = ctypes.c_void_p
            p = f(os.fsencode(s))
            if p:
                try: return ctypes.wstring_at(p)
                finally: ctypes.WinDLL('kernel32').HeapFree(ctypes.WinDLL('kernel32').GetProcessHeap(), 0, ctypes.c_void_p(p))
    except Exception:
        pass
    return 'Z:' + s.replace('/', '\\')                 # Wine's default: Z: is the Linux root


# ---------------------------------------------------------------------------- the clang toolchain

LLVM_TOOLS = ('clang-cl.exe', 'lld-link.exe', 'llvm-rc.exe', 'llvm-mt.exe', 'llvm-lib.exe')


def use_clang():
    """True when the game is built with the LLVM toolchain: under Wine/Proton, or MA2_TOOLCHAIN=clang (maintainers)."""
    return is_wine() or os.environ.get('MA2_TOOLCHAIN', '').strip().lower() == 'clang'


def toolchain_dir():
    o = os.environ.get('MA2_WINE_TOOLCHAIN', '').strip().strip('"')
    if o: return Path(dos_path(o))
    base = os.environ.get('LOCALAPPDATA') or str(Path.home() / 'AppData' / 'Local')
    return Path(base) / 'MechAssault2' / 'Toolchain'


def _first(paths):
    return next((p for p in paths if p.is_file()), None)


def toolchain_missing(root=None):
    """The parts of the toolchain under `root` that are missing (empty: complete): llvm, sdk, cmake, ninja."""
    root = Path(root or toolchain_dir())
    miss = []
    if not all((root / 'llvm' / 'bin' / t).is_file() for t in LLVM_TOOLS) or not list((root / 'llvm' / 'lib' / 'clang').glob('*/include/stddef.h')):
        miss.append('llvm')
    x = root / 'xwin'
    if not ((x / 'crt' / 'include' / 'vcruntime.h').is_file() and (x / 'sdk' / 'include' / 'um' / 'windows.h').is_file()
            and (x / 'sdk' / 'include' / 'ucrt' / 'stdio.h').is_file() and _first([x / 'crt/lib/x86_64/msvcrt.lib', x / 'crt/lib/x86_64/MSVCRT.lib'])
            and _first([x / 'sdk/lib/um/x86_64/kernel32.lib', x / 'sdk/lib/um/x86_64/Kernel32.lib'])
            and _first([x / 'sdk/lib/ucrt/x86_64/ucrt.lib'])):
        miss.append('sdk')
    if not (root / 'cmake' / 'bin' / 'cmake.exe').is_file(): miss.append('cmake')
    if not (root / 'ninja' / 'ninja.exe').is_file(): miss.append('ninja')
    return miss


def env_script(root, out):
    """Write the batch file that stands where vcvars64.bat does: INCLUDE / LIB for clang-cl and lld-link (the Microsoft CRT and SDK
    from xwin), LLVM first on PATH, MA2_LLVM / MA2_XWIN for wine-clang.cmake. For a maintainer's shell: the installer's build gets
    the same values from toolchain_env() (Wine's cmd.exe is not used for the build). Returns `out`."""
    root = Path(root); x = root / 'xwin'
    inc = [x / 'crt/include', x / 'sdk/include/ucrt', x / 'sdk/include/um', x / 'sdk/include/shared', x / 'sdk/include/winrt']
    lib = [x / 'crt/lib/x86_64', x / 'sdk/lib/um/x86_64', x / 'sdk/lib/ucrt/x86_64']
    lines = ['@echo off',
             'set "INCLUDE=%s"' % ';'.join(str(p) for p in inc),
             'set "LIB=%s"' % ';'.join(str(p) for p in lib),
             'set "PATH=%s;%%PATH%%"' % (root / 'llvm' / 'bin'),
             'set "MA2_LLVM=%s"' % (root / 'llvm').as_posix(),
             'set "MA2_XWIN=%s"' % x.as_posix(),
             'set "CC=" & set "CXX=" & set "RC="',
             'exit /b 0', '']
    Path(out).write_text('\r\n'.join(lines), encoding='utf-8')
    return Path(out)


def toolchain_env(root):
    """The environment the build needs (what env_script() sets), for a build started straight from Python: INCLUDE, LIB, MA2_LLVM,
    MA2_XWIN, LLVM first on PATH."""
    root = Path(root); x = root / 'xwin'
    return {'INCLUDE': ';'.join(str(p) for p in (x / 'crt/include', x / 'sdk/include/ucrt', x / 'sdk/include/um', x / 'sdk/include/shared',
                                                 x / 'sdk/include/winrt')),
            'LIB': ';'.join(str(p) for p in (x / 'crt/lib/x86_64', x / 'sdk/lib/um/x86_64', x / 'sdk/lib/ucrt/x86_64')),
            'MA2_LLVM': (root / 'llvm').as_posix(), 'MA2_XWIN': x.as_posix(),
            'PATH': str(root / 'llvm' / 'bin') + os.pathsep + os.environ.get('PATH', '')}


def find_toolchain():
    """(env .cmd, cmake.exe, ninja.exe, toolchain folder) in the shape of install.find_visual_studio(), or None when a part is
    missing (toolchain_missing names it). The env script is written into the toolchain folder (or %TEMP% if that is read-only)."""
    root = toolchain_dir()
    if toolchain_missing(root): return None
    try:
        script = env_script(root, root / 'clang-env.cmd')
    except OSError:
        import tempfile
        script = env_script(root, Path(tempfile.gettempdir()) / 'ma2-clang-env.cmd')
    return script, root / 'cmake' / 'bin' / 'cmake.exe', root / 'ninja' / 'ninja.exe', root


def clang_version(root=None):
    """'18.1.8' of the toolchain's clang-cl, or ''. Never raises."""
    try:
        exe = Path(root or toolchain_dir()) / 'llvm' / 'bin' / 'clang-cl.exe'
        out = subprocess.run([str(exe), '--version'], capture_output=True, text=True, timeout=60,
                             creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0)).stdout
        m = re.search(r'clang version (\S+)', out or '')
        return m.group(1) if m else ''
    except Exception:
        return ''


def toolchain_file():
    """data/wine-clang.cmake: the CMake toolchain file of the LLVM build (clang-cl, lld-link, llvm-rc, llvm-mt, the xwin CRT/SDK)."""
    return Path(__file__).resolve().parent / 'data' / 'wine-clang.cmake'


# ---------------------------------------------------------------------------- d3dcompiler_47.dll (Wine/Proton only)

D3DCOMPILER = 'd3dcompiler_47.dll'


def _file_version(path):
    """'10.0.26100.9457' from a DLL's version resource, or ''. Never raises."""
    try:
        v = ctypes.WinDLL('version')
        n = v.GetFileVersionInfoSizeW(str(path), None)
        if not n: return ''
        buf = ctypes.create_string_buffer(n)
        if not v.GetFileVersionInfoW(str(path), 0, n, buf): return ''
        p, ln = ctypes.c_void_p(), ctypes.c_uint()
        if not v.VerQueryValueW(buf, chr(92),ctypes.byref(p), ctypes.byref(ln)) or ln.value < 52: return ''
        ms, ls = ctypes.cast(p, ctypes.POINTER(ctypes.c_uint32))[2:4]
        return '%d.%d.%d.%d' % (ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF)
    except Exception:
        return ''


def d3dcompiler_usable(path):
    """True when `path` is a real (Microsoft) d3dcompiler_47.dll: a 64-bit PE of a plausible size that is not one of Wine's
    placeholder DLLs (Wine's own d3dcompiler_47 cannot compile the game's shaders). Never raises."""
    try:
        p = Path(path)
        if not p.is_file() or not (1 << 20) < p.stat().st_size < (64 << 20): return False
        data = p.read_bytes()
        if data[:2] != b'MZ' or b'Wine builtin DLL' in data[:4096] or b'Wine placeholder DLL' in data[:4096]: return False
        pe = int.from_bytes(data[0x3C:0x40], 'little')
        return data[pe:pe + 4] == b'PE\0\0' and int.from_bytes(data[pe + 4:pe + 6], 'little') == 0x8664
    except Exception:
        return False


def d3dcompiler_state(game_dir):
    """{'found', 'source', 'name', 'version'}: a usable d3dcompiler_47.dll in the game folder, else in MA2_WINE_D3DCOMPILER_DIR
    (the switch point for a later way to supply it, see data/toolchain.json "d3dcompiler"). Never raises."""
    name = D3DCOMPILER
    cands = [Path(game_dir) / name]
    extra = os.environ.get('MA2_WINE_D3DCOMPILER_DIR', '').strip().strip('"')
    if extra: cands.append(Path(dos_path(extra)) / name)
    for c in cands:
        if d3dcompiler_usable(c):
            return {'found': True, 'source': c, 'name': name, 'version': _file_version(c)}
    return {'found': False, 'source': None, 'name': name, 'version': ''}


# d3dcompiler_47.dll (owner decision 11-24: the builtin where it works, the player's own DLL only where needed; no Microsoft DLL
# ships, ever). Decided by a PROBE, not by a version number (Wine 10 is untested): this prefix's own D3DCompile compiles the
# game's three base shaders (kit/source/runtime/src/kernel/nv2a_gpu.hlsl). All three compile: nothing extra is needed
# (GE-Proton 11). Any one fails (Wine 9.0: ByteAddressBuffer is a syntax error, and the pixel shader crashes its compiler): the
# player is asked to put d3dcompiler_47.dll in the game folder. The probe runs in a child Python, so a compiler crash cannot take
# the installer down; a probe that cannot run counts as a failure (the message only asks for a file that always works).
PROBE_SHADERS = (('vertex_main', 'vs_5_0'), ('pixel_main', 'ps_5_0'), ('pixel_clamped', 'ps_5_0'))
PROBE_TIMEOUT = 180


def wine_version_tuple(v=None):
    """(9, 0) from '9.0', (10, 0) from '10.0-rc2'; (0, 0) when unknown. For the logs."""
    m = re.match(r'\s*(\d+)(?:\.(\d+))?', v if v is not None else (wine_version() or ''))
    return (int(m.group(1)), int(m.group(2) or 0)) if m else (0, 0)


def _compile_here(hlsl, entry, profile):
    """D3DCompile `entry` of the file `hlsl` with this process's d3dcompiler_47 (the prefix's own under Wine). (hr, first error line)."""
    from ctypes import c_void_p, c_char_p, c_size_t, c_uint, POINTER, byref
    d = ctypes.WinDLL('d3dcompiler_47', winmode=0)        # winmode=0: the normal search (Proton's needs it)
    src = Path(hlsl).read_bytes()
    f = d.D3DCompile
    f.argtypes = [c_char_p, c_size_t, c_char_p, c_void_p, c_void_p, c_char_p, c_char_p, c_uint, c_uint, POINTER(c_void_p), POINTER(c_void_p)]
    f.restype = ctypes.c_long
    code, err = c_void_p(), c_void_p()
    hr = f(src, len(src), b'nv2a_gpu.hlsl', None, None, entry.encode(), profile.encode(), 1 << 15, 0, byref(code), byref(err)) & 0xFFFFFFFF
    text = ''
    if err.value:
        vt = ctypes.cast(err, POINTER(POINTER(c_void_p))).contents
        ptr = ctypes.WINFUNCTYPE(c_void_p, c_void_p)(vt[3])(err); size = ctypes.WINFUNCTYPE(c_size_t, c_void_p)(vt[4])(err)
        text = ctypes.string_at(ptr, size).decode('utf-8', 'replace')
    return hr, (text.strip().splitlines() or [''])[0][:200]


def probe_d3dcompiler(hlsl, python=None):
    """{'ok': True when this prefix's own d3dcompiler_47 compiles all PROBE_SHADERS of `hlsl`, 'results': [...], 'detail': '...'}.
    Runs `python` (default: this one) on this file in a child process. Never raises."""
    import subprocess, sys
    if not python:                                       # the window runs pythonw.exe: the probe prints, so python.exe
        con = Path(sys.executable).with_name('python.exe')
        python = str(con) if con.is_file() else sys.executable
    try:
        r = subprocess.run([python, '-B', '-s', str(Path(__file__).resolve()), '--probe', str(hlsl)],
                           capture_output=True, text=True, timeout=PROBE_TIMEOUT, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        lines = [l for l in (r.stdout or '').splitlines() if l.startswith('probe ')]
        ok = r.returncode == 0 and len(lines) == len(PROBE_SHADERS) and all(' hr=0x00000000 ' in l + ' ' for l in lines)
        detail = '; '.join(l[6:] for l in lines) or ('exit %s: %s' % (r.returncode, ((r.stderr or '').strip().splitlines() or [''])[-1][:200]))
        if r.returncode and lines and len(lines) < len(PROBE_SHADERS): detail += '; then the compiler stopped (exit %s)' % r.returncode
        return {'ok': ok, 'results': lines, 'detail': detail}
    except Exception as e:
        return {'ok': False, 'results': [], 'detail': 'the probe could not run (%s)' % e}


def needs_own_d3dcompiler(hlsl, python=None):
    """(needed, probe result): True under Wine/Proton when the prefix's own d3dcompiler cannot compile the game's base shaders (the
    player must put Microsoft's d3dcompiler_47.dll in the game folder). (False, None) on Windows: nothing is probed there."""
    if not is_wine(): return False, None
    res = probe_d3dcompiler(hlsl, python)
    return not res['ok'], res


# ---------------------------------------------------------------------------- recomplauncher:// inside the prefix

LAUNCHER_SCHEME = 'recomplauncher'


def register_launcher_link():
    """Under Wine/Proton, make recomplauncher://... (the game's Messages "Update now" for a RecompLauncher install) open on the Linux
    side: Wine has no handler for that scheme, so ShellExecute fails there. The prefix's HKCU class is pointed at Wine's
    winebrowser, which hands the link to the desktop (xdg-open), where RecompLauncher is registered. An existing handler is left as
    it is. Returns a log line, or '' when nothing was done (Windows, or already there). Raises OSError on a registry failure."""
    if not is_wine(): return ''
    import winreg
    root = r'Software\Classes\%s' % LAUNCHER_SCHEME
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, root + r'\shell\open\command') as k:
            if winreg.QueryValueEx(k, '')[0]: return ''
    except OSError:
        pass
    sysdir = os.path.join(os.environ.get('SystemRoot', r'C:\windows'), 'system32')
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, root) as k:
        winreg.SetValueEx(k, '', 0, winreg.REG_SZ, 'URL:RecompLauncher')
        winreg.SetValueEx(k, 'URL Protocol', 0, winreg.REG_SZ, '')
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, root + r'\shell\open\command') as k:
        winreg.SetValueEx(k, '', 0, winreg.REG_SZ, '"%s" "%%1"' % os.path.join(sysdir, 'winebrowser.exe'))
    return '%s:// links open through winebrowser (Wine/Proton: Messages "Update now" reaches RecompLauncher)' % LAUNCHER_SCHEME


if __name__ == '__main__':          # python wine_host.py --probe <nv2a_gpu.hlsl>: one line per shader, for probe_d3dcompiler()
    import sys
    if len(sys.argv) == 3 and sys.argv[1] == '--probe':
        for entry, profile in PROBE_SHADERS:
            try:
                hr, first = _compile_here(sys.argv[2], entry, profile)
            except Exception as e:
                hr, first = 0xFFFFFFFF, repr(e)[:200]
            print('probe %s %s hr=0x%08X %s' % (entry, profile, hr, first), flush=True)
