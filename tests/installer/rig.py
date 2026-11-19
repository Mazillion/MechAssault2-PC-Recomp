"""Headless-ish rig for the installer window: the window is fully transparent (alpha 0, no taskbar button) and captured with PrintWindow.
Nothing is launched: App.launch (and, for the new window, the restart / sign-in hooks) are replaced. LOCALAPPDATA and the shortcut
folder point at temp folders.

MA2_UI_MODULE=gui_legacy runs the classic 1.1.3 window (gui_legacy.py) under the same name 'gui' (ui_flow_legacy.py does this)."""
import ctypes, importlib, os, sys, tempfile, time
from ctypes import wintypes
from pathlib import Path

WORK = Path(__file__).resolve().parents[1]
KIT = WORK.parent / 'kit'
sys.path.insert(0, str(KIT / 'installer'))
(WORK / 'tmp').mkdir(exist_ok=True)
os.environ.setdefault('LOCALAPPDATA', tempfile.mkdtemp(prefix='ma2ux-ui-local-', dir=str(WORK / 'tmp')))
os.environ.setdefault('MA2_SHORTCUT_DIR', tempfile.mkdtemp(prefix='ma2ux-ui-lnk-', dir=str(WORK / 'tmp')))
os.environ.setdefault('MA2_UI_DISC_SEARCH', '')          # never look through the real Downloads / Desktop for disc images
os.environ.pop('MA2_INSTALLER_EXE', None)                 # never add a real sign-in (RunOnce) entry
if os.environ.get('MA2_UI_MODULE', 'gui') != 'gui_legacy':
    os.environ.setdefault('MA2_UI_OFFSCREEN', '1')        # the new window: far off screen as well as transparent (the mouse never reaches it)
import tkinter as tk                                     # noqa: E402
MODULE = os.environ.get('MA2_UI_MODULE', 'gui')
gui = importlib.import_module(MODULE)                    # noqa: E402
sys.modules['gui'] = gui
import ui_theme, precheck, install                       # noqa: E402
LEGACY = MODULE == 'gui_legacy'

user32, gdi32 = ctypes.windll.user32, ctypes.windll.gdi32
KEEP = []                     # every window made here stays referenced: a background check finishing later must never be the one that frees Tk

def make(scale=None, plain=False, fonts=True):
    # The default install folder is C:\Games\MechAssault 2 on most PCs, and a PC that already has an install there would turn every wizard test into an
    # "update" run: point the default at a folder that never exists.
    precheck.default_install_folder = lambda: str(WORK / 'tmp' / 'no-default-install' / 'MechAssault 2')
    if fonts: ui_theme.load_fonts()
    root = tk.Tk(); KEEP.append(root)
    root.withdraw(); root.attributes('-alpha', 0.0); root.attributes('-toolwindow', True)   # fully transparent: never visible
    if LEGACY:
        if scale: root.tk.call('tk', 'scaling', scale * 96 / 72.0)
        theme = ui_theme.Theme(root, force_plain=plain)
        app = gui.App(root, theme)
        app.confirms = []; app.answer = True
        app.confirm = lambda title, text, yes='Stop', no='Keep installing': (app.confirms.append(text), app.answer)[1]
    else:
        app = gui.App(root, scale=scale or 1.0, plain=plain)
        app.signins, app.restarts = [], []
        app.set_resume_on_signin = lambda: (app.signins.append(1), True)[1]
        app.restart_windows = lambda: app.restarts.append(1)
        app.clear_signin = lambda: None
    app.launches = []
    try:                      # never activated: the test window must not take the keyboard from whatever the owner is using
        hwnd = user32.GetParent(root.winfo_id()) or root.winfo_id()
        user32.SetWindowLongW(hwnd, -20, user32.GetWindowLongW(hwnd, -20) | 0x08000000)       # WS_EX_NOACTIVATE
    except Exception:
        pass
    root.deiconify(); root.attributes('-alpha', 0.0)
    app.launch = lambda what, path: app.launches.append((what, str(path)))
    real = root.destroy
    root.destroy = lambda: (app.stop_pump(), real())
    return root, app

def pump(root, seconds=0.2):
    end = time.time() + seconds
    while time.time() < end:
        root.update_idletasks(); root.update(); time.sleep(0.01)

def until(root, cond, timeout=20, step=0.05):
    end = time.time() + timeout
    while time.time() < end:
        pump(root, step)
        if cond(): return True
    return False

def snap(root, path):
    """Save the window (title bar included) to a PNG using PrintWindow; works for a window that is off screen."""
    from PIL import Image
    root.update_idletasks(); root.update()
    hwnd = user32.GetParent(root.winfo_id()) or root.winfo_id()
    r = wintypes.RECT(); user32.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    dc = user32.GetDC(0); mem = gdi32.CreateCompatibleDC(dc); bmp = gdi32.CreateCompatibleBitmap(dc, w, h)
    gdi32.SelectObject(mem, bmp)
    user32.PrintWindow(hwnd, mem, 2)

    class BMI(ctypes.Structure):
        _fields_ = [('size', ctypes.c_uint), ('w', ctypes.c_int), ('h', ctypes.c_int), ('planes', ctypes.c_ushort), ('bits', ctypes.c_ushort),
                    ('comp', ctypes.c_uint), ('img', ctypes.c_uint), ('x', ctypes.c_int), ('y', ctypes.c_int), ('u', ctypes.c_uint), ('i', ctypes.c_uint)]
    bi = BMI(); bi.size = ctypes.sizeof(BMI); bi.w = w; bi.h = -h; bi.planes = 1; bi.bits = 32
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0)
    img = Image.frombuffer('RGBA', (w, h), buf, 'raw', 'BGRA', 0, 1).convert('RGB')
    gdi32.DeleteObject(bmp); gdi32.DeleteDC(mem); user32.ReleaseDC(0, dc)
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    img.save(path)
    return img
