"""The MechAssault 2 PC Recomp Installer window (install.py does the work; precheck.py does the checks).

The common path is three screens: Setup (choose the disc, press Install), Installing, Done (LAUNCH). Everything else is a state of
those: Install options and Troubleshooting pages, Update / Repair / Continue for an existing or interrupted install, and the
"stop" screens for the few things only the player can do (approve the C++ tools, restart, free space, a blocked game).

The window drives the engine exactly as the 1.1.3 window did: the same options give the same Installer(...) call. Every new
mechanism (per-monitor DPI, file drop, taskbar progress and flash, the dark title bar) is optional and falls back on its own
(ui_win.py). If this window cannot start at all, the classic 1.1.3 window (gui_legacy.py) opens instead.
"""
import ctypes, faulthandler, json, os, sys, time, traceback      # the standard library only, until the start-up log is open
from pathlib import Path

HERE = Path(__file__).resolve().parent
TITLE = 'MechAssault 2 PC Recomp Installer'
APP_ID = 'MechAssault2.PCRecomp.Installer'
STARTUP_LOG = (None, None)

def startup_log():
    """Run as the installer window (pythonw.exe has no stdout/stderr): %LOCALAPPDATA%\\MechAssault2\\Logs\\installer-ui-<time>.log
    becomes stdout and stderr, and faulthandler writes there, before anything that can fail (imports, Tcl/Tk) runs."""
    try:
        d = Path(os.environ.get('LOCALAPPDATA') or str(Path.home() / 'AppData' / 'Local')) / 'MechAssault2' / 'Logs'
        d.mkdir(parents=True, exist_ok=True)
        for f in sorted(d.glob('installer-ui-*.log'), key=lambda f: f.stat().st_mtime)[:-9]:
            try: f.unlink()
            except OSError: pass
        path = d / ('installer-ui-%s.log' % time.strftime('%Y%m%d-%H%M%S'))
        log = open(path, 'a', encoding='utf-8', buffering=1)
    except OSError:
        return None, None
    sys.stdout = sys.stderr = log
    try: faulthandler.enable(log)
    except Exception: pass
    return path, log

if __name__ == '__main__':
    STARTUP_LOG = startup_log()

sys.path.insert(0, str(HERE))
IMPORT_ERROR = None
try:
    import queue, shutil, subprocess, threading
    import tkinter as tk
    from tkinter import filedialog, ttk
    import install, precheck, progress, ui_theme                     # noqa: E402
    import ui_kit, ui_win                                             # noqa: E402
    from ui_kit import P, HexButton, Link, Toggle, Segment, Zone      # noqa: E402
except Exception:                                                     # the classic window may still work (main() falls back)
    if __name__ != '__main__': raise
    IMPORT_ERROR = traceback.format_exc()
    try:
        import tkinter                                                # the classic window needs Tcl/Tk too: without it, say so and stop
    except Exception:
        traceback.print_exc()
        try: ctypes.windll.user32.MessageBoxW(None, 'The installer could not start (a part of it failed to load).\n\nDetails were saved to:\n%s'
                                              % (STARTUP_LOG[0] or 'the installer could not write a log'), TITLE, 0x10)
        except Exception: pass
        sys.exit(3)

# ---------------------------------------------------------------------------- words

PHASES = [('read', 'Reading your disc', ('check', 'disc', 'extract', 'verify')),
          ('setup', 'Setting things up', ('analyze', 'recompile', 'generate', 'patch')),
          ('build', 'Building the game', ('build',)),
          ('install', 'Installing', ('assets', 'install')),
          ('textures-2x', 'HD textures', ('textures-2x',)),
          ('textures-4x', 'Ultra HD textures', ('textures-4x',)),
          ('videos', 'Upscaled movies', ('videos',)),
          ('finish', 'Finishing up', ('finish',))]
EXTRA_SECONDS = {'textures-2x': 2 * 3600, 'textures-4x': 5 * 3600, 'videos': 8 * 3600}
TEX_NAMES = {'original': 'Original', '2x': 'HD', '4x': 'Ultra HD'}
FAILED = {
    'build_failed': ('Building the game didn\'t finish', 'Something stopped the build. Your progress is saved, so try again.'),
    'compile_error': ('Building the game failed', 'The C++ tools reported an error. If the install folder\'s name has unusual characters, '
                      'choose a simple one such as C:\\Games\\MechAssault 2. Otherwise try again.'),
    'access_denied': ('Windows or your antivirus blocked a file', 'If your antivirus reported a threat in the install folder and you trust '
                      'this game, restore the file from its protection history, then try again.'),
    'out_of_memory': ('This PC ran out of memory', 'Close other programs, then try again. Your progress is saved.'),
    'network': ('A download didn\'t finish', 'Check your internet connection, then try again.'),
    'gpu': ('Your graphics card can\'t make the HD content', 'The game itself is fine. Turn the HD options off and try again.'),
    'ffmpeg_mismatch': ('The cutscene player files aren\'t the expected build', 'Use exactly the file named in Details (a different version '
                        'or a damaged download doesn\'t work), then try again.'),
    'compiler': ('Microsoft\'s C++ tools didn\'t work', 'Try again. If it keeps happening, collect diagnostics from Help.'),
    'unexpected': ('Something went wrong', 'Your progress is saved, so try again. If it keeps happening, collect diagnostics from Help.'),
}
DISC_HINTS = {'not_xbox': ('That isn\'t an Xbox disc image', 'Choose an .iso or .xiso made from the MechAssault 2 disc'),
              'no_xbe': ('That isn\'t a MechAssault 2 disc image', 'It is a different game or an incomplete copy'),
              'wrong_title': ('That isn\'t a MechAssault 2 disc image', 'It is a different Xbox game'),
              'wrong_version': ('A different region or version', 'Only the US (NTSC) retail release is supported'),
              'modified': ('This copy is modified', 'Use an unmodified copy of your retail disc'),
              'truncated': ('This disc image is incomplete', 'Make the disc image again'),
              'archive': ('That\'s a compressed archive', 'Extract the .iso or .xiso from it first'),
              'compressed_image': ('Compressed images aren\'t supported', 'Use a plain .iso or .xiso'),
              'missing': ('That file wasn\'t found', 'Choose the disc image again'),
              'empty': ('That file is empty', 'Choose the disc image again'),
              'read_error': ('Windows couldn\'t read that file', 'Choose the disc image again'),
              'relative': ('Choose the file itself', 'Use Browse to pick it'),
              'disc': ('The disc image couldn\'t be read', 'Choose it again, or make the disc image again')}
SAC_TEXT = 'Windows Smart App Control is on. If it blocks the game, setup will help.'
HELP_TEXT = [
    ('Windows warned me about the installer', 'Windows SmartScreen warns about programs it does not recognize, and this installer is not '
     'code-signed. If you downloaded it from where you expected, choose More info, then Run anyway. The installer never changes your '
     'security settings.'),
    ('Where do I get a disc image?', 'You need your own copy of MechAssault 2 for the original Xbox (the US / NTSC retail disc), copied to a '
     'disc image (.iso or .xiso) or a folder. Tools that copy original Xbox discs are widely documented. This installer never links to '
     'download sites and carries no game files: the game\'s program, art, sound and levels all come from your disc.'),
    ('What does the installer do?', None),                  # TECH
    ('How long does it take?', 'Usually 15 to 40 minutes, depending on the PC. HD textures and upscaled movies are made on your graphics '
     'card afterwards and take hours. You can keep using the PC, play while they finish later, or add them any time by running the '
     'installer again.'),
    ('Smart App Control', 'If Windows Smart App Control is on, it can block programs that were built on this PC, including this game. If '
     'it does, setup tells you. Smart App Control can be turned off in Windows Security, but it can\'t be turned back on afterwards, so '
     'decide carefully. The installer never changes it.'),
    ('Settings, saves and logs', 'They are kept in %LOCALAPPDATA%\\MechAssault2. Updating, repairing and uninstalling keep them unless '
     'you ask otherwise. Your own scenarios in Documents\\MechAssault 2 are never deleted.'),
    ('Something went wrong', 'Open Help > Collect diagnostics. It saves one .zip with the installer and game logs (your user name is '
     'replaced) for a problem report on %s.'),
]

TECH = ('The installer reads your disc image, translates the original Xbox program to C with the open-source xboxrecomp toolkit, applies '
        'the PC release\'s fixes and features, compiles the game on this PC with Microsoft\'s C++ tools and builds its PC menus from '
        'your disc. The result is a ready-to-play folder. ' + precheck.NO_GAME_DATA + ' ' + precheck.SHADER_LINE) if IMPORT_ERROR is None else ''

def __getattr__(name):
    """FAILURES: the 1.1.3 window's failure texts (gui_legacy.py), for scripts and tests that read them. Loaded only when asked for."""
    if name == 'FAILURES':
        import gui_legacy
        return gui_legacy.FAILURES
    raise AttributeError(name)

def short_path(p, n):
    """A long path cut at folder boundaries: C:\\Users\\...\\Games\\MechAssault 2 (never in the middle of a name)."""
    p = str(p)
    if len(p) <= n: return p
    parts = p.split('\\')
    tail = []
    for part in reversed(parts[1:]):
        if len(parts[0]) + 5 + len('\\'.join([part] + tail)) > n and tail: break
        tail.insert(0, part)
    return parts[0] + '\\\u2026\\' + '\\'.join(tail)

def elide(text, font, width):
    """Cut text with an ellipsis so it fits `width` pixels in `font`."""
    if font.measure(text) <= width: return text
    while text and font.measure(text + '\u2026') > width: text = text[:-1]
    return text.rstrip() + '\u2026'

def open_path(p):
    try: os.startfile(str(p))
    except OSError: pass

def fmt_minutes(m):
    m = int(round(m))
    if m < 60: return '%d min' % max(1, m)
    h, r = divmod(m, 60)
    return '%d h %d min' % (h, r) if r else '%d h' % h

def fmt_left(sec):
    if sec < 60: return 'Less than a minute left'
    m = sec / 60.0
    if m < 10: return 'About %d min left' % max(1, round(m))
    if m < 60: return 'About %d min left' % (int(round(m / 5.0)) * 5)
    m = int(round(m / 10.0)) * 10
    return 'About %s left' % fmt_minutes(m)

# ---------------------------------------------------------------------------- the window

class App:
    W, H = 880, 520                                 # logical client size

    def __init__(self, root, scale=None, plain=None):
        self.root = root
        self.q = queue.Queue(); self.worker = None; self.inst = None
        self.launches_hook = None
        self.facts = {'gpus': None, 'reco': None, 'compiler': None, 'minutes': None, 'sac': None, 'vs_list': None}
        # choices (the same names and defaults as the 1.1.3 window)
        self.disc = ''; self.image = None; self.disc_token = 0; self.disc_checking = False
        self.dest = ''; self.folder = None; self.dest_token = 0; self.dest_chosen = False; self.tried_other = False
        self.texture = 'original'; self.videos = False
        self.sc = {'desktop': True, 'start': True, 'editor': False}
        self.awake = True; self.lowprio = False; self.merge = True; self.switch = True
        self.jobs = None; self.vs = None; self.keep_work = False; self.engine = 'auto'; self.upscaler = ''
        self.mode = 'install'; self.screen = 'setup'; self.page = None; self.stop = None; self.confirm = None
        self.notice = None; self.menu = None; self.failed = None; self.extras_failed = None
        self.model = None; self.lines = []; self.step_title = ''; self.slow = ''; self.shown_frac = 0.0; self.shown_eta = None
        self.started = None; self.show_details = False; self.tools_busy = False; self.tools_cancel = False
        self.w = {}; self.primary = None; self.want_focus = None; self.prog = {}; self.scan_x = 0.0
        self.closing = False; self.fallbacks = ui_win.FALLBACKS
        self.forced_scale = scale is not None
        # window
        root.title(TITLE)
        root.report_callback_exception = self.ui_error
        try: root.iconbitmap(default=str(HERE / 'data' / 'app.ico'))
        except tk.TclError: pass
        root.resizable(False, False)
        self.plain = ui_theme.high_contrast() if plain is None else plain
        root.update_idletasks()
        self.hwnd = ui_win.frame_hwnd(root)
        self.dpi_seen = ui_win.window_dpi(self.hwnd)
        self.k = ui_kit.Kit(root, self.pick_scale(scale), self.plain)
        root.configure(bg=P['bg'])
        self.k.style_ttk()
        self.cv = tk.Canvas(root, bg=P['bg'], highlightthickness=0, bd=0)
        self.cv.pack(fill='both', expand=True)
        self.place_window()
        self.draw_background()
        ui_win.style_window(self.hwnd)
        self.taskbar = ui_win.Taskbar(self.hwnd)
        self.drop = ui_win.DropTarget(); self.drop.install(self.hwnd)
        self.anim = ui_win.animations_on()
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.bind('<Return>', self.on_return); root.bind('<KP_Enter>', self.on_return)
        root.bind('<Escape>', self.on_escape)
        # what is in the remembered folder decides the first screen (as 1.1.3's Welcome banner did)
        self.dest = precheck.remembered_install() or precheck.default_install_folder()
        self.mode = self.mode_for(self.dest)
        self.load_installed_choices()
        self.check_dest()
        resume = self.read_resume_marker()
        for fn in (self.detect_hardware, self.detect_compiler, self.detect_misc, self.find_disc):
            threading.Thread(target=self.guarded(fn), daemon=True).start()
        self.render()
        self._after = root.after(100, self.pump)
        self._tick = 0
        if resume: root.after(300, lambda: self.resume_after_restart(resume))

    # -- scale and placement
    def pick_scale(self, forced=None):
        """One scale for everything: the monitor's DPI, clamped so the whole window fits the work area (1366x768 at 125 % and up)."""
        sys_scale = forced if forced else ((self.dpi_seen / 96.0) if self.dpi_seen else self.root.winfo_fpixels('1i') / 96.0)
        wa = self.work_area()
        fit = min((wa[3] - 40) / (self.H + 31.0), (wa[2] - 40) / float(self.W))
        return max(0.75, min(sys_scale, fit))

    def work_area(self):
        sw, sh = self.root.winfo_screenwidth(), self.root.winfo_screenheight()
        wa = ui_win.work_area(self.hwnd) or (0, 0, sw, sh - 48)
        return wa[0], wa[1], min(wa[2], sw), min(wa[3], sh - 40)

    def place_window(self):
        dp = self.k.dp
        w, h = dp(self.W), dp(self.H)
        self.cv.configure(width=w, height=h)
        if os.environ.get('MA2_UI_OFFSCREEN'):
            self.root.geometry('%dx%d+-32000+-32000' % (w, h)); return
        l, t, ww, wh = self.work_area()
        x = l + max(0, (ww - w) // 2); y = t + max(0, (wh - h - dp(31)) // 3)
        self.root.geometry('%dx%d+%d+%d' % (w, h, x, y))

    def rescale(self, scale):
        self.k.rescale(scale); self.k.style_ttk()
        self.place_window(); self.draw_background(); self.render()

    def draw_background(self):
        self.cv.delete('hex')
        self.hexes = [] if self.plain else ui_kit.hex_field(self.cv, self.k, self.k.dp(self.W), self.k.dp(self.H), self.k.dp(600))
        self.cv.tag_lower('hex')

    # -- helpers
    def guarded(self, fn):
        def run():
            try: fn()
            except Exception: print('ui: background check %s failed:\n%s' % (fn.__name__, traceback.format_exc()), flush=True)
        return run

    def bg(self, fn, cb):
        def run():
            try: res = fn()
            except Exception as e: res = e
            self.q.put(('call', lambda: cb(res)))
        threading.Thread(target=run, daemon=True).start()

    def ui_error(self, exc, val, tb):
        """A Tk callback raised: the traceback goes to the installer log; the window stays usable."""
        traceback.print_exception(exc, val, tb)

    # -- what is installed where
    def mode_for(self, dest):
        d = precheck.clean_path(dest)
        if not d: return 'install'
        try:
            if precheck.read_state(d) is not None: return 'resume'
            rel = install.install_relation(d)
        except Exception:
            return 'install'
        return 'update' if rel == 'older' else 'maintain' if rel in ('same', 'newer') else 'install'

    def state(self):
        return precheck.read_state(precheck.clean_path(self.dest)) or {}

    def have(self):
        d = Path(precheck.clean_path(self.dest) or '.')
        if self.mode not in ('update', 'maintain'): return {'2x': False, '4x': False, 'videos': False}
        return {'2x': (d / 'game_files/texture_packs/2x/index.json').is_file(), '4x': (d / 'game_files/texture_packs/4x/index.json').is_file(),
                'videos': (d / 'game_files/Movie/Enhanced/2x').is_dir()}

    def load_installed_choices(self):
        """An update keeps the installed textures choice on the Textures line (nothing is re-made: those packs are already there)."""
        if self.mode in ('update', 'maintain'):
            self.texture = 'original'; self.videos = False

    def game_files_ok(self):
        try:
            d = Path(precheck.clean_path(self.dest))
            xbe = d / 'game_files' / install.RELEASE['xbe']
            return xbe.is_file() and xbe.stat().st_size == install.RELEASE['xbe_bytes']
        except OSError:
            return False

    def action(self):
        if self.mode == 'install': return 'install'
        if self.mode == 'update': return 'update'
        if self.mode == 'resume': return 'resume'
        return 'addons' if self.new_extras() else 'repair'

    def new_extras(self):
        h = self.have()
        return (self.texture != 'original' and not h.get(self.texture)) or (self.videos and not h['videos'])

    def needs_disc(self):
        act = self.action()
        if act == 'addons': return False
        if act in ('update', 'repair') and self.game_files_ok(): return False
        if act == 'resume':
            st = self.state()
            return (not st.get('addons_only') and not st.get('core_done') and 'extract' not in st.get('done', [])
                    and not (st.get('update') and self.game_files_ok()))
        return act in ('install', 'update', 'repair')

    def opts(self):
        return dict(textures=[] if self.texture == 'original' else [self.texture], videos=self.videos)

    def done_set(self):
        act = self.action()
        return {'extract', 'build'} if act == 'addons' else ({'extract'} if self.game_files_ok() and act in ('update', 'repair') else set())

    def need_bytes(self):
        return install.needed_space(self.opts(), self.done_set(), (self.facts.get('reco') or {}).get('engine', 'pytorch'))

    def minutes(self):
        m = self.facts.get('minutes') or 15
        return 1 if self.action() == 'addons' else m

    def extras_time(self):
        o = self.opts()
        if not (o['textures'] or o['videos']): return ''
        return 'several hours' if (o['videos'] or '4x' in o['textures']) else 'a few hours'

    def params(self):
        """Installer arguments for the current choices (as the 1.1.3 window)."""
        action = self.action()
        tex = [] if self.texture == 'original' else [self.texture]
        return dict(xiso=precheck.clean_path(self.disc) or None, dest=precheck.clean_path(self.dest),
                    textures=tex, videos=self.videos, update=action in ('update', 'repair'),
                    shortcuts=[k for k, v in self.sc.items() if v] if action in ('install', 'update', 'repair') else [],
                    keep_awake=self.awake, low_priority=self.lowprio, merge_settings=self.merge, switch_textures=self.switch)

    # -- background facts
    def detect_hardware(self):
        gpus = precheck.detect_gpus()
        self.facts['gpus'] = gpus; self.facts['reco'] = precheck.recommend_textures(gpus)
        self.q.put(('call', self.after_facts))

    def detect_compiler(self):
        self.facts['compiler'] = install.compiler_state()

    def detect_misc(self):
        total, avail = precheck.memory_info()
        self.facts['minutes'] = precheck.estimate_minutes(os.cpu_count() or 4, avail, total)
        sac = getattr(precheck, 'sac_notice', None)
        self.facts['sac'] = ('enforce' if sac() else None) if sac else precheck.smart_app_control()
        self.q.put(('call', self.after_facts))

    def after_facts(self):
        if self.screen in ('setup', 'options'): self.check_dest(keep=True); self.render()

    def find_disc(self):
        """A disc image the player already has (Downloads, Desktop, the last one used): Setup opens on "Disc ready". Read-only."""
        env = os.environ.get('MA2_UI_DISC_SEARCH')
        if env is not None: dirs = [Path(p) for p in env.split(os.pathsep) if p]
        else:
            home = Path(os.environ.get('USERPROFILE') or Path.home())
            dirs = [home / 'Downloads', home / 'Desktop', home / 'Documents']
        cands = []
        last = (self.state() or {}).get('xiso')
        if last and Path(last).is_file(): cands.append(Path(last))
        for d in dirs:
            try:
                for f in sorted(d.iterdir()):
                    if f.suffix.lower() in ('.iso', '.xiso') and f.is_file() and 1 * precheck.GB <= f.stat().st_size <= 10 * precheck.GB: cands.append(f)
            except OSError:
                continue
        for f in cands[:8]:
            if self.disc or self.closing: return
            res = precheck.validate_image(str(f), install.RELEASE)
            if res.ok:
                print('ui: found the disc image %s' % f.name, flush=True)
                self.q.put(('call', lambda f=f, res=res: self.found_disc(str(f), res))); return

    def found_disc(self, path, res):
        if self.disc or self.screen not in ('setup', 'options'): return
        self.disc = path; self.image = res; self.want_focus = 'primary'; self.render()

    # -- choices
    def set_disc(self, text):
        text = precheck.clean_path(text)
        self.disc = text; self.image = None; self.disc_token += 1; token = self.disc_token
        if not text: self.disc_checking = False; self.render(); return
        if not precheck.is_absolute(text):
            self.image = precheck.ImageResult(False, 'relative', 'Enter a full path.', 'Not a full path'); self.render(); return
        self.disc_checking = True; self.render()
        self.bg(lambda: precheck.validate_image(text, install.RELEASE), lambda r: self.disc_checked(token, r))

    def disc_checked(self, token, res):
        if token != self.disc_token: return
        if isinstance(res, Exception):
            res = precheck.ImageResult(False, 'read_error', 'Couldn\'t read the file: %s.' % res, 'Can\'t read the file')
        self.disc_checking = False; self.image = res
        self.want_focus = 'primary' if res.ok else 'zone'
        self.render()

    def browse_disc(self):
        p = filedialog.askopenfilename(parent=self.root, title='MechAssault 2 disc image',
                                       filetypes=[('Xbox disc image', '*.iso *.xiso'), ('Game folder (ma2.xbe)', 'ma2.xbe'), ('All files', '*.*')])
        if p: self.set_disc(os.path.normpath(p))

    def set_dest(self, text, chosen=True):
        self.dest = precheck.clean_path(text); self.dest_chosen = self.dest_chosen or chosen
        new = self.mode_for(self.dest)
        if new != self.mode:
            self.mode = new; self.confirm = None; self.load_installed_choices()
        self.check_dest()
        self.render()

    def browse_dest(self):
        p = self.pick_folder()
        if p: self.set_dest(p)

    def pick_folder(self):
        """The folder picker; a folder with other things in it gets a new "MechAssault 2" folder inside it. None = cancelled."""
        p = filedialog.askdirectory(parent=self.root, title='Install folder', mustexist=False)
        if not p: return None
        p = os.path.normpath(p)
        try:                       # a folder with other things in it: install into a new "MechAssault 2" folder inside it (no question asked)
            pp = Path(p)
            if pp.is_dir() and any(pp.iterdir()) and precheck.read_state(p) is None and not precheck.is_install(p) and pp.parent != pp:
                p = str(pp / 'MechAssault 2')
            elif pp.parent == pp:
                p = str(pp / 'Games' / 'MechAssault 2')
        except OSError:
            pass
        return p

    def check_dest(self, keep=False):
        self.dest_token += 1; token = self.dest_token
        if not keep: self.folder = None
        text, need = self.dest, self.need_bytes()
        enhanced = self.texture != 'original' or self.videos
        self.bg(lambda: precheck.check_folder(text, need, enhanced, install.KIT), lambda r: self.dest_checked(token, r, need))

    def dest_checked(self, token, r, need):
        if token != self.dest_token or isinstance(r, Exception): return
        self.folder = r
        if 'space' in r.codes() and not self.dest_chosen and not self.tried_other and self.mode == 'install':
            self.tried_other = True
            other = self.other_drive(need)
            if other: self.set_dest(other, chosen=False); return
        if self.screen in ('setup', 'options'): self.render()

    def other_drive(self, need):
        """A fixed drive with room for the install, when the default drive has too little (instead of a stop)."""
        try:
            mask = ctypes.windll.kernel32.GetLogicalDrives()
        except Exception:
            return None
        cur = os.path.splitdrive(self.dest)[0].upper()
        best = None
        for i in range(26):
            if not mask & (1 << i): continue
            root = '%s:\\' % chr(65 + i)
            if root[:2] == cur or precheck.drive_type(root) != 3: continue
            try: free = shutil.disk_usage(root).free
            except OSError: continue
            if free > need * 1.3 and (best is None or free > best[1]): best = (root, free)
        return str(Path(best[0]) / 'Games' / 'MechAssault 2') if best else None

    def set_texture(self, v):
        self.texture = v; self.check_dest(keep=True); self.render()

    def set_videos(self, v):
        self.videos = bool(v); self.check_dest(keep=True); self.render()

    # -- navigation
    def go(self, screen, page=None, focus=None):
        if screen != self.screen: self.confirm = None
        self.screen = screen; self.page = page; self.menu = None
        self.want_focus = focus
        self.render()

    def on_return(self, e):
        if isinstance(e.widget, (tk.Text, ttk.Combobox)): return
        if self.primary is not None and self.primary.enabled and self.primary.winfo_exists():
            self.primary.invoke(); return 'break'

    def on_escape(self, e=None):
        if self.menu: self.menu = None; self.want_focus = 'help'; self.render(); return 'break'
        if self.confirm: self.confirm = None; self.render(); return 'break'
        back = {'options': 'setup', 'trouble': 'options', 'page': self.page_back if self.screen == 'page' else None,
                'diag': getattr(self, 'diag_back', 'setup'), 'uninstall': 'setup'}.get(self.screen)
        if back: self.go(back); return 'break'

    # -- rendering
    def render(self):
        if self.closing: return
        cur = self.focus_now()
        keep = getattr(cur, 'key', None) if cur is not None else None
        for wdg in list(self.w.values()):
            try: wdg.destroy()
            except tk.TclError: pass
        for c in self.cv.winfo_children():
            try: c.destroy()
            except tk.TclError: pass
        self.cv.delete('s'); self.w = {}; self.primary = None; self.prog = {}
        getattr(self, 'scr_' + self.screen)()
        if self.primary is not None: self.root.tk.call('raise', self.primary._w)       # the primary is the last Tab stop on every screen
        if self.menu: self.draw_menu()
        self.cv.tag_raise('s')
        want = self.want_focus; self.want_focus = None
        target = self.w.get(want) if want else None
        if target is None and want == 'primary': target = self.primary
        if target is None and keep: target = self.w.get(keep)
        if target is None and want is None and keep is None: target = self.default_focus()
        if target is not None:
            try:
                if target.enabled: target.focus_set()
            except (tk.TclError, AttributeError): pass
        self.update_title()

    def focus_now(self):
        """The window's focus widget, also while another program is in front (Tk's focus_get is None then)."""
        try: return self.root.nametowidget(self.root.tk.call('focus', '-lastfor', self.root))
        except (tk.TclError, KeyError): return None

    def default_focus(self):
        if self.primary is not None and self.primary.enabled: return self.primary
        for k in ('zone', 'browse'):
            if k in self.w: return self.w[k]
        return None

    def T(self, x, y, text, font='body', fill='text', anchor='nw', width=None, px=False):
        dp = (lambda v: v) if px else self.k.dp
        return self.cv.create_text(dp(x), dp(y), text=text, font=self.k.fonts[font], fill=P.get(fill, fill), anchor=anchor,
                                   width=dp(width) if width else 0, tags='s')

    def put(self, widget, x, y, anchor='nw', px=False):
        dp = (lambda v: v) if px else self.k.dp
        self.cv.create_window(dp(x), dp(y), window=widget, anchor=anchor, tags='s')
        if getattr(widget, 'key', None): self.w[widget.key] = widget
        return widget

    def link(self, key, text, cmd, bg='bg', **kw):
        w = Link(self.cv, self.k, text, cmd, P[bg], key=key, **kw)
        self.w[key] = w
        return w

    def button(self, key, text, cmd, enabled=True, big=False, bg='panel', primary=True):
        b = HexButton(self.cv, self.k, text, cmd, P[bg], big=big, enabled=enabled, key=key)
        self.w[key] = b
        if primary: self.primary = b
        return b

    def footer(self, left=(), right=()):
        """The footer bar: items are widgets or ('text', str, font, colour) or ('segs', [(str, colour)], font)."""
        k, dp, cv = self.k, self.k.dp, self.cv
        W, H = dp(self.W), dp(self.H); top = H - dp(64); cy = top + dp(32)
        cv.create_rectangle(0, top, W, H, fill=P['panel'], outline='', tags='s')
        cv.create_line(0, top, W, top, fill=P['line'], width=k.line(), tags='s')
        def width(it):
            if isinstance(it, tuple):
                if it[0] == 'text': return k.fonts[it[2]].measure(it[1])
                return sum(k.fonts[it[2]].measure(s) for s, _ in it[1])
            return it.winfo_reqwidth()
        def draw(it, x):
            if isinstance(it, tuple):
                if it[0] == 'text': cv.create_text(x, cy, text=it[1], font=k.fonts[it[2]], fill=P.get(it[3], it[3]), anchor='w', tags='s')
                else:
                    for s, c in it[1]:
                        cv.create_text(x, cy, text=s, font=k.fonts[it[2]], fill=P.get(c, c), anchor='w', tags='s'); x += k.fonts[it[2]].measure(s)
            else:
                cv.create_window(x, cy, window=it, anchor='w', tags='s')
        x = dp(40) - dp(4)
        for it in left: draw(it, x); x += width(it) + dp(14)
        x = W - dp(24) + dp(5)
        for it in reversed(right):
            x -= width(it); draw(it, x); x -= dp(14)

    def kicker(self, x, y, text, colour='amber'):
        dp = self.k.dp
        ui_kit.small_hex(self.cv, dp(x), dp(y + 9), dp(12), P[colour], tags='s')
        self.T(x + 20, y, text, 'kick', colour)

    def strip(self, y, text, bad=False, links=(), width=560):
        """A one-to-three line notice: grey bar (info) or red bar (a problem only the player can fix)."""
        k, dp, cv = self.k, self.k.dp, self.cv
        lw = sum(l.winfo_reqwidth() + dp(8) for l in links)
        t = cv.create_text(dp(40 + 40), dp(y + 10), text=text, font=k.fonts['small'], fill=P['text'], anchor='nw', width=dp(width - 54) - lw, tags='s')
        x0, y0, x1, y1 = cv.bbox(t)
        h = max(y1 - dp(y), dp(24)) + dp(10)
        r = cv.create_rectangle(dp(40), dp(y), dp(40 + width), dp(y) + h, fill=P['panel2'], outline='', tags='s')
        bar = cv.create_rectangle(dp(40), dp(y), dp(43), dp(y) + h, fill=P['danger'] if bad else P['steel'], outline='', tags='s')
        cv.tag_lower(r, t); cv.tag_lower(bar, t)
        ui_kit.icon(cv, 'info', dp(40 + 22), dp(y + 19), dp(9), 's')
        x = dp(40 + width) - dp(8)
        for l in reversed(links):
            cv.create_window(x, dp(y) + dp(5), window=l, anchor='ne', tags='s'); x -= l.winfo_reqwidth() + dp(8)
        return h / k.scale

    def row(self, y, label, value, small='', small_colour='muted', link=None, dim=False, first=False):
        dp, cv = self.k.dp, self.cv
        if first: cv.create_line(dp(40), dp(y), dp(600), dp(y), fill=P['line'], width=self.k.line(), tags='s')
        cv.create_line(dp(40), dp(y + 50), dp(600), dp(y + 50), fill=P['line'], width=self.k.line(), tags='s')
        c = (lambda n: 'dis_text') if dim else (lambda n: n)
        self.T(40, y + 25, label, 'small', c('muted'), anchor='w')
        t = self.T(160, y + 25, value, 'row', c('text'), anchor='w')
        if small:
            x1 = self.cv.bbox(t)[2]
            self.cv.create_text(x1 + dp(8), dp(y + 26), text=small, anchor='w', font=self.k.fonts['small'], fill=P[c(small_colour)], tags='s')
        if link is not None: self.put(link, 600 + 4, y + 25, anchor='e')

    def seg_total(self):
        """('segs', ...) for the live total in the footer, or a red one when the drive is too small."""
        need = self.need_bytes() * 1.1 / precheck.GB
        r = self.folder
        if r is not None and 'space' in r.codes():
            return ('segs', [('Needs ', 'text'), ('%.1f GB' % need, 'danger'), (' \u00b7 ', 'text'), ('%.1f GB' % ((r.free or 0) / precheck.GB), 'danger'), (' free', 'text')], 'mono')
        extra = self.extras_time()
        segs = [('Needs ', 'text'), ('%.1f GB' % need, 'amber'), (' \u00b7 about ', 'text'), (fmt_minutes(self.minutes()), 'amber')]
        if extra: segs += [(' + ', 'text'), (extra, 'amber')]
        return ('segs', segs, 'mono')

    def drive_line(self):
        r = self.folder
        drive = os.path.splitdrive(self.dest)[0] or '?'
        if r is None: return 'Drive %s \u00b7 checking' % drive, 'muted'
        if r.free is None: return 'Drive %s' % drive, 'muted'
        return 'Drive %s \u00b7 %.0f GB free' % (drive, r.free / precheck.GB) if r.free >= 10 * precheck.GB else \
               'Drive %s \u00b7 %.1f GB free' % (drive, r.free / precheck.GB), ('danger' if 'space' in r.codes() else 'muted')

    def tex_line(self):
        h = self.have()
        if self.mode in ('update', 'maintain') and self.texture == 'original' and not self.videos:
            got = [TEX_NAMES[x] for x in ('4x', '2x') if h.get(x)]
            return (got[0], 'already installed') if got else ('Original', '')
        reco = self.facts.get('reco')
        name = TEX_NAMES[self.texture] + (' + upscaled movies' if self.videos else '')
        if self.texture == 'original' and not self.videos:
            if reco is None: return 'Original', ''
            if reco.get('can') and reco.get('rec') in ('2x', '4x'): return 'Original', '%s recommended for your graphics card' % TEX_NAMES[reco['rec']]
            if reco.get('can'): return 'Original', 'HD available in Install options'
            return 'Original', ''
        return name, 'made after the game is ready'

    # ------------------------------------------------------------------ screens

    def scr_setup(self):
        if self.mode != 'install': return self.scr_existing()
        k, dp = self.k, self.k.dp
        self.T(40, 30, 'Install MechAssault 2', 'h1')
        ready = bool(self.image and self.image.ok)
        top = 86
        zh = self.make_zone(top)
        zb = top + zh + 18
        dim = not ready
        dim = False                                   # the rows work before the disc is chosen
        ch1 = self.link('dest', 'Change', self.browse_dest)
        dl, dc = self.drive_line()
        self.row(zb, 'Install to', short_path(self.dest, 34), dl, dc, ch1, dim, first=True)
        tl, ts = self.tex_line()
        ch2 = self.link('tex', 'Change', lambda: self.go('options'))
        self.row(zb + 50, 'Quality', tl, ts, 'muted', ch2, dim)
        opt = self.link('options', 'Install options', lambda: self.go('options'), icon='gear')
        self.put(opt, 36, zb + 112)
        self.setup_strip(zb + 150, ready)
        self.std_footer(self.install_primary(ready), diag=True)

    def install_primary(self, ready, label='INSTALL'):
        """(total item, primary button) for Setup-type screens."""
        r = self.folder
        if not ready:
            return ('text', 'Choose your disc to continue', 'small', 'muted'), self.button('primary', label, self.begin, enabled=False)
        if r is None:
            return ('text', 'Checking the install folder\u2026', 'small', 'muted'), self.button('primary', label, self.begin, enabled=False)
        err = r.first_error()
        if err and err.code == 'space':
            return self.seg_total(), self.button('primary', 'CHOOSE DRIVE', self.browse_dest)
        if err:
            return ('text', 'Choose another install folder', 'small', 'muted'), self.button('primary', 'CHOOSE FOLDER', self.browse_dest)
        return self.seg_total(), self.button('primary', label, self.begin)

    def std_footer(self, total_primary, legal=True, diag=False):
        total, prim = total_primary
        hl = self.link('help', 'Help', self.open_menu, quiet=True, bg='panel')
        left = [hl]
        if diag:
            left.append(self.link('f_diag', 'Collect diagnostics', self.open_diag, quiet=True, bg='panel'))
        if legal:
            left += [('text', 'Unofficial fan project', 'small', 'muted'), self.link('legal', 'Legal', lambda: self.show_page('legal'), quiet=True, bg='panel')]
        self.footer(left, [total, prim])

    def setup_strip(self, y, ready):
        """At most one notice under the rows: a folder problem, then the space stop, then a note, then Smart App Control."""
        r = self.folder
        err = r.first_error() if r else None
        if self.notice and not ready:
            self.strip(y, self.notice[1], self.notice[0] == 'bad'); return
        if not self.disc and not self.image and not self.disc_checking:
            self.put(self.link('where', 'Where do I get a disc image?', lambda: self.show_page('help')), 36, y); return
        if not ready: return
        if err and err.code == 'space':
            drive = os.path.splitdrive(self.dest)[0] or 'This drive'
            self.strip(y, '%s needs %.1f GB free and has %.1f GB. Choose another drive or free up some space.'
                       % (drive, self.need_bytes() * 1.1 / precheck.GB, (r.free or 0) / precheck.GB), bad=True)
        elif err:
            links = [self.link('fix', 'Use %s' % precheck.short(err.fix, 30), lambda f=err.fix: self.set_dest(f), bg='panel2')] if err.fix else []
            self.strip(y, err.text, bad=True, links=links)
        elif self.notice:
            self.strip(y, self.notice[1], self.notice[0] == 'bad')
        elif self.other_drive_short():
            self.strip(y, self.other_drive_short())
        elif r and any(f.level == 'warn' for f in r.findings):
            self.strip(y, next(f.text for f in r.findings if f.level == 'warn'))
        elif self.facts.get('sac') == 'enforce':
            self.strip(y, SAC_TEXT, links=[self.link('sac', 'Learn more', lambda: self.show_page('help'), bg='panel2')])

    def other_drive_short(self):
        """The engine's live space plan (B1) for the drives other than the install drive: the C++ tools on the system drive, the HD
        content's tools in TEMP. A note only (the install drive's own space is the CHOOSE DRIVE stop, as 1.1.3)."""
        try:
            comp = self.facts.get('compiler') or {}
            plan = install.space_plan(precheck.clean_path(self.dest), self.opts(), self.done_set(), (self.facts.get('reco') or {}).get('engine', 'pytorch'),
                                      None, build_tools=comp.get('state') == 'missing' and self.action() not in ('addons', 'resume'))
            inst = (os.path.splitdrive(os.path.abspath(precheck.clean_path(self.dest)))[0] or '').upper()
            for e in plan.get('drives', []):
                if e['drive'] in plan.get('short', []) and e['drive'] != inst:
                    why = "Microsoft's C++ tools" if 'buildtools' in e['what'] else "the HD content's tools while they unpack"
                    return ('Drive %s needs about %.0f GB free for %s and has %.1f GB. Free up some space there before installing.'
                            % (e['drive'], (e['core'] + e['extras']) / precheck.GB, why, (e['free'] or 0) / precheck.GB))
        except Exception as ex:
            print('ui: space plan not available (%s)' % ex, flush=True)
        return ''

    def make_zone(self, top, compact=False):
        """The disc drop zone (Setup). Returns its logical height."""
        k, dp = self.k, self.k.dp
        img, empty = self.image, not self.disc and not self.image
        h = 110 if (empty and not self.disc_checking) else 72
        z = Zone(self.cv, k, self.browse_disc, P['panel'], dp(560), dp(h))
        self.put(z, 40, top)
        name = Path(self.disc).name if self.disc else ''
        if self.disc_checking:
            z.setup('checking', 'Checking your disc\u2026', name)
        elif empty:
            z.setup('empty', 'Drop your disc image here' if self.drop.ok else 'Choose your disc image', '')
            pre = '.iso or .xiso file \u00b7 or '
            b = Link(z, k, 'browse for it', self.browse_disc, P['panel'], key='browse', small=True); self.w['browse'] = b
            pw = k.fonts['small'].measure(pre); tw = pw + b.winfo_reqwidth()
            x0 = dp(280) - tw / 2
            z.create_text(x0, dp(90), text=pre, anchor='w', font=k.fonts['small'], fill=P['muted'])
            z.create_window(x0 + pw, dp(90), window=b, anchor='w')
        elif img and img.ok:
            info = img.info or {}
            size = info.get('size')
            sub = 'MechAssault 2 \u00b7 %s' % ('%.1f GB \u00b7 US retail' % (size / precheck.GB) if size else 'game folder \u00b7 US retail')
            c = Link(z, k, 'Change', self.browse_disc, P['panel'], key='disc_change')
            z.setup('ok', 'Disc ready', elide(sub, k.fonts['small'], dp(560 - 78 - 26) - c.winfo_reqwidth()))
            self.w['disc_change'] = c
            z.create_window(dp(560 - 14), dp(36), window=c, anchor='e')
        else:
            code = img.code if img else 'missing'
            head, hint = DISC_HINTS.get(code, ('That disc image can\'t be used', img.message if img else ''))
            c = Link(z, k, 'Choose another file', self.browse_disc, P['panel'], key='disc_change')
            z.setup('bad', head, elide(('%s \u00b7 %s' % (hint, name)) if name else hint, k.fonts['small'], dp(560 - 78 - 26) - c.winfo_reqwidth()))
            self.w['disc_change'] = c
            z.create_window(dp(560 - 14), dp(36), window=c, anchor='e')
        return h

    def scr_existing(self):
        """Update, Repair / add content, and Continue (an interrupted install): the same layout."""
        k, dp = self.k, self.k.dp
        st = self.state(); stamp = precheck.read_stamp(precheck.clean_path(self.dest)) or {}
        m = self.mode
        core_done = m == 'resume' and st.get('core_done')
        if m == 'update':
            have = stamp.get('public_version')
            self.kicker(40, 30, ('%s INSTALLED' % have) if have else 'AN EARLIER VERSION IS INSTALLED')
            target = install.PUBLIC_VERSION if have != install.PUBLIC_VERSION else '%s (build %s)' % (install.PUBLIC_VERSION, install.BUILD)
            self.T(40, 56, 'Update to %s' % target, 'h1')
            self.T(40, 100, 'Your settings and saves stay as they are.', 'body', 'muted', width=560)
        elif m == 'maintain':
            rel = install.install_relation(precheck.clean_path(self.dest))
            self.kicker(40, 30, 'INSTALLED BY A NEWER INSTALLER' if rel == 'newer' else 'VERSION %s INSTALLED' % (stamp.get('public_version') or install.PUBLIC_VERSION), 'ok')
            self.T(40, 56, 'MechAssault 2 is installed', 'h1')
            self.T(40, 100, 'Add HD content in Install options, or repair the game. Your settings and saves are kept.', 'body', 'muted', width=560)
        else:
            self.kicker(40, 30, 'INSTALLED \u00b7 HD CONTENT NOT FINISHED' if core_done else 'INSTALL INTERRUPTED', 'ok' if core_done else 'amber')
            self.T(40, 56, 'Finish the HD content' if core_done else 'Continue installing', 'h1')
            last = st.get('last_title')
            self.T(40, 100, 'The game is ready to play. Continue makes only the HD content.' if core_done else
                   ('It stopped while %s. Steps already finished are kept.' % self.stopped_while(st.get('last_step'))
                    if st.get('last_step') else 'Steps already finished are kept.'), 'body', 'muted', width=560)
        y = 150
        dl, dc = self.drive_line()
        self.row(y, 'Installed in' if m != 'resume' else 'Install to', short_path(self.dest, 34), dl, dc,
                 self.link('dest', 'Change', self.install_elsewhere), first=True); y += 50     # another folder = a fresh install there
        if self.needs_disc():
            img = self.image
            if self.disc_checking: self.row(y, 'Disc', 'Checking\u2026', Path(self.disc).name, 'muted', None)
            elif img and img.ok: self.row(y, 'Disc', 'Disc ready', precheck.short(Path(self.disc).name, 26), 'muted', self.link('disc_change', 'Change', self.browse_disc))
            elif img:
                head, hint = DISC_HINTS.get(img.code, ('That disc image can\'t be used', ''))
                self.row(y, 'Disc', head, '', 'muted', self.link('disc_change', 'Choose another', self.browse_disc))
            else:
                self.row(y, 'Disc', 'Choose your disc image', '', 'muted', self.link('browse', 'Browse\u2026', self.browse_disc))
            y += 50
        elif m in ('update', 'maintain'):
            self.row(y, 'Disc', 'Not needed', 'your game files are reused', 'muted', None); y += 50
        if m != 'resume':
            tl, ts = self.tex_line()
            self.row(y, 'Quality', tl, ts, 'muted', self.link('tex', 'Change', lambda: self.go('options'))); y += 50
        y += 12
        x = 36
        items = []
        if m != 'resume': items.append(self.link('options', 'Install options', lambda: self.go('options'), icon='gear'))
        if m == 'resume':
            items.append(self.link('startover', 'Discard the HD content' if core_done else 'Start over',
                                   lambda: self.ask('startover'), quiet=True))
        if m in ('update', 'maintain'):
            items.append(self.link('uninstall', 'Uninstall', lambda: self.go('uninstall'), quiet=True))
        for it in items: self.put(it, x, y); x += it.winfo_reqwidth() / k.scale + 18
        if self.notice: self.strip(y + 44, self.notice[1], self.notice[0] == 'bad')
        if self.confirm: return self.confirm_footer()
        ok = (not self.needs_disc() or (self.image and self.image.ok))
        label = {'update': 'UPDATE', 'resume': 'CONTINUE'}.get(m) or ('ADD CONTENT' if self.action() == 'addons' else 'REPAIR')
        if m == 'resume':
            tp = (('text', 'Finished steps are kept', 'small', 'muted'), self.button('primary', label, self.begin, enabled=bool(ok)))
            if not ok: tp = (('text', 'Choose your disc to continue', 'small', 'muted'), tp[1])
        else:
            tp = self.install_primary(bool(ok), label)
        exe = Path(precheck.clean_path(self.dest)) / 'MechAssault 2.exe'
        if m in ('update', 'maintain') or (m == 'resume' and core_done):
            if m in ('update', 'maintain') and tp[0][0] == 'segs': tp = (('segs', [('Rebuilds the game \u00b7 ', 'text')] + tp[0][1][1:], 'mono'), tp[1])
            if exe.is_file():
                play = self.button('play', 'PLAY', self.play, primary=False)
                play.ghost = True; play.draw()
                total, prim = tp
                hl = self.link('help', 'Help', self.open_menu, quiet=True, bg='panel')
                self.footer([hl], [total, play, prim]); return
        self.std_footer(tp)

    def stopped_while(self, step):
        """The same plain step names as the Installing list ("building the game", "making the HD textures")."""
        t = self.phase_title(step)
        return ('making the ' + t) if step in EXTRA_SECONDS else t[:1].lower() + t[1:]

    def install_elsewhere(self):
        """Change on an installed / interrupted install: pick another folder for a fresh install there (the existing install is left
        exactly as it is; picking it again keeps Update / Repair / Continue)."""
        p = self.pick_folder()
        if not p or os.path.normcase(os.path.abspath(p)) == os.path.normcase(os.path.abspath(precheck.clean_path(self.dest) or '.')): return
        self.notice = None; self.confirm = None
        self.set_dest(p)

    # -- confirmation in the footer (Start over, Discard, Cancel)
    def ask(self, what):
        self.confirm = what; self.want_focus = 'safe'; self.render()

    def confirm_footer(self):
        what = self.confirm
        if what == 'cancel':
            q, go, safe = 'Cancel? Your progress is kept.', ('Cancel install', self.cancel_install), 'KEEP INSTALLING'
        elif what == 'startover':
            core = self.state().get('core_done')
            q = 'Delete the unfinished HD content? The game stays.' if core else 'Delete the unfinished work? Your game files are kept.'
            go, safe = ('Discard' if core else 'Start over', self.start_over), 'KEEP IT'
        else:
            return
        d = self.link('confirm_go', go[0], go[1], bg='panel', danger=True)
        b = self.button('safe', safe, lambda: self.ask(None))
        self.footer([d], [('text', q, 'small', 'text'), b])

    def start_over(self):
        d = precheck.clean_path(self.dest)
        try: install.start_over(d)
        except Exception as e: print('ui: start over failed: %s' % e, flush=True)
        self.confirm = None; self.mode = self.mode_for(d); self.notice = None; self.check_dest(); self.render()

    # -- Install options and Troubleshooting
    def scr_options(self):
        k, dp, cv = self.k, self.k.dp, self.cv
        cv.create_rectangle(0, 0, dp(self.W), dp(self.H), fill=P['panel'], outline='', tags='s')
        self.T(40, 22, 'INSTALL OPTIONS', 'h1s')
        y = 66
        secs = []
        def sec(title, subs, widget):
            nonlocal y
            n = len(subs)
            h = 30 + 18 * n
            self.T(40, y + 11, title, 'row')
            for i, (s, c) in enumerate(subs): self.T(40, y + 34 + 18 * i, s, 'small', c, width=520)
            if widget is not None: self.put(widget, 840 + (4 if isinstance(widget, Link) else 0), y + h / 2 + 4, anchor='e')
            y += h + 8
            secs.append(y)
            cv.create_line(dp(40), dp(y - 4), dp(840), dp(y - 4), fill=P['line'], width=k.line(), tags='s')
        dl, _ = self.drive_line()
        need = self.need_bytes() * 1.1 / precheck.GB
        loc = '%s \u00b7 %s \u00b7 needs %.1f GB' % (short_path(self.dest, 40), dl, need)
        sec('Install location', [(loc, 'danger' if self.folder and 'space' in self.folder.codes() else 'muted')],
            self.link('o_dest', 'Change', self.browse_dest if self.mode == 'install' else self.install_elsewhere, bg='panel'))
        reco = self.facts.get('reco'); h = self.have()
        if reco is None:
            seg = Segment(cv, k, [('original', 'Original', True), ('2x', 'HD', False), ('4x', 'Ultra HD', False)], self.texture, self.set_texture, P['panel'], key='o_tex')
            sec('Textures', [('Checking your graphics card\u2026', 'muted')], seg)
        elif reco.get('can'):
            opts = [('original', 'Original', True), ('2x', 'HD', not h['2x']), ('4x', 'Ultra HD', not h['4x'])]
            seg = Segment(cv, k, opts, self.texture, self.set_texture, P['panel'], key='o_tex')
            o = self.opts()
            extra = (install.needed_space(o, self.done_set(), reco.get('engine', 'pytorch')) -
                     install.needed_space({'textures': [], 'videos': o['videos']}, self.done_set(), reco.get('engine', 'pytorch'))) / precheck.GB
            subs = [({'original': 'Original \u00b7 no extra space', '2x': 'HD (2x) \u00b7 +%.1f GB \u00b7 a few hours' % extra,
                      '4x': 'Ultra HD (4x) \u00b7 +%.1f GB \u00b7 several hours' % extra}[self.texture], 'muted')]
            if reco.get('engine') == 'vulkan':
                subs.append(('Made on your graphics card through Vulkan (new, not yet tested on AMD or Intel cards).', 'muted'))
            elif reco.get('rec') in ('2x', '4x'):
                subs.append(('%s recommended for your graphics card.' % TEX_NAMES[reco['rec']], 'muted'))
            got = [TEX_NAMES[x] for x in ('2x', '4x') if h.get(x)]
            if got: subs.append(('Already installed: %s.' % ' and '.join(got), 'text'))
            sec('Textures', subs, seg)
        can_vid = reco is not None and reco.get('videos', reco.get('can'))
        if can_vid:
            tg = Toggle(cv, k, self.videos, self.set_videos, P['panel'], enabled=not h['videos'], key='o_vid')
            subs = [('Already installed.', 'text')] if h['videos'] else [('Sharpens the game\'s pre-rendered movies. In-game cutscenes are unaffected.', 'text'), ('+3.5 GB \u00b7 needs 25 GB free while it runs \u00b7 many hours \u00b7 uses your graphics card', 'muted')]
            sec('Upscaled movies', subs, tg)
        if self.action() in ('install', 'update', 'repair'):
            f = tk.Frame(cv, bg=P['panel'])
            for key, label in (('desktop', 'Desktop'), ('start', 'Start menu'), ('editor', 'Scenario Editor')):
                tk.Label(f, text=label, font=k.fonts['small'], fg=P['muted'], bg=P['panel']).pack(side='left', padx=(dp(14), dp(4)))
                t = Toggle(f, k, self.sc[key], lambda v, key=key: self.sc.__setitem__(key, v), P['panel'], key='o_sc_' + key)
                t.pack(side='left'); self.w[t.key] = t
            sec('Shortcuts', [('Where to find the game', 'muted')], f)
        self.put(self.link('trouble', 'Troubleshooting\u2026', lambda: self.go('trouble'), quiet=True, bg='panel'), 36, y + 4)
        self.footer([], [self.seg_total(), self.button('done', 'DONE', lambda: self.go('setup', focus='options'))])

    def scr_trouble(self):
        k, dp, cv = self.k, self.k.dp, self.cv
        cv.create_rectangle(0, 0, dp(self.W), dp(self.H), fill=P['panel'], outline='', tags='s')
        self.T(40, 22, 'TROUBLESHOOTING', 'h1s')
        self.T(40, 58, 'For when support asks. Automatic is right for almost everyone.', 'body', 'muted')
        self.T(40, 98, 'While installing', 'row')
        self.T(40, 120, '"Keep my PC responsive" builds more slowly in the background.', 'small', 'muted')
        seg = Segment(cv, k, [(False, 'Normal', True), (True, 'Keep my PC responsive', True)], self.lowprio,
                      lambda v: setattr(self, 'lowprio', v), P['panel'], key='t_prio')
        self.put(seg, 840, 110, anchor='e')
        cv.create_line(dp(40), dp(146), dp(840), dp(146), fill=P['line'], width=k.line(), tags='s')
        y = 160
        def kv(label, widget, note=''):
            nonlocal y
            self.T(40, y + 14, label, 'small', 'muted', anchor='w')
            self.put(widget, 280, y + 14, anchor='w')
            if note: self.T(560, y + 14, note, 'small', 'muted', anchor='w')
            y += 34
        def combo(key, values, current, on):
            cb = ttk.Combobox(cv, values=[v[1] for v in values], state='readonly', style='Dark.TCombobox', font=k.fonts['mono_s'], width=24)
            cb.set(next((v[1] for v in values if v[0] == current), values[0][1]))
            cb.bind('<<ComboboxSelected>>', lambda e: on(values[[v[1] for v in values].index(cb.get())][0]))
            cb.key = key; self.w[key] = cb
            return cb
        vs = [(None, 'Automatic')] + [(p, n) for p, n in (self.facts.get('vs_list') or [])]
        if self.facts.get('vs_list') is None:
            self.facts['vs_list'] = []
            self.bg(lambda: [(i['installationPath'], i.get('displayName') or Path(i['installationPath']).name) for i in install.vs_instances()],
                    lambda r: (self.facts.__setitem__('vs_list', [] if isinstance(r, Exception) else r), self.screen == 'trouble' and self.render()))
        kv('C++ tools', combo('t_vs', vs, self.vs, lambda v: setattr(self, 'vs', v)))
        kv('Parallel jobs', combo('t_jobs', [(None, 'Automatic')] + [(n, str(n)) for n in (1, 2, 4, 8, 16)], self.jobs, lambda v: setattr(self, 'jobs', v)))
        kv('HD content made with', combo('t_engine', [('auto', 'Automatic'), ('pytorch', 'NVIDIA graphics card (CUDA)'), ('vulkan', 'Any graphics card (Vulkan)'),
                                                      ('cpu', 'Processor only (very slow)')], self.engine, lambda v: setattr(self, 'engine', v)))
        up = self.link('t_up', precheck.short(Path(self.upscaler).name, 26) if self.upscaler else 'Choose file\u2026', self.pick_upscaler, bg='panel')
        kv('Offline upscaler files', up, 'for a PC without internet' if not self.upscaler else '')
        kv('Keep the work folder', Toggle(cv, k, self.keep_work, lambda v: setattr(self, 'keep_work', v), P['panel'], key='t_keep'))
        kv('Keep the PC awake', Toggle(cv, k, self.awake, lambda v: setattr(self, 'awake', v), P['panel'], key='t_awake'))
        if self.action() in ('update', 'repair'):
            kv('Add new settings', Toggle(cv, k, self.merge, lambda v: setattr(self, 'merge', v), P['panel'], key='t_merge'), 'a backup is kept')
        if self.action() in ('update', 'repair', 'addons'):
            kv('Use new HD content now', Toggle(cv, k, self.switch, lambda v: setattr(self, 'switch', v), P['panel'], key='t_switch'))
        back = self.link('back', '\u2039 Back', lambda: self.go('options', focus='trouble'), quiet=True, bg='panel')
        self.footer([back], [self.button('done', 'DONE', lambda: self.go('setup', focus='options'))])

    def pick_upscaler(self):
        p = filedialog.askopenfilename(parent=self.root, title='Upscaler files (zip)', filetypes=[('Zip file', '*.zip'), ('All files', '*.*')])
        self.upscaler = os.path.normpath(p) if p else ''
        self.render()

    # -- Installing
    def begin(self):
        """Primary on Setup / Update / Repair / Continue: the checks the 1.1.3 Systems page did, then the install."""
        self.notice = None
        act = self.action()
        if act == 'resume' or act == 'addons': return self.start()
        try:
            w = precheck.windows_info()
        except Exception:
            w = {'ok': True}
        if not w.get('ok', True):
            return self.show_stop(style='failed', icon='blocked', kicker='CAN\'T INSTALL', head='This PC needs a newer Windows',
                                  body='MechAssault 2 PC Recomp needs 64-bit Windows 10 (version 1903 or later) or Windows 11.', primary=None)
        comp = self.facts.get('compiler')
        if comp is None:
            self.waiting('Getting ready\u2026')
            self.bg(install.compiler_state, self.after_compiler)
        else:
            self.after_compiler(comp)

    def after_compiler(self, comp):
        if isinstance(comp, Exception):
            print('ui: compiler check failed (the install checks again): %s' % comp, flush=True); comp = {'state': 'ready'}
        self.facts['compiler'] = comp
        st = comp.get('state')
        if st == 'ready': return self.start()
        if st == 'incomplete':
            names = [install.VS_PARTS[m][0] for m in comp.get('missing') or [] if m in install.VS_PARTS]
            return self.show_stop(style='needs', icon='needs', kicker='ACTION NEEDED', head='Microsoft\'s C++ tools need a part',
                                  body='Visual Studio is missing %s. Add it in the Visual Studio Installer, then press Check again.'
                                       % (' and '.join(names) or 'a part the game needs'),
                                  primary=('OPEN VS INSTALLER', self.open_vsi), links=[('Check again', self.recheck_compiler)], details=comp.get('detail'),
                                  back=True, help=True)
        sysdrive = os.environ.get('SystemDrive', 'C:') + '\\'
        try: free = shutil.disk_usage(sysdrive).free
        except OSError: free = None
        if free is not None and free < 8 * precheck.GB:
            return self.show_stop(style='needs', icon='needs', kicker='ACTION NEEDED', head='Free up space on %s' % sysdrive[:2],
                                  body='Microsoft\'s C++ tools need about 8 GB free on %s, and it has %.1f GB. Free up some space, then press Check again.'
                                       % (sysdrive[:2], free / precheck.GB), primary=('CHECK AGAIN', self.recheck_compiler))
        self.show_stop(style='needs', icon='needs', kicker='ACTION NEEDED', head='Install Microsoft\'s C++ tools',
                       body='The game is built on this PC with Microsoft\'s free C++ Build Tools (about 6 GB, 10-30 minutes). Windows will ask '
                            'for permission. By continuing you accept Microsoft\'s license terms for the Build Tools.',
                       primary=('INSTALL C++ TOOLS', self.install_tools), back=True, help=True,
                       links=[('License terms', lambda: self.web('https://visualstudio.microsoft.com/license-terms/')),
                              ('I\'ll install them myself', self.manual_tools)])

    def web(self, url):
        import webbrowser
        webbrowser.open(url)

    def open_vsi(self):
        comp = self.facts.get('compiler') or {}
        try: install.open_vs_installer(comp.get('vs'), comp.get('add'))
        except Exception as e: print('ui: could not open the Visual Studio Installer: %s' % e, flush=True)

    def manual_tools(self):
        self.web('https://visualstudio.microsoft.com/visual-cpp-build-tools/')
        self.show_stop(style='needs', icon='needs', kicker='ACTION NEEDED', head='Install Microsoft\'s C++ tools',
                       body='Install "Desktop development with C++" from the page that opened, then press Check again.',
                       primary=('CHECK AGAIN', self.recheck_compiler))

    def recheck_compiler(self):
        self.waiting('Checking Microsoft\'s C++ tools\u2026')
        self.bg(install.compiler_state, self.after_compiler)

    def waiting(self, text):
        self.stop = None; self.model = None; self.step_title = text; self.slow = ''
        self.go('progress')

    def install_tools(self):
        """The one-click Build Tools install, exactly as the 1.1.3 Systems page ran it, then on into the game install."""
        self.tools_busy = True; self.tools_cancel = False
        self.waiting('Installing Microsoft\'s C++ tools\u2026')
        self.slow = 'Windows will ask for permission; Microsoft\'s own window shows its progress (10-30 minutes)'
        self.render()
        work = Path(os.environ.get('TEMP', '.')) / 'MA2-buildtools'
        logf = None
        try:
            d = precheck.logs_dir(); d.mkdir(parents=True, exist_ok=True)
            logf = open(d / ('buildtools-%s.log' % time.strftime('%Y%m%d-%H%M%S')), 'a', encoding='utf-8', buffering=1)
        except OSError:
            pass
        def note(line):
            if logf:
                try: logf.write(line + '\n')
                except (OSError, ValueError): pass
            self.q.put(('log', line))
            if 'downloading' in line.lower(): self.q.put(('call', lambda: self.set_slow('Downloading Microsoft\'s installer')))
        def go():
            try:
                note('MechAssault 2 Recomp installer, Build Tools one-click install, %s' % time.strftime('%Y-%m-%d %H:%M'))
                install.install_build_tools(work, note, lambda: self.tools_cancel)
                res = install.compiler_state()
                note('compiler check afterwards: %s' % res.get('state'))
                return res
            except Exception as e:
                note('FAILED: %s' % e); raise
            finally:
                shutil.rmtree(work, ignore_errors=True)
                if logf: logf.close()
        def done(res):
            self.tools_busy = False
            if isinstance(res, Exception):
                if getattr(res, 'code', None) == 'reboot': return self.show_restart()
                if getattr(res, 'kind', None) == 'cancelled': return self.go('setup')
                return self.show_stop(style='failed', icon='bad', kicker='COULDN\'T FINISH', head='Microsoft\'s C++ tools didn\'t install',
                                      body='Check your internet connection and try again, or install them yourself.', details=str(res),
                                      primary=('TRY AGAIN', self.install_tools), links=[('I\'ll install them myself', self.manual_tools)])
            self.facts['compiler'] = res
            self.after_compiler(res)
        self.bg(go, done)

    def set_slow(self, text):
        self.slow = text
        if self.screen == 'progress': self.update_progress()

    def start(self):
        """Start the engine (or the recorded-log fake in tests) on a worker thread; the same call as the 1.1.3 window."""
        self.failed = None; self.extras_failed = None; self.lines = []; self.model = None; self.step_title = 'Getting ready'; self.slow = ''
        self.shown_frac = 0.0; self.shown_eta = None; self.stop = None; self.confirm = None
        p = self.params()
        if self.action() == 'resume':
            st = self.state()
            o = st.get('options') or {}
            p.update(textures=o.get('textures', []), videos=o.get('videos', False), update=bool(st.get('update')), xiso=p['xiso'] or st.get('xiso'))
        extra = {}
        if self.jobs: extra['jobs'] = self.jobs
        if self.vs: extra['vs'] = self.vs
        if self.keep_work: extra['keep_work'] = True
        if self.engine == 'cpu': extra['allow_cpu'] = True
        for var in ('MA2_TEXTURE_ENGINE', 'MA2_VIDEO_ENGINE'):
            if self.engine in ('pytorch', 'vulkan', 'cpu'): os.environ[var] = 'vulkan' if self.engine == 'vulkan' else 'pytorch'
        if self.upscaler: os.environ['MA2_UPSCALER_SOURCE'] = self.upscaler
        q = self.q
        cb = dict(log=lambda s: q.put(('log', s)), step=lambda i, n, title, name, skipped=False: q.put(('step', (i, n, title, name, skipped))))
        fake = os.environ.get('MA2_INSTALLER_FAKE')
        if fake:
            import fakeinstall
            f = os.environ.get('MA2_FAKE_FAIL')
            self.inst = fakeinstall.FakeInstaller(fake, speed=float(os.environ.get('MA2_FAKE_SPEED', '40')), fail=tuple(f.split(':')) if f else None,
                                                  extra=[x for x in os.environ.get('MA2_FAKE_EXTRA', '').split(',') if x], dest=p['dest'], **cb)
        else:
            self.inst = install.Installer(p['xiso'], p['dest'], p['textures'], p['videos'], install_build_tools=False, update=p['update'],
                                          shortcuts=p['shortcuts'], keep_awake=p['keep_awake'], low_priority=p['low_priority'],
                                          merge_settings=p['merge_settings'], switch_textures=p['switch_textures'], **extra, **cb)
            self.inst.on_status = lambda text: q.put(('status', text))
            self.inst.until = os.environ.get('MA2_INSTALLER_UNTIL')
        self.inst.on_plan = lambda names: q.put(('plan', names))
        self.last_params = p
        self.started = time.monotonic()
        self.worker = threading.Thread(target=self.work, daemon=True); self.worker.start()
        self.go('progress')
        self.taskbar.set('normal', 0.0)

    def work(self):
        q, inst = self.q, self.inst
        try:
            inst.run(); q.put(('done', None))
        except install.Stop as e:
            q.put(('stopped', (e.kind, str(e), getattr(e, 'code', None))))
        except Exception:
            q.put(('log', traceback.format_exc())); q.put(('stopped', ('unexpected', 'Unexpected error (details in the log).', None)))

    def event(self, kind, value):
        if kind == 'plan':
            self.model = progress.ProgressModel(list(value))
            if self.screen == 'progress': self.render()
            return
        if kind == 'log':
            self.lines.append(value); del self.lines[:-2000]
            if self.model: self.model.feed(value)
            return
        if kind == 'step':
            i, n, title, name, skipped = value
            if not self.model: self.model = progress.ProgressModel([name])
            self.model.step(name, skipped); self.step_title = self.phase_title(name); self.slow = ''
            if self.screen == 'progress': self.update_progress(full=True)
            return
        if kind == 'status':
            self.slow = value
            if self.screen == 'progress': self.update_progress()
            return
        if kind == 'done':
            self.worker = None
            if self.model: self.model.complete()
            self.finished()
            return
        if kind == 'stopped':
            self.worker = None
            self.stopped(*value)

    def phase_title(self, name):
        return next((t for _, t, steps in PHASES if name in steps), 'Installing')

    def phases(self):
        m = self.model
        if not m: return []
        out = []
        for pid, title, steps in PHASES:
            ss = [s for s in steps if s in m.status]
            if not ss: continue
            sts = [m.status[s] for s in ss]
            st = 'failed' if 'failed' in sts else 'active' if 'active' in sts else 'done' if all(x in ('done', 'kept') for x in sts) else \
                 'active' if any(x in ('done', 'kept') for x in sts) else 'pending'
            out.append((title, st))
        return out

    def overall(self):
        """0..1 over the core and the extras together (never goes back)."""
        m = self.model
        if not m: return 0.0
        tot = got = 0.0
        for s in m.steps:
            w = progress.STEP_SECONDS.get(s) or EXTRA_SECONDS.get(s) or 1
            st = m.status.get(s)
            if st in ('done', 'kept'): f = 1.0
            elif st == 'active':
                if s in EXTRA_SECONDS: f = m.sub.get(s, min(0.9, (m.clock() - m.start_at.get(s, m.clock())) / float(w)))
                else: f = m._in_step(s)
            else: f = 0.0
            tot += w; got += w * f
        self.shown_frac = max(self.shown_frac, got / tot if tot else 0.0)
        return self.shown_frac

    def eta(self):
        """Seconds left, shown only once measured, never going up (it stretches instead)."""
        m = self.model
        if not m: return None
        e = m.eta()
        if e is None: return None
        cur = m.current
        if cur not in EXTRA_SECONDS:
            e += sum(EXTRA_SECONDS[s] * (m.pace() or 1.0) for s in m.steps if s in EXTRA_SECONDS and m.status.get(s) == 'pending')
        if self.shown_eta is None or e < self.shown_eta[0] - (time.monotonic() - self.shown_eta[1]):
            self.shown_eta = (e, time.monotonic())
        left = self.shown_eta[0] - (time.monotonic() - self.shown_eta[1])
        return max(left, 30.0)

    def scr_progress(self):
        k, dp, cv = self.k, self.k.dp, self.cv
        tools = self.model is None
        frac = self.overall() if not tools else 0.0
        self.prog['pct'] = self.T(40, 112, '' if tools else '%d%%' % int(frac * 100), 'pct', 'amber', anchor='sw')
        tx = 40 if tools else 40 + k.fonts['pct'].measure('%d%%' % int(frac * 100)) / k.scale + 22
        self.prog['title'] = self.T(tx, 101, (self.step_title or 'Installing') + '\u2026' if not self.step_title.endswith('\u2026') else self.step_title, 'h1m', anchor='sw')
        y = 120
        cv.create_rectangle(dp(40), dp(y), dp(840), dp(y + 26), fill=P['panel'], outline=P['steel'], width=k.line(), tags='s')
        self.prog['fill'] = cv.create_rectangle(dp(43), dp(y + 3), dp(43), dp(y + 23), fill=P['amber'], outline='', tags='s')
        self.prog['scan'] = cv.create_rectangle(dp(43), dp(y + 3), dp(48), dp(y + 23), fill=P['amber_hover'], outline='', tags='s',
                                                state='normal' if self.anim else 'hidden')
        self.prog['note'] = self.T(40, 170, '', 'body', 'muted', anchor='w', width=560)
        self.prog['eta'] = self.T(840, 170, '', 'mono', 'text', anchor='e')
        if not tools:
            dl = self.link('details', ('\u25be Hide details' if self.show_details else '\u25b8 Show details'), self.toggle_details, quiet=True)
            self.put(dl, 36, 196)
            if self.show_details:
                ph = self.phases()
                hh = 18 + 22 * len(ph) + 30
                cv.create_rectangle(dp(40), dp(230), dp(840), dp(230 + hh), fill=P['panel'], outline=P['line'], width=k.line(), tags='s')
                self.prog['steps'] = []
                for i, (t, st) in enumerate(ph):
                    it = self.T(56, 240 + 22 * i, '', 'small', 'muted')
                    self.prog['steps'].append(it)
                self.put(self.link('openlog', 'Open log', self.open_log, quiet=True, bg='panel', small=True), 52, 236 + 22 * len(ph))
        if self.confirm == 'cancel': self.confirm_footer()
        elif self.tools_busy: self.footer([], [self.link('cancel', 'Cancel', self.cancel_tools, quiet=True, bg='panel')])
        elif tools: self.footer([], [])
        else: self.footer([], [self.link('cancel', 'Cancel', lambda: self.ask('cancel'), quiet=True, bg='panel')])
        self.update_progress(full=True)

    def toggle_details(self):
        self.show_details = not self.show_details; self.want_focus = 'details'; self.render()

    def cancel_tools(self):
        self.tools_cancel = True; self.set_slow('Stopping (Microsoft\'s installer keeps running on its own)')

    def update_progress(self, full=False):
        if self.screen != 'progress' or not self.prog: return
        cv, k, dp = self.cv, self.k, self.k.dp
        m = self.model
        if m:
            frac = self.overall()
            cv.itemconfigure(self.prog['pct'], text='%d%%' % int(frac * 100))
            cv.coords(self.prog['title'], dp(40) + k.fonts['pct'].measure('%d%%' % int(frac * 100)) + dp(22), dp(101))
            x1 = dp(43) + (dp(837) - dp(43)) * frac
            cv.coords(self.prog['fill'], dp(43), dp(123), x1, dp(143))
            self.scan_end = x1
        else:
            frac = None; self.scan_end = dp(837)
        title = self.step_title if self.step_title.endswith('\u2026') else self.step_title + '\u2026'
        cv.itemconfigure(self.prog['title'], text=title)
        slow = self.slow[:1].upper() + self.slow[1:] if self.slow else ''          # the engine's own phrase ("taking longer: low memory, ...")
        cv.itemconfigure(self.prog['note'], text=slow or 'You can keep using your PC.')
        e = self.eta() if m else None
        cv.itemconfigure(self.prog['eta'], text=fmt_left(e) if e is not None else ('Working out the time left\u2026' if m else ''))
        if self.prog.get('steps'):
            for it, (t, st) in zip(self.prog['steps'], self.phases()):
                cv.itemconfigure(it, text={'done': '\u2713 ', 'active': '\u25b8 ', 'failed': '\u2715 '}.get(st, '\u25cb ') + t,
                                 fill=P['ok'] if st == 'done' else P['amber'] if st == 'active' else P['danger'] if st == 'failed' else P['muted'])
        if full or self._tick % 10 == 0: self.update_title(frac)
        if frac is not None: self.taskbar.set('normal', frac)

    def animate(self):
        """The scan line along the filled part of the bar, and the hexes it passes light up (off with Windows' animation effects)."""
        if self.screen != 'progress' or not self.prog or not self.anim: return
        try:
            if self.root.state() == 'iconic': return
        except tk.TclError:
            return
        dp = self.k.dp
        end = getattr(self, 'scan_end', dp(43))
        if end - dp(43) < dp(12): return
        self.scan_x = (self.scan_x + dp(6)) if self.scan_x + dp(6) < end - dp(5) else dp(43)
        self.cv.coords(self.prog['scan'], self.scan_x, dp(123), self.scan_x + dp(5), dp(143))
        for it, cx, cy in self.hexes:
            lit = abs(cx - self.scan_x) < dp(30) and dp(60) < cy < dp(200)
            col = P['hexlit'] if lit else P['hexline']
            if self.cv.itemcget(it, 'outline') != col: self.cv.itemconfigure(it, outline=col)

    def update_title(self, frac=None):
        if self.screen == 'progress' and self.model and frac is not None:
            t = '%d%% \u00b7 %s \u00b7 %s' % (int(frac * 100), self.step_title.rstrip('\u2026') + '\u2026', TITLE)
        else:
            t = TITLE
        if self.root.title() != t: self.root.title(t)

    def cancel_install(self):
        self.confirm = None
        if self.inst: threading.Thread(target=self.inst.cancel, daemon=True).start()
        self.set_slow('Stopping\u2026'); self.render()

    # -- the end of a run
    def finished(self):
        self.taskbar.set('none')
        self.dest_installed = precheck.clean_path(self.dest)
        self.go('done', focus='primary')
        self.attention()

    def stopped(self, kind, msg, code=None):
        self.failed = (kind, msg)
        if self.model: self.model.fail(self.inst.stage if self.inst else None)
        if kind == 'cancelled':
            self.taskbar.set('none')
            self.mode = self.mode_for(self.dest); self.check_dest()
            self.notice = ('info', 'Install paused. Finished steps are kept: press Continue when you\'re ready.') if self.mode == 'resume' else ('info', 'Install cancelled.')
            return self.go('setup')
        if kind == 'extras':
            self.extras_failed = msg
            return self.finished()
        if kind == 'compiler' and code == 'reboot':
            return self.show_restart()
        if kind == 'compiler':
            self.facts['compiler'] = None
            self.waiting('Checking Microsoft\'s C++ tools\u2026')
            return self.bg(install.compiler_state, lambda c: self.after_compiler(c) if not (isinstance(c, dict) and c.get('state') == 'ready') else self.show_failed(kind, msg))
        if kind == 'disc':
            self.taskbar.set('paused', self.shown_frac)
            self.mode = self.mode_for(self.dest)
            self.image = precheck.ImageResult(False, 'disc', msg, 'The disc image could not be read')
            self.attention(); return self.go('setup', focus='zone' if self.mode == 'install' else 'disc_change')
        if kind == 'disk_full':
            drive = os.path.splitdrive(precheck.clean_path(self.dest))[0] or 'the install drive'
            return self.show_stop(style='needs', icon='needs', kicker='ACTION NEEDED', head='Free up space on %s' % drive,
                                  body='There isn\'t enough free space on %s to carry on. Free up some space, then press Try again. Finished steps are kept.' % drive,
                                  primary=('TRY AGAIN', self.resume), close=True, details=msg)
        self.show_failed(kind, msg)

    def show_failed(self, kind, msg):
        head, body = FAILED.get(kind, FAILED['unexpected'])
        if kind == 'network' and msg and 'couldn\'t verify the secure connection' in msg:     # TLS: the internet itself works
            head = 'Windows couldn\'t verify the download site'
            body = ('Windows couldn\'t verify the secure connection to the download site. The installer already tried Windows\' own '
                    'connection. Details says how to get the file without this download (on another PC, for example).')
        elif kind == 'network' and msg:          # the download's own one-line cause (A4), then what to do
            first = msg.split('. ')[0].strip().rstrip('.')
            if 0 < len(first) <= 170: body = first + '. Check your connection, then try again.'
        self.show_stop(style='failed', icon='bad', kicker='COULDN\'T FINISH', head=head, body=body, primary=('TRY AGAIN', self.resume),
                       details=msg, help=True)

    def resume(self):
        """Try again after a stop: Continue with the finished steps kept (1.1.3's Resume)."""
        self.mode = self.mode_for(self.dest)
        self.start()

    def show_stop(self, style, icon, kicker, head, body, primary=None, links=(), details='', close=False, help=False, note='', back=False):
        self.stop = dict(style=style, icon=icon, kicker=kicker, head=head, body=body, primary=primary, links=list(links),
                         details=details, close=close, help=help or style == 'failed', note=note, back=back)
        if style == 'failed': self.taskbar.set('error', self.shown_frac or 1.0)
        else: self.taskbar.set('paused', self.shown_frac or 1.0)
        self.go('stop', focus='primary')
        self.attention()

    def show_restart(self):
        self.show_stop(style='needs', icon='restart', kicker='ACTION NEEDED', head='Restart to finish setting up',
                       body='Windows needs to restart before setup can continue. Setup carries on by itself after you sign in.',
                       primary=('RESTART NOW', self.restart_now), close=True, note='Save your work in other apps first.')

    def scr_stop(self):
        s = self.stop or {}
        k, dp = self.k, self.k.dp
        col = 'danger' if s.get('style') == 'failed' else 'amber'
        ui_kit.icon(self.cv, s.get('icon', 'needs'), dp(40 + 30), dp(56 + 34), dp(30), 's')
        self.kicker(126, 60, s.get('kicker', ''), col)
        self.T(126, 88, s.get('head', ''), 'h1', width=700)
        self.T(126, 136, s.get('body', ''), 'body', 'muted', width=620)
        x = 122
        for i, (label, fn) in enumerate(s.get('links') or []):
            l = self.link('stop_link%d' % i, label, fn); self.put(l, x, 216); x += l.winfo_reqwidth() / k.scale + 18
        left = []
        if s.get('back'): left.append(self.link('back', '\u2039 Back', lambda: self.go('setup'), quiet=True, bg='panel'))
        if s.get('help'): left.append(self.link('help', 'Help', self.open_menu, quiet=True, bg='panel'))
        if s.get('details'): left.append(self.link('stop_details', 'Details', lambda: self.show_page('details'), quiet=True, bg='panel'))
        if s.get('note'): left.append(('text', s['note'], 'small', 'muted'))
        right = []
        if s.get('close'): right.append(self.link('close', 'Close', self.close_for_later, quiet=True, bg='panel'))
        if s.get('primary'): right.append(self.button('primary', s['primary'][0], s['primary'][1]))
        self.footer(left, right)

    def close_for_later(self):
        if self.stop and self.stop.get('icon') == 'restart': self.save_resume(); self.set_resume_on_signin()
        self.close()

    # -- restart and carry on after sign-in
    def marker(self):
        return precheck.local_dir() / 'installer-resume.json'

    def save_resume(self):
        try:
            d = dict(disc=self.disc, dest=self.dest, texture=self.texture, videos=self.videos, sc=self.sc, awake=self.awake, lowprio=self.lowprio,
                     merge=self.merge, switch=self.switch, time=time.time())
            self.marker().parent.mkdir(parents=True, exist_ok=True)
            self.marker().write_text(json.dumps(d), encoding='utf-8')
        except OSError as e:
            print('ui: could not save the resume note: %s' % e, flush=True)

    def read_resume_marker(self):
        try:
            p = self.marker()
            if not p.is_file(): return None
            d = json.loads(p.read_text(encoding='utf-8')); p.unlink()
            return d if time.time() - d.get('time', 0) < 7 * 86400 else None
        except (OSError, ValueError):
            return None

    def resume_after_restart(self, d):
        print('ui: carrying on after the restart', flush=True)
        self.clear_signin()
        self.texture, self.videos = d.get('texture', 'original'), bool(d.get('videos'))
        self.sc.update(d.get('sc') or {}); self.awake = d.get('awake', True); self.lowprio = d.get('lowprio', False)
        self.merge = d.get('merge', True); self.switch = d.get('switch', True)
        if d.get('dest'): self.set_dest(d['dest'])
        if d.get('disc'): self.disc = d['disc']; self.image = precheck.validate_image(d['disc'], install.RELEASE)
        self.facts['compiler'] = None
        if not self.needs_disc() or (self.image and self.image.ok): self.begin()
        else: self.render()

    def set_resume_on_signin(self):
        """The installer exe opens again once at the next sign-in (the engine's RunOnce entry; the window then finds the resume note).
        Tests replace this."""
        exe = os.environ.get('MA2_INSTALLER_EXE')
        if not exe: print('ui: no installer exe path; setup will not start by itself after sign-in', flush=True); return False
        ok = install.schedule_resume_after_restart('"%s"' % exe)
        if not ok: print('ui: could not add the sign-in entry; the player starts the installer again', flush=True)
        return ok

    def clear_signin(self):
        """Tests replace this."""
        try: install.clear_resume_after_restart()
        except Exception: pass

    def restart_windows(self):
        ui_win.restart_windows()

    def restart_now(self):
        self.save_resume(); self.set_resume_on_signin()
        try: self.restart_windows()
        except Exception as e: print('ui: restart failed: %s' % e, flush=True)

    # -- Done
    def shortcut_line(self):
        st = getattr(self.inst, 'state', None) or {}
        if 'shortcuts' not in st: return ''
        made = [Path(p) for p in st.get('shortcuts') or [] if 'Editor' not in Path(p).name]
        asked = any(self.sc.get(x) for x in ('desktop', 'start')) and self.action() in ('install', 'update', 'repair', 'resume')
        try: desk = install.known_folder('desktop')
        except Exception: desk = None
        on_desk = any(desk and p.parent == Path(desk) for p in made)
        in_start = any(not (desk and p.parent == Path(desk)) for p in made)
        if on_desk and in_start: return 'Shortcuts are on your desktop and in the Start menu.'
        if on_desk: return 'A shortcut is on your desktop.'
        if in_start: return 'You\'ll find it in the Start menu.'
        return 'Find it with Open install folder.' if asked else ''

    def scr_done(self):
        k, dp = self.k, self.k.dp
        ui_kit.emblem(self.cv, k, dp(40), dp(28), 's')
        self.kicker(40, 262, 'INSTALLED', 'ok')
        self.T(40, 286, 'Ready to play', 'h1l')
        act = self.action()
        body = []
        if act in ('update', 'repair'): body.append('Updated to %s. Your settings and saves were kept.' % install.PUBLIC_VERSION)
        elif act == 'addons': body.append('The new HD content is installed.')
        sl = self.shortcut_line()
        if sl: body.append(sl)
        if body: self.T(40, 336, ' '.join(body), 'body', 'muted', width=620)
        notes = list(getattr(self.inst, 'notes', []) or [])
        if self.extras_failed:
            msg = self.extras_failed
            if msg.startswith('MechAssault 2 is installed. '): msg = msg[len('MechAssault 2 is installed. '):]      # the headline says it
            first = msg.split('. ')[0].split(': ')[0].rstrip('.') + '.'
            for a, b in (('2x enhanced textures', 'HD textures'), ('4x enhanced textures', 'Ultra HD textures'), ('enhanced cutscene videos', 'upscaled movies')):
                first = first.replace(a, b)
            self.strip(372, first + ' You can play now and try again later.',
                       links=[self.link('retry_extras', 'Try again', self.resume, bg='panel2'), self.link('notes', 'Details', lambda: self.show_page('notes'), bg='panel2')], width=800)
        elif notes:
            self.strip(372, notes[0] if len(notes[0]) < 160 else notes[0][:157] + '\u2026',
                       links=[self.link('notes', 'Details', lambda: self.show_page('notes'), bg='panel2')], width=800)
        of = self.link('open_folder', 'Open install folder', lambda: self.launch('open', precheck.clean_path(self.dest)), quiet=True, bg='panel')
        self.footer([], [of, self.button('primary', 'LAUNCH', self.play, big=True)])

    def play(self):
        exe = Path(precheck.clean_path(self.dest)) / 'MechAssault 2.exe'
        ok = self.launch('play', exe)
        if ok is False: return
        self.close()

    def launch(self, what, path):
        """Open a folder / select a file in Explorer / start the game. Tests replace this."""
        if what == 'select':
            try: subprocess.Popen(['explorer.exe', '/select,' + str(path)])
            except OSError: pass
        elif what == 'open': open_path(path)
        else:
            try: subprocess.Popen([str(path)], cwd=str(Path(path).parent), close_fds=True)
            except OSError as e:
                self.show_blocked(path, e); return False
        return True

    def show_blocked(self, path, e):
        code = getattr(e, 'winerror', None) or getattr(e, 'errno', None)
        name = Path(str(path)).name
        if code in (2, 225):
            body = ('Your antivirus removed %s from %s. If you trust it, restore it from your antivirus\'s protection history, '
                    'or run this installer again and choose Repair.' % (name, precheck.short(Path(str(path)).parent, 40)))
        else:
            body = ('Windows security or your antivirus stopped MechAssault 2 from starting. If you trust it, restore the game from your '
                    'security app\'s protection history, then try again.')
        self.show_stop(style='failed', icon='blocked', kicker='COULDN\'T START', head='Windows blocked the game', body=body,
                       primary=('TRY AGAIN', self.play), details=install.launch_advice(path, e), help=True)

    # -- text pages (Help & FAQ, Legal, Details, notes)
    def show_page(self, which):
        self.page_back = self.screen if self.screen != 'page' else getattr(self, 'page_back', 'setup')
        self.go('page', page=which)

    def page_text(self, which):
        if which == 'legal':
            fonts = ''
            try: fonts = (HERE / 'data' / 'fonts' / 'FONTS.txt').read_text(encoding='utf-8')
            except OSError: pass
            return 'LEGAL', [('', precheck.FAN_LINE), ('Your copy', 'You need your own copy of the game. The installer carries no game files.'),
                             ('Version', install.build_label()), ('Fonts', fonts.strip())]
        if which == 'help':
            return 'HELP & FAQ', [(t, TECH if b is None else b % precheck.support_where() if '%s' in b else b) for t, b in HELP_TEXT]
        if which == 'notes':
            out = []
            if self.extras_failed: out.append(('Not finished', self.extras_failed))
            for n in getattr(self.inst, 'notes', []) or []: out.append(('', n))
            return 'NOTES', out
        s = self.stop or {}
        tail = '\n'.join(self.lines[-40:])
        return 'DETAILS', [('', s.get('details') or (self.failed[1] if self.failed else ''))] + ([('Last lines of the log', tail)] if tail else [])

    def scr_page(self):
        k, dp, cv = self.k, self.k.dp, self.cv
        cv.create_rectangle(0, 0, dp(self.W), dp(self.H), fill=P['panel'], outline='', tags='s')
        title, parts = self.page_text(self.page)
        self.T(40, 22, title, 'h1s')
        f = tk.Frame(cv, bg=P['panel'])
        txt = tk.Text(f, bg=P['panel'], fg=P['text'], font=k.fonts['small'], bd=0, highlightthickness=0, wrap='word',
                      padx=0, pady=0, spacing1=dp(2), spacing3=dp(2), insertbackground=P['panel'], cursor='arrow', takefocus=1)
        sb = ttk.Scrollbar(f, orient='vertical', command=txt.yview, style='Dark.Vertical.TScrollbar'); txt['yscrollcommand'] = sb.set
        txt.tag_configure('h', font=k.fonts['bold'], foreground=P['text'], spacing1=dp(8))
        txt.tag_configure('b', foreground=P['text'], font=k.fonts['body'])
        txt.tag_configure('mono', font=k.fonts['mono_s'], foreground=P['muted'])
        for head, body in parts:
            if head: txt.insert('end', head + '\n', 'h')
            txt.insert('end', (body or '') + '\n', 'mono' if head == 'Last lines of the log' else 'b')
        txt.configure(state='disabled')
        sb.pack(side='right', fill='y'); txt.pack(side='left', fill='both', expand=True)
        cv.create_window(dp(40), dp(66), window=f, anchor='nw', width=dp(800), height=dp(370), tags='s')
        txt.key = 'pagetext'; self.w['pagetext'] = txt
        left = [self.link('back', '\u2039 Back', lambda: self.go(self.page_back), quiet=True, bg='panel')]
        if self.page == 'details':
            left.append(self.link('copy', 'Copy', self.copy_details, quiet=True, bg='panel'))
            left.append(self.link('openlog', 'Open log folder', self.open_logs, quiet=True, bg='panel'))
        self.footer(left, [self.button('done', 'DONE', lambda: self.go(self.page_back))])

    def copy_details(self):
        inst = self.inst
        text = precheck.support_bundle(self.lines, (self.failed and inst and getattr(inst, 'stage', '')) or '', True, install.PUBLIC_VERSION, install.BUILD,
                                       self.facts.get('gpus') or [], (self.facts.get('compiler') or {}).get('vs') or '')
        self.root.clipboard_clear(); self.root.clipboard_append(text)

    def open_logs(self):
        d = precheck.logs_dir()
        try: d.mkdir(parents=True, exist_ok=True)
        except OSError: pass
        self.launch('open', d)

    def open_log(self):
        log = getattr(self.inst, 'session_log', None)
        if log and Path(log).is_file(): self.launch('open', log)
        else: self.open_logs()

    # -- Help menu
    def open_menu(self):
        self.menu = True; self.want_focus = 'm_help'; self.render()

    def draw_menu(self):
        k, dp = self.k, self.k.dp
        f = tk.Frame(self.cv, bg=P['panel2'], highlightthickness=k.line(), highlightbackground=P['steel'])
        items = [('m_help', 'Help & FAQ', lambda: self.menu_do(lambda: self.show_page('help'))),
                 ('m_diag', 'Collect diagnostics\u2026', lambda: self.menu_do(self.open_diag)),
                 ('m_log', 'Open log', lambda: self.menu_do(self.open_log))]
        ws = []
        for key, label, fn in items:
            l = Link(f, k, label, fn, P['panel2'], quiet=False, key=key)
            l.f = l.fu = k.fonts['body']; l.configure(width=dp(220)); l.colour = lambda l=l: P['text']
            l.pack(fill='x', padx=dp(4), pady=(dp(2), 0)); l.draw(); ws.append(l); self.w[key] = l
            l.bind('<Up>', lambda e: (e.widget.tk_focusPrev().focus_set(), 'break')[1])
            l.bind('<Down>', lambda e: (e.widget.tk_focusNext().focus_set(), 'break')[1])
            l.bind('<FocusOut>', lambda e: self.root.after(50, self.menu_focus_check), add='+')
        tk.Frame(f, bg=P['panel2'], height=dp(4)).pack()
        self.cv.create_window(dp(36), dp(self.H - 58), window=f, anchor='sw', tags='s')

    def menu_focus_check(self):
        if not self.menu: return
        fw = self.focus_now()
        if fw is None or not getattr(fw, 'key', '').startswith('m_'):
            self.menu = None; self.render()

    def menu_do(self, fn):
        self.menu = None; fn()
        if self.menu is None and self.screen not in ('page', 'diag'): self.render()

    # -- Collect diagnostics (in the window; the zip goes to LocalAppData, never into guarded folders)
    def open_diag(self):
        self.diag_back = self.screen if self.screen not in ('diag', 'page') else 'setup'
        self.diag = {'dump': False, 'rec': False, 'result': None, 'busy': False, 'preview': False}
        self.go('diag')

    def scr_diag(self):
        k, dp, cv = self.k, self.k.dp, self.cv
        cv.create_rectangle(0, 0, dp(self.W), dp(self.H), fill=P['panel'], outline='', tags='s')
        self.T(40, 22, 'COLLECT DIAGNOSTICS', 'h1s')
        self.T(40, 62, 'Saves one .zip for a problem report: basic facts about this PC, the install record, your settings and the newest '
                       'installer and game logs. Your user name is replaced in every text file. It is saved in AppData\\Local\\MechAssault2\\Diagnostics '
                       'on this PC and never sent anywhere.', 'body', 'muted', width=780)
        try: extras = precheck.diagnostics_extras()
        except Exception: extras = {}
        y = 120
        d = self.diag
        if extras.get('dump'):
            self.T(40, y + 14, 'Include the newest crash or hang dump (it can contain folder names and your user name)', 'small', 'text', anchor='w')
            self.put(Toggle(cv, k, d['dump'], lambda v: d.__setitem__('dump', v), P['panel'], key='d_dump'), 840, y + 14, anchor='e'); y += 36
        if extras.get('botrec'):
            self.T(40, y + 14, 'Include the newest bot match recording', 'small', 'text', anchor='w')
            self.put(Toggle(cv, k, d['rec'], lambda v: d.__setitem__('rec', v), P['panel'], key='d_rec'), 840, y + 14, anchor='e'); y += 36
        self.put(self.link('d_preview', ('\u25be ' if d['preview'] else '\u25b8 ') + 'What goes in the zip', self.toggle_preview, quiet=True, bg='panel'), 36, y + 4)
        y += 36
        if d['preview']:
            self.T(52, y, 'recomp-install.json (what is installed where)\npc_settings.ini (your settings)\nthe newest three game logs and the newest installer logs\n'
                          'system.txt: Windows version, graphics cards, screen size, the C++ tools found, how far an unfinished install got'
                          + ('\nthe last lines this window showed and the step that stopped' if self.failed else ''), 'small', 'muted', width=760)
            y += 100
        r = d.get('result')
        if d['busy']: self.T(40, y + 10, 'Collecting\u2026', 'body', 'text')
        elif isinstance(r, Exception): self.strip(y, 'Couldn\'t collect diagnostics: %s' % r, bad=True, width=800)
        elif r:
            self.strip(y, 'Saved %s in %s. Attach it to your problem report (%s).' % (Path(r['path']).name, precheck.short(Path(r['path']).parent, 44), precheck.support_where()),
                       links=[self.link('d_show', 'Show file', lambda: self.launch('select', r['path']), bg='panel2')], width=800)
        back = self.link('back', '\u2039 Back', lambda: self.go(self.diag_back), quiet=True, bg='panel')
        prim = self.button('primary', 'DONE', lambda: self.go(self.diag_back)) if r and not isinstance(r, Exception) else \
               self.button('primary', 'COLLECT', self.collect, enabled=not d['busy'])
        self.footer([back], [prim])

    def toggle_preview(self):
        self.diag['preview'] = not self.diag['preview']; self.want_focus = 'd_preview'; self.render()

    def collect(self):
        d = self.diag; d['busy'] = True; d['result'] = None; self.render()
        dest = precheck.remembered_install() or precheck.clean_path(self.dest) or precheck.default_install_folder()
        stage = (self.failed and self.inst and getattr(self.inst, 'stage', '')) or ''
        failure = '%s: %s' % self.failed if self.failed else ''
        lines = list(self.lines); dump, rec = d['dump'], d['rec']
        out = Path(precheck.diagnostics_dir()) if hasattr(precheck, 'diagnostics_dir') else precheck.local_dir() / 'Diagnostics'
        def work():
            out.mkdir(parents=True, exist_ok=True)
            return precheck.collect_diagnostics(dest, out, install.PUBLIC_VERSION, install.BUILD, self.facts.get('gpus'), dump, rec, stage, failure, lines)
        def done(res):
            d['busy'] = False; d['result'] = res
            if not isinstance(res, Exception): self.launch('select', res['path'])
            if self.screen == 'diag': self.want_focus = 'primary'; self.render()
        self.bg(work, done)

    # -- Uninstall
    def scr_uninstall(self):
        k, dp, cv = self.k, self.k.dp, self.cv
        cv.create_rectangle(0, 0, dp(self.W), dp(self.H), fill=P['panel'], outline='', tags='s')
        self.T(40, 22, 'UNINSTALL', 'h1s')
        self.T(40, 62, 'Removes the game folder, its shortcuts and its caches. This can\'t be undone. Your own scenarios in '
                       'Documents\\MechAssault 2\\Scenarios are never deleted.', 'body', 'text', width=780)
        self.wipe = getattr(self, 'wipe', False)
        dl, dc = self.drive_line()
        self.row(120, 'Installed in', short_path(self.dest, 52), '', 'muted', None, first=True)
        self.T(40, 196, 'Also delete my settings and saves', 'row', anchor='w')
        self.put(Toggle(cv, k, self.wipe, lambda v: setattr(self, 'wipe', v), P['panel'], key='u_wipe'), 840, 196, anchor='e')
        if getattr(self, 'uninstalling', False): self.T(40, 240, 'Uninstalling\u2026', 'body')
        back = self.link('back', '\u2039 Back', lambda: self.go('setup'), quiet=True, bg='panel')
        b = self.button('primary', 'UNINSTALL', self.do_uninstall, enabled=not getattr(self, 'uninstalling', False))
        b.danger = True; b.draw()
        self.footer([back], [b])

    def do_uninstall(self):
        d = precheck.clean_path(self.dest); wipe = self.wipe
        self.uninstalling = True; self.render()
        def done(res):
            self.uninstalling = False; self.wipe = False
            if isinstance(res, Exception): self.notice = ('bad', 'The uninstall stopped: %s' % res)
            else: self.notice = ('info', res['text'])
            self.mode = self.mode_for(d)
            if self.mode == 'install': self.dest = precheck.default_install_folder(); self.texture = 'original'; self.videos = False
            self.check_dest(); self.go('setup')
        self.bg(lambda: install.uninstall(d, wipe), done)

    # -- a waiting / progress screen without a model uses scr_progress; attention when something needs the player
    def attention(self):
        """Bring a minimised window back and flash the taskbar button when a stop or Done appears with nobody looking."""
        try:
            if self.root.state() == 'iconic': self.root.deiconify()
        except tk.TclError:
            pass
        if not ui_win.is_foreground(self.hwnd): ui_win.flash(self.hwnd)

    # -- event pump
    def pump(self):
        try:
            for _ in range(400):
                kind, value = self.q.get_nowait()
                if kind == 'call': value()
                else: self.event(kind, value)
        except queue.Empty:
            pass
        except Exception:
            traceback.print_exc()
        try:
            for p in self.drop.take(): self.on_drop(p)
        except Exception as e:
            ui_win.degrade('drop', e)
        self._tick += 1
        if self.screen == 'progress' and self.model and self._tick % 5 == 0: self.update_progress()
        if self._tick % 5 == 0 and not self.forced_scale: self.check_dpi()
        self._after = self.root.after(100, self.pump)
        if self.anim and self.screen == 'progress': self.animate()

    def on_drop(self, path):
        print('ui: a file was dropped on the window', flush=True)
        if self.screen not in ('setup', 'options') or self.worker: return
        if self.mode == 'install' or self.needs_disc():
            self.screen = 'setup'; self.set_disc(path)

    def check_dpi(self):
        d = ui_win.window_dpi(self.hwnd)
        if d and self.dpi_seen and d != self.dpi_seen:
            self.dpi_seen = d
            s = self.pick_scale()
            if abs(s - self.k.scale) > 0.01: self.rescale(s)

    def stop_pump(self):
        try: self.root.after_cancel(self._after)
        except (tk.TclError, AttributeError): pass

    def close(self):
        if self.worker and self.worker.is_alive():
            self.attention()
            if self.screen == 'progress': self.ask('cancel')
            return
        self.closing = True
        try: self.taskbar.set('none')
        except Exception: pass
        self.drop.remove()
        ui_theme.unload_fonts()
        self.stop_pump()
        self.root.destroy()

# ---------------------------------------------------------------------------- start-up

def message_box(text):
    """A plain Windows message box (the window may not exist yet). Tests replace this."""
    try: ctypes.windll.user32.MessageBoxW(None, text, TITLE, 0x10)
    except Exception: pass

def open_ui_log():
    """%LOCALAPPDATA%\\MechAssault2\\Logs\\installer-ui-<time>.log; becomes stdout and stderr (pythonw has neither), so a crash leaves a trace."""
    try:
        d = Path(os.environ.get('LOCALAPPDATA') or str(Path.home() / 'AppData' / 'Local')) / 'MechAssault2' / 'Logs'
        d.mkdir(parents=True, exist_ok=True)
        for f in sorted(d.glob('installer-ui-*.log'), key=lambda f: f.stat().st_mtime)[:-9]:
            try: f.unlink()
            except OSError: pass
        path = d / ('installer-ui-%s.log' % time.strftime('%Y%m%d-%H%M%S'))
        return path, open(path, 'a', encoding='utf-8', buffering=1)
    except OSError:
        return None, None

_KEEP = []

def start_new():
    """The new window. Raises if it cannot be built (main() then opens the classic one)."""
    if IMPORT_ERROR: raise RuntimeError('a part of the new window failed to load:\n' + IMPORT_ERROR)
    if os.environ.get('MA2_UI_CRASH_TEST'): raise RuntimeError('MA2_UI_CRASH_TEST: simulated start-up failure')
    if ui_win.faulted('startup'): raise RuntimeError('injected fault (MA2_UI_FAULT=startup)')
    if os.environ.get('MA2_UI_LEGACY'): raise RuntimeError('MA2_UI_LEGACY is set: the classic window was asked for')
    ui_win.set_dpi_awareness()
    ui_win.set_app_id(APP_ID)
    ui_theme.load_fonts()
    root = tk.Tk()
    _KEEP.append(root)          # never let a background check drop the last reference to Tk (Tcl must be deleted on this thread)
    try:
        root.withdraw()
        app = App(root)
        root.deiconify()
        return root, app
    except BaseException:
        try: root.destroy()
        except Exception: pass
        raise

def start_legacy():
    """The classic 1.1.3 window (gui_legacy.py), when the new one could not start."""
    import gui_legacy
    gui_legacy.message_box = lambda text: message_box(text)
    gui_legacy.main()

def main():
    log_path, log = STARTUP_LOG if STARTUP_LOG[1] else open_ui_log()     # opened at the top of this file when run as the window
    if log: sys.stdout = sys.stderr = log
    try: print('ui: installer window %s, session %s' % (install.build_label(), precheck.session_id()), flush=True)
    except Exception: pass
    try:
        root, app = start_new()
    except Exception:
        traceback.print_exc()
        print('ui: the new installer window could not start; opening the classic window instead', flush=True)
        try:
            ui_theme.unload_fonts()
        except Exception:
            pass
        try:
            return start_legacy()
        except SystemExit:
            raise
        except Exception:
            traceback.print_exc()
            message_box('The installer could not start.\n\nDetails were saved to:\n%s' % (log_path or 'the installer could not write a log'))
            sys.exit(3)
    try:
        root.mainloop()
    except Exception:
        traceback.print_exc()
        message_box('The installer stopped unexpectedly.\n\nDetails were saved to:\n%s' % (log_path or 'the installer could not write a log'))
        sys.exit(3)
    if log and log_path and log_path.is_file() and log_path.stat().st_size == 0:      # nothing went wrong: no empty log left behind
        try: faulthandler.disable()
        except Exception: pass
        log.close()
        try: log_path.unlink()
        except OSError: pass

if __name__ == '__main__':
    main()
