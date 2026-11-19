"""Small Windows helpers for the installer window: DPI awareness, the dark title bar, the taskbar button (progress, flash),
dropping a file on the window, restarting Windows and the sign-in resume entry.

Every helper is optional polish. Each one is wrapped: if it fails it writes ONE line to the installer log, remembers the
fallback in FALLBACKS and the window carries on exactly as without it (plain Browse, no flash, no taskbar progress, ...).
Tests force a failure with MA2_UI_FAULT=<name>[,<name>...] (or 'all'): dpi, appid, titlebar, workarea, taskbar, flash, drop, anim.
"""
import ctypes, os, subprocess, sys
from ctypes import wintypes

FALLBACKS = []                     # names of the mechanisms that fell back this session (tests read it)

def faulted(name):
    f = {x.strip() for x in os.environ.get('MA2_UI_FAULT', '').split(',') if x.strip()}
    return name in f or 'all' in f

def _inject(name):
    if faulted(name): raise RuntimeError('injected fault (MA2_UI_FAULT=%s)' % name)

def degrade(name, err):
    if name not in FALLBACKS: FALLBACKS.append(name)
    try: print('ui: %s not available, using the plain fallback (%s)' % (name, err), flush=True)
    except Exception: pass

try:
    user32 = ctypes.WinDLL('user32', use_last_error=True)
    shell32 = ctypes.WinDLL('shell32')
except OSError:                    # not Windows: every helper below degrades
    user32 = shell32 = None

LRESULT = ctypes.c_ssize_t

# ---------------------------------------------------------------------------- process set-up (before tk.Tk())

def set_dpi_awareness():
    """Per-Monitor v2 when Windows has it; else the 1.1.3 behaviour (system DPI aware). Returns 'pmv2', 'system' or ''."""
    try:
        _inject('dpi')
        f = user32.SetProcessDpiAwarenessContext
        f.argtypes = [ctypes.c_void_p]; f.restype = wintypes.BOOL
        if f(ctypes.c_void_p(-4)): return 'pmv2'
        raise OSError('SetProcessDpiAwarenessContext refused (%d)' % ctypes.get_last_error())
    except Exception as e:
        degrade('dpi', e)
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
        return 'system'
    except Exception:
        return ''

def set_app_id(app_id):
    """One taskbar identity (and our icon) instead of python's."""
    try:
        _inject('appid')
        shell32.SetCurrentProcessExplicitAppUserModelID(ctypes.c_wchar_p(app_id))
        return True
    except Exception as e:
        degrade('appid', e); return False

# ---------------------------------------------------------------------------- the window

def frame_hwnd(root):
    """The top-level (wrapper) window Windows sees for a Tk root."""
    try:
        return int(root.wm_frame(), 16) or user32.GetParent(root.winfo_id())
    except Exception:
        return 0

def style_window(hwnd):
    """Dark native title bar and no maximise box (the window has one fixed size)."""
    try:
        _inject('titlebar')
        dwm = ctypes.WinDLL('dwmapi')
        on = ctypes.c_int(1)
        if dwm.DwmSetWindowAttribute(wintypes.HWND(hwnd), 20, ctypes.byref(on), 4) != 0:
            dwm.DwmSetWindowAttribute(wintypes.HWND(hwnd), 19, ctypes.byref(on), 4)
        get, put = user32.GetWindowLongW, user32.SetWindowLongW
        get.argtypes = [wintypes.HWND, ctypes.c_int]; get.restype = ctypes.c_long
        put.argtypes = [wintypes.HWND, ctypes.c_int, ctypes.c_long]; put.restype = ctypes.c_long
        style = get(hwnd, -16)
        put(hwnd, -16, style & ~0x00010000)                       # WS_MAXIMIZEBOX
        user32.SetWindowPos(wintypes.HWND(hwnd), None, 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0004 | 0x0010 | 0x0020)
        return True
    except Exception as e:
        degrade('titlebar', e); return False

def window_dpi(hwnd):
    """The window's current DPI (Per-Monitor v2), or 0 if unknown."""
    try:
        f = user32.GetDpiForWindow; f.argtypes = [wintypes.HWND]; f.restype = ctypes.c_uint
        return int(f(hwnd) or 0)
    except Exception:
        return 0

def work_area(hwnd=0):
    """(left, top, width, height) of the work area (screen minus taskbar) of the monitor holding the window, or None."""
    try:
        _inject('workarea')
        class MI(ctypes.Structure):
            _fields_ = [('cb', wintypes.DWORD), ('mon', wintypes.RECT), ('work', wintypes.RECT), ('flags', wintypes.DWORD)]
        mf = user32.MonitorFromWindow; mf.argtypes = [wintypes.HWND, wintypes.DWORD]; mf.restype = ctypes.c_void_p
        mon = mf(hwnd or None, 2 if hwnd else 1)
        mi = MI(); mi.cb = ctypes.sizeof(MI)
        gi = user32.GetMonitorInfoW; gi.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        if not mon or not gi(mon, ctypes.byref(mi)): raise OSError('GetMonitorInfo failed')
        r = mi.work
        return r.left, r.top, r.right - r.left, r.bottom - r.top
    except Exception as e:
        degrade('workarea', e); return None

def animations_on():
    """Windows' "Animation effects" setting (the progress scan line follows it)."""
    try:
        _inject('anim')
        v = wintypes.BOOL(1)
        if user32.SystemParametersInfoW(0x1042, 0, ctypes.byref(v), 0): return bool(v.value)
    except Exception as e:
        degrade('anim', e); return False
    return True

def flash(hwnd):
    """Flash the taskbar button until the window comes to the front (only when it is not in front already)."""
    try:
        _inject('flash')
        class FW(ctypes.Structure):
            _fields_ = [('cb', ctypes.c_uint), ('hwnd', wintypes.HWND), ('flags', ctypes.c_uint), ('count', ctypes.c_uint), ('timeout', ctypes.c_uint)]
        f = FW(); f.cb = ctypes.sizeof(FW); f.hwnd = hwnd; f.flags = 0x03 | 0x0C; f.count = 5     # FLASHW_ALL | FLASHW_TIMERNOFG
        user32.FlashWindowEx(ctypes.byref(f))
        return True
    except Exception as e:
        degrade('flash', e); return False

def is_foreground(hwnd):
    try:
        return bool(hwnd) and user32.GetForegroundWindow() == hwnd
    except Exception:
        return True

# ---------------------------------------------------------------------------- taskbar progress (ITaskbarList3, plain ctypes COM)

class _GUID(ctypes.Structure):
    _fields_ = [('a', ctypes.c_uint32), ('b', ctypes.c_uint16), ('c', ctypes.c_uint16), ('d', ctypes.c_ubyte * 8)]

def _guid(text):
    g = _GUID(); ctypes.oledll.ole32.CLSIDFromString(ctypes.c_wchar_p(text), ctypes.byref(g)); return g

class Taskbar:
    """Progress on the taskbar button. States: 'none', 'normal', 'paused' (amber: needs you), 'error' (red), 'busy'."""
    STATES = {'none': 0, 'busy': 1, 'normal': 2, 'error': 4, 'paused': 8}

    def __init__(self, hwnd):
        self.hwnd, self.obj, self.dead = hwnd, None, False

    def _ready(self):
        if self.obj or self.dead: return bool(self.obj)
        try:
            _inject('taskbar')
            ole = ctypes.oledll.ole32
            try: ole.CoInitializeEx(None, 2)                      # the window's thread is an STA (Tk's file dialog uses COM too)
            except OSError: pass                                   # already initialised (RPC_E_CHANGED_MODE is fine)
            obj = ctypes.c_void_p()
            ole.CoCreateInstance(ctypes.byref(_guid('{56FDF344-FD6D-11d0-958A-006097C9A090}')), None, 1,
                                 ctypes.byref(_guid('{ea1afb91-9e28-4b86-90e9-9e9f8a5eefaf}')), ctypes.byref(obj))
            self.obj = obj
            self._call(3, [])                                     # HrInit
            return True
        except Exception as e:
            self.dead = True; self.obj = None; degrade('taskbar', e); return False

    def _call(self, index, types, *args):
        vt = ctypes.cast(ctypes.cast(self.obj, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
        hr = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, *types)(vt[index])(self.obj, *args)
        if hr < 0: raise OSError('ITaskbarList3 call %d failed (0x%08X)' % (index, hr & 0xFFFFFFFF))

    def set(self, state, frac=None):
        if not self._ready(): return False
        try:
            self._call(10, [wintypes.HWND, ctypes.c_int], self.hwnd, self.STATES.get(state, 0))
            if frac is not None and state in ('normal', 'paused', 'error'):
                self._call(9, [wintypes.HWND, ctypes.c_ulonglong, ctypes.c_ulonglong], self.hwnd, int(max(0.0, min(1.0, frac)) * 1000), 1000)
            return True
        except Exception as e:
            self.dead = True; self.obj = None; degrade('taskbar', e); return False

# ---------------------------------------------------------------------------- dropping a file on the window (WM_DROPFILES)

SUBCLASSPROC = ctypes.WINFUNCTYPE(LRESULT, wintypes.HWND, ctypes.c_uint, wintypes.WPARAM, wintypes.LPARAM, ctypes.c_size_t, ctypes.c_size_t)

class DropTarget:
    """DragAcceptFiles plus a window subclass that reads WM_DROPFILES. The paths are only queued here (the window procedure must
    not call into Tk); the window's pump picks them up. install() returns False (and logs one line) if anything fails."""
    WM_DROPFILES = 0x0233

    def __init__(self):
        self.hwnd = 0; self.proc = None; self.paths = []; self.ok = False

    def install(self, hwnd):
        try:
            _inject('drop')
            comctl = ctypes.WinDLL('comctl32')
            self._set, self._remove, self._def = comctl.SetWindowSubclass, comctl.RemoveWindowSubclass, comctl.DefSubclassProc
            self._set.argtypes = [wintypes.HWND, SUBCLASSPROC, ctypes.c_size_t, ctypes.c_size_t]; self._set.restype = wintypes.BOOL
            self._remove.argtypes = [wintypes.HWND, SUBCLASSPROC, ctypes.c_size_t]; self._remove.restype = wintypes.BOOL
            self._def.argtypes = [wintypes.HWND, ctypes.c_uint, wintypes.WPARAM, wintypes.LPARAM]; self._def.restype = LRESULT
            self._query = shell32.DragQueryFileW
            self._query.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_wchar_p, ctypes.c_uint]; self._query.restype = ctypes.c_uint
            self._finish = shell32.DragFinish; self._finish.argtypes = [ctypes.c_void_p]
            self.proc = SUBCLASSPROC(self._wndproc)
            if not self._set(hwnd, self.proc, 0x4D41, 0): raise OSError('SetWindowSubclass failed')
            self.hwnd = hwnd
            shell32.DragAcceptFiles(wintypes.HWND(hwnd), True)
            self.ok = True
            return True
        except Exception as e:
            self.ok = False; degrade('drop', e); return False

    def _wndproc(self, hwnd, msg, wp, lp, uid, data):
        try:
            if msg == self.WM_DROPFILES:
                try:
                    n = self._query(wp, 0xFFFFFFFF, None, 0)
                    for i in range(min(n, 8)):
                        size = self._query(wp, i, None, 0) + 1
                        buf = ctypes.create_unicode_buffer(size)
                        self._query(wp, i, buf, size)
                        self.paths.append(buf.value)
                finally:
                    self._finish(wp)
                return 0
        except Exception as e:                                 # never let a drop break the window
            degrade('drop', e)
        return self._def(hwnd, msg, wp, lp)

    def take(self):
        p, self.paths = self.paths, []
        return p

    def remove(self):
        if self.ok and self.hwnd:
            try:
                shell32.DragAcceptFiles(wintypes.HWND(self.hwnd), False)
                self._remove(self.hwnd, self.proc, 0x4D41)
            except Exception:
                pass
        self.ok = False

def send_test_drop(hwnd, paths):
    """Tests: post a real WM_DROPFILES (a DROPFILES block in global memory, as Explorer sends) to the window."""
    data = ('\0'.join(paths) + '\0\0').encode('utf-16-le')
    head = 20
    k32 = ctypes.WinDLL('kernel32')
    k32.GlobalAlloc.restype = ctypes.c_void_p; k32.GlobalLock.restype = ctypes.c_void_p
    k32.GlobalLock.argtypes = [ctypes.c_void_p]; k32.GlobalUnlock.argtypes = [ctypes.c_void_p]
    h = k32.GlobalAlloc(0x0042, head + len(data))
    p = k32.GlobalLock(h)
    ctypes.memmove(p, (ctypes.c_uint32 * 5)(head, 0, 0, 0, 1), head)     # pFiles, pt.x, pt.y, fNC, fWide
    ctypes.memmove(p + head, data, len(data))
    k32.GlobalUnlock(h)
    sm = user32.SendMessageW; sm.argtypes = [wintypes.HWND, ctypes.c_uint, wintypes.WPARAM, wintypes.LPARAM]; sm.restype = LRESULT
    return sm(hwnd, 0x0233, h, 0)

# ---------------------------------------------------------------------------- restart (the sign-in entry is install.schedule_resume_after_restart)

def restart_windows():
    """Restart now (a normal user may restart; Windows asks other programs to close)."""
    exe = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'shutdown.exe')
    subprocess.Popen([exe, '/r', '/t', '0', '/d', 'p:4:2'], creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
