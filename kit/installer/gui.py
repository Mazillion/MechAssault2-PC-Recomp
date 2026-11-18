"""The MechAssault 2 Recomp Installer window: a seven-page wizard (install.py does the work; precheck.py does the checks).

Welcome - Systems - Disc - Location - Loadout - Install - Done, plus a failure screen. Next stays disabled until the page is
valid and the reason shows in the status line; there are no modal errors (only "Stop installing?" asks).
"""
import ctypes, faulthandler, os, sys, time, traceback               # the standard library only, until the start-up log is open
from pathlib import Path

HERE = Path(__file__).resolve().parent
TITLE = 'MechAssault 2 Recomp Installer'
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

try:
    import queue, shutil, subprocess, threading
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    sys.path.insert(0, str(HERE))
    import install, precheck, progress, ui_theme                     # noqa: E402
    from ui_theme import C                                            # noqa: E402
except Exception:
    if __name__ != '__main__': raise
    traceback.print_exc()
    try: ctypes.windll.user32.MessageBoxW(None, 'The installer could not start (a part of it failed to load).\n\nDetails were saved to:\n%s'
                                          % (STARTUP_LOG[0] or 'the installer could not write a log'), TITLE, 0x10)
    except Exception: pass
    sys.exit(3)

try:
    ctypes.windll.shcore.SetProcessDpiAwareness(1)
except Exception:
    pass

PAGES = ['welcome', 'systems', 'disc', 'location', 'loadout', 'install', 'done']
RAIL = {'welcome': 'Welcome', 'systems': 'Systems', 'disc': 'Disc', 'location': 'Location', 'loadout': 'Loadout',
        'install': 'Install', 'done': 'Done'}
KICKER = {'welcome': ('PILOT BRIEFING', 'Build MechAssault 2 for PC'), 'systems': ('SYSTEMS CHECK', 'Checking this PC'),
          'disc': ('LOAD DISC', 'Choose your disc image'), 'location': ('DROP ZONE', 'Where to install'),
          'loadout': ('LOADOUT', 'Options'), 'install': ('POWERING UP', 'Building your PC version'),
          'done': ('MISSION COMPLETE', 'Ready to play')}
TECH = ('The installer reads your disc image, translates the original Xbox program to C with the open-source xboxrecomp toolkit, applies '
        'the PC release\'s fixes and features, compiles the game on this PC with Microsoft\'s C++ tools and builds its PC menus from '
        'your disc. The result is a ready-to-play folder. ' + precheck.NO_GAME_DATA + ' ' + precheck.SHADER_LINE)
SMARTSCREEN = ('Windows may have said "Windows protected your PC". That is SmartScreen: it warns about programs it does not recognise, and a new installer '
               'that is not code-signed is one of those. This installer is not code-signed (certificates cost money). If you downloaded it from where you '
               'expected, choose More info, then Run anyway. Windows Smart App Control, if it is on, can block an unsigned program outright, with no Run anyway '
               'choice; the installer never turns it off. Antivirus programs may also ask about it.')
FAILURES = {
    'cancelled': ('Install paused.', 'Run the installer again and choose Resume.'),
    'disk_full': ('The drive ran out of space.', 'Free up space on {drive} and press Resume.'),
    'compiler': ('The C++ Build Tools are missing or incomplete.', 'Open the Systems page for the fix, or install Visual Studio with "Desktop development with C++".'),
    'build_failed': ('Compiling the game failed.', 'The build stopped without naming a compiler error. Low memory, a damaged Build Tools install, or '
                     'antivirus removing a file can do this. Try Resume once; if it fails again, {report}.'),
    'compile_error': ('Compiling the game failed.', 'The compiler or linker reported an error (below, and in the log). If the install folder\'s '
                      'name has commas, brackets or other unusual characters, install to a simple folder such as C:\\Games\\MechAssault 2. '
                      'Otherwise try Resume once; if it fails again, {report}.'),
    'access_denied': ('Windows or your antivirus blocked a file.', 'If your antivirus reported a threat in this folder, restore the file or '
                      'allow the folder, then Resume. Controlled Folder Access can also cause this.'),
    'out_of_memory': ('The PC ran out of memory while compiling.', 'Close other programs and press Resume.'),
    'network': ('A download failed.', 'Check your connection and press Resume.'),
    'ffmpeg_mismatch': ('The FFmpeg files are not the expected build.', 'Use exactly the file named below (a different FFmpeg version '
                        'or a damaged download does not work), then press Resume.'),
    'gpu': ('PyTorch does not support this graphics card yet.', 'The enhanced content cannot be made on this PC; the game itself is not affected. Run the installer again without enhanced content.'),
    'disc': ('The disc image is damaged or could not be read.', 'Choose the image again (Choose disc image), or dump the disc again.'),
    'unexpected': ('Something unexpected went wrong.', 'Press Resume to try again. If it keeps happening, {report}.'),
    'extras': ('MechAssault 2 is installed and ready to play.',
               'Only the enhanced content was not finished. Close the installer and play, or press Resume to try the enhanced content again.'),
}
BUILD_TOOLS_NOTICE = ('Microsoft\'s free C++ Build Tools are needed to compile the game. "Install for me" downloads Microsoft\'s own installer (signed by Microsoft). '
                      'It asks for administrator approval and shows its own progress (about 6 GB, 10-30 minutes). By continuing you accept Microsoft\'s '
                      'licence terms for the Build Tools.')
BUILD_TOOLS_TERMS = 'https://visualstudio.microsoft.com/license-terms/'
EXPECTED = {'2x': ('2x Enhanced', 'AI-upscaled on your GPU', '+0.6 GB', 'several hours'),
            '4x': ('4x Enhanced', 'Sharpest', '+2.2 GB', 'longest')}

def open_path(p):
    try:
        os.startfile(str(p))
    except OSError:
        pass

class Page(tk.Frame):
    key = ''
    next_text = 'Next'
    back = True

    def __init__(self, app):
        super().__init__(app.body, bg=C['bg'])
        self.app, self.t = app, app.t
        self.build()

    def build(self): pass
    def on_show(self): pass
    def on_hide(self): pass
    def valid(self): return True, ''
    def kicker(self): return KICKER[self.key]

    def buttons(self, ok):
        i = [k for k in self.app.order() if PAGES.index(k) < PAGES.index(self.key)]
        out = []
        if self.back: out.append(('Back', self.app.go_back, 'TButton', bool(i)))
        out.append((self.next_text, self.app.go_next, 'Primary.TButton', ok))
        return out

    def heading(self, text, sub=None):
        self.t.label(self, text, 'h2', 'text').pack(anchor='w', pady=(0, 2))
        if sub: self.t.label(self, sub, 'body', 'muted', wraplength=self.app.wrap, justify='left').pack(anchor='w', pady=(0, 8))

class App:
    def __init__(self, root, theme):
        self.root, self.t = root, theme
        S = theme.S
        self.q = queue.Queue(); self.worker = None; self.inst = None
        self.launched = []                      # (what) -- tests replace launch()
        self.facts = {'gpus': None, 'reco': None, 'compiler': None}
        self.dest = tk.StringVar(); self.disc = tk.StringVar()
        self.action = tk.StringVar(value='install')          # install | update | repair | addons | resume
        self.texture = tk.StringVar(value='original'); self.videos = tk.BooleanVar()
        self.sc = {k: tk.BooleanVar(value=v) for k, v in (('desktop', True), ('start', True), ('editor', False))}
        self.awake = tk.BooleanVar(value=True); self.lowprio = tk.BooleanVar(); self.merge = tk.BooleanVar(value=True)
        self.switch = tk.BooleanVar(value=True); self.close_on_play = tk.BooleanVar(value=True)
        self.folder_report = None; self.image_result = None; self.started = None
        self.wrap = S(600)
        # a short screen (1366x768 at 125 %) gets the compact Welcome page
        self.compact = (root.winfo_screenheight() - S(90)) / theme.scale < 640
        root.title(TITLE)
        root.report_callback_exception = self.ui_error
        root.configure(bg=C['bg'])
        try: root.iconbitmap(default=str(HERE / 'data' / 'app.ico'))
        except tk.TclError: pass
        w, h = S(880), S(620)
        sw, sh = root.winfo_screenwidth(), root.winfo_screenheight()
        w, h = min(w, sw - S(20)), min(h, sh - S(60))
        off = os.environ.get('MA2_UI_OFFSCREEN')       # test rig: keep the window far off screen
        root.geometry('%dx%d+%s+%s' % (w, h, '-32000' if off else max(0, (sw - w) // 2), '-32000' if off else max(0, (sh - h) // 3)))
        root.minsize(min(S(780), w), min(S(560), h))
        self.layout()
        self.pages = {k: cls(self) for k, cls in (('welcome', Welcome), ('systems', Systems), ('disc', Disc), ('location', Location),
                                                  ('loadout', Loadout), ('install', Install), ('done', Done))}
        self.cur = None
        self.dest.set(precheck.remembered_install() or precheck.default_install_folder())
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.bind('<Return>', self.on_return)
        self._root_w = 0; root.bind('<Configure>', self.on_root_configure)
        self._after = root.after(100, self.pump)
        threading.Thread(target=self.detect_hardware, daemon=True).start()
        self.show('welcome')

    # -- window layout
    def layout(self):
        t, S, root = self.t, self.t.S, self.root
        root.columnconfigure(1, weight=1); root.rowconfigure(0, weight=1)
        self.rail = tk.Frame(root, bg=C['panel'], width=S(206)); self.rail.grid(row=0, column=0, sticky='ns'); self.rail.grid_propagate(False)
        tk.Frame(root, bg=C['line'], width=1).grid(row=0, column=0, sticky='nse')
        em = tk.Canvas(self.rail, width=S(206), height=S(104), bg=C['panel'], highlightthickness=0)
        em.pack(); ui_theme.draw_emblem(em, S(36), S(54), S(24), fill=C['panel2'])
        em.create_text(S(70), S(44), text='MECHASSAULT 2', anchor='w', font=t.fonts['rail'], fill=C['text'])
        em.create_text(S(70), S(64), text='RECOMP INSTALLER', anchor='w', font=t.fonts['kicker'], fill=C['amber'])
        self.rail_rows = {}
        for k in PAGES:
            row = tk.Frame(self.rail, bg=C['panel']); row.pack(fill='x', pady=S(1))
            bar = tk.Frame(row, bg=C['panel'], width=S(3)); bar.pack(side='left', fill='y')
            glyph = tk.Label(row, text='\u25cb', font=t.fonts['rail'], fg=C['steel'], bg=C['panel'], width=2); glyph.pack(side='left', padx=(S(10), 0), pady=S(6))
            name = tk.Label(row, text=RAIL[k], font=t.fonts['rail'], fg=C['muted'], bg=C['panel'], anchor='w'); name.pack(side='left')
            self.rail_rows[k] = (row, bar, glyph, name)
        self.fan = tk.Label(self.rail, text='Unofficial fan project.\nNot affiliated with Microsoft,\nXbox or the MechAssault /\nBattleTech rights holders.',
                            font=t.fonts['small'], fg=C['steel'], bg=C['panel'], justify='left', anchor='w')
        self.fan.pack(side='bottom', anchor='w', padx=S(14), pady=S(12))
        self.main = tk.Frame(root, bg=C['bg']); self.main.grid(row=0, column=1, sticky='nsew')
        self.main.columnconfigure(0, weight=1); self.main.rowconfigure(1, weight=1)
        head = tk.Frame(self.main, bg=C['bg'], height=S(92)); head.grid(row=0, column=0, sticky='ew'); head.grid_propagate(False)
        art = tk.Canvas(head, width=S(250), height=S(92), bg=C['bg'], highlightthickness=0); art.pack(side='right', fill='y')
        if not t.plain: ui_theme.draw_radar(art, S(250), S(92), t.scale)
        txt = tk.Frame(head, bg=C['bg']); txt.pack(side='left', padx=S(28), pady=S(14), fill='y')
        self.kicker_l = t.label(txt, '', 'kicker', 'amber', bg='bg'); self.kicker_l.pack(anchor='w')
        self.title_l = t.label(txt, '', 'h1', 'text', bg='bg'); self.title_l.pack(anchor='w')
        tk.Frame(self.main, bg=C['line'], height=1).grid(row=0, column=0, sticky='sew')
        self.body = tk.Frame(self.main, bg=C['bg']); self.body.grid(row=1, column=0, sticky='nsew', padx=S(28), pady=(S(12), 0))
        self.foot = tk.Frame(self.main, bg=C['bg']); self.foot.grid(row=2, column=0, sticky='ew', padx=S(28), pady=S(12))
        self.foot.columnconfigure(0, weight=1)
        self.status = t.label(self.foot, '', 'small', 'orange', bg='bg', anchor='w', justify='left', wraplength=S(380)); self.status.grid(row=0, column=0, sticky='w')
        self.btns = tk.Frame(self.foot, bg=C['bg']); self.btns.grid(row=0, column=1, sticky='e')
        self.wrap = S(612)

    # -- navigation
    def order(self):
        o = ['welcome', 'systems']
        if self.needs_disc(): o.append('disc')
        return o + ['location', 'loadout', 'install', 'done']

    def needs_disc(self):
        act = self.action.get()
        if act == 'addons': return False
        if act in ('update', 'repair') and self.game_files_ok(): return False
        if act == 'resume':           # an interrupted install that never finished extracting still needs the disc (a failed extract: choose it again)
            st = precheck.read_state(precheck.clean_path(self.dest.get())) or {}
            return (not st.get('addons_only') and not st.get('core_done') and 'extract' not in st.get('done', [])
                    and not (st.get('update') and self.game_files_ok()))
        return act in ('install', 'update', 'repair')

    def game_files_ok(self):
        try:
            d = Path(precheck.clean_path(self.dest.get()))
            xbe = d / 'game_files' / install.RELEASE['xbe']
            return xbe.is_file() and xbe.stat().st_size == install.RELEASE['xbe_bytes']
        except OSError:
            return False

    def show(self, key):
        if self.cur: self.cur.on_hide(); self.cur.pack_forget()
        self.cur = self.pages[key]
        k, ti = self.cur.kicker()
        self.kicker_l['text'], self.title_l['text'] = k, ti
        self.cur.pack(fill='both', expand=True)
        self.cur.on_show()
        self.update_rail()
        self.refresh()

    def go_next(self):
        cur = PAGES.index(self.cur.key)
        nxt = [k for k in self.order() if PAGES.index(k) > cur]
        if nxt: self.show(nxt[0])

    def go_back(self):
        cur = PAGES.index(self.cur.key)
        prev = [k for k in self.order() if PAGES.index(k) < cur]
        if prev: self.show(prev[-1])

    def on_return(self, event):
        if isinstance(event.widget, (tk.Text, ttk.Button, tk.Button)): return
        ok, _ = self.cur.valid()
        if ok and any(b[0] == self.cur.next_text and b[3] for b in self.cur.buttons(ok)): self.cur.buttons(ok)[-1][1]()

    def update_rail(self, fault=False):
        order = self.order(); cur = self.cur.key
        for k, (row, bar, glyph, name) in self.rail_rows.items():
            hidden = k not in order
            if hidden: row.pack_forget(); continue
            row.pack(fill='x', pady=self.t.S(1), before=self.fan) if not row.winfo_ismapped() else None
            i, ci = PAGES.index(k), PAGES.index(cur)
            state = 'active' if k == cur else 'done' if i < ci else 'todo'
            col = C['amber'] if state == 'active' else C['ok'] if state == 'done' else C['steel']
            if fault and k == cur: col = C['danger']
            glyph['text'] = '\u25b6' if state == 'active' else '\u2713' if state == 'done' else '\u25cb'
            glyph['fg'] = col; name['fg'] = C['text'] if state == 'active' else C['muted']
            bg = C['panel2'] if state == 'active' else C['panel']
            for w in (row, glyph, name): w['bg'] = bg
            bar['bg'] = col if state == 'active' else C['panel']
        self.fan['text'] = 'Unofficial fan project.\nNot affiliated with Microsoft,\nXbox or the MechAssault /\nBattleTech rights holders.'

    def refresh(self):
        page = self.cur
        if page is None: return
        ok, why = page.valid()
        self.status['text'] = why if (why and not ok) else (why or '')
        self.status['fg'] = C['orange'] if not ok else C['muted']
        for w in self.btns.winfo_children(): w.destroy()
        self.btn_widgets = []
        for text, cmd, style, enabled in page.buttons(ok):
            b = ttk.Button(self.btns, text=text, command=cmd, style=style, state='normal' if enabled else 'disabled')
            self.btn_widgets.append(b)
            if style.startswith('Primary') and enabled: b.focus_set()
        self.lay_buttons()
        self.root.update_idletasks()

    def lay_buttons(self):
        """The footer buttons in one row, or in two or more right-aligned rows when they do not fit the window (the failure screen has up to
        seven: Close, Open log folder, Copy log, Collect diagnostics, Systems check, Report a problem, Resume). The primary button stays in the last row."""
        S = self.t.S; ws = [b for b in getattr(self, 'btn_widgets', []) if b.winfo_exists()]
        if not ws: return
        gap = S(8); rw = self.root.winfo_width()
        avail = (rw - self.rail.winfo_reqwidth() - 2 * S(28)) if rw > 300 else 10 ** 6         # before the window is mapped: one row
        widths = [b.winfo_reqwidth() + gap for b in ws]
        rows, cur, used = [], [], 0
        for i in reversed(range(len(ws))):
            if cur and used + widths[i] > avail: rows.insert(0, cur); cur, used = [], 0
            cur.insert(0, i); used += widths[i]
        rows.insert(0, cur)
        ncols = max(len(r) for r in rows)
        for b in ws: b.grid_forget()
        for r, idxs in enumerate(rows):
            for c, i in enumerate(idxs):
                ws[i].grid(row=r, column=ncols - len(idxs) + c, sticky='e', padx=(gap, 0), pady=(S(6) if r else 0, 0))

    def on_root_configure(self, event):
        if event.widget is self.root and event.width != self._root_w:
            self._root_w = event.width; self.root.after_idle(self.lay_buttons)

    # -- services
    def bg(self, fn, cb):
        def run():
            try: res = fn()
            except Exception as e:
                res = e
            self.q.put(('call', lambda: cb(res)))
        threading.Thread(target=run, daemon=True).start()

    def detect_hardware(self):
        gpus = precheck.detect_gpus()
        self.facts['gpus'] = gpus; self.facts['reco'] = precheck.recommend_textures(gpus)
        self.q.put(('call', lambda: self.pages['loadout'].on_gpus() if self.cur and self.cur.key == 'loadout' else None))

    def confirm(self, title, text, yes='Stop', no='Keep installing'):
        return messagebox.askyesno(TITLE, text, icon='question')

    def ask_uninstall(self, dest):
        """The Uninstall question: (go, also delete settings and saves). Tests replace this."""
        win = tk.Toplevel(self.root); win.title(TITLE); win.configure(bg=C['bg']); win.transient(self.root); win.resizable(False, False)
        S = self.t.S; wipe = tk.BooleanVar(value=False); answer = {'go': False}
        self.t.label(win, 'Uninstall MechAssault 2 from %s?' % precheck.short(dest, 60), 'h2', 'text', bg='bg', wraplength=S(520), justify='left').pack(anchor='w', padx=S(20), pady=(S(16), S(6)))
        self.t.label(win, 'This deletes the game folder, its Desktop and Start-menu shortcuts and the shader and route caches. Your own scenarios in Documents\\MechAssault 2\\Scenarios are never deleted.',
                     'body', 'muted', bg='bg', wraplength=S(520), justify='left').pack(anchor='w', padx=S(20))
        ttk.Checkbutton(win, text='Also delete my settings and saves', variable=wipe).pack(anchor='w', padx=S(20), pady=(S(10), S(4)))
        row = tk.Frame(win, bg=C['bg']); row.pack(anchor='e', padx=S(20), pady=(S(8), S(16)))
        def finish(go): answer['go'] = go; win.destroy()
        ttk.Button(row, text='Cancel', command=lambda: finish(False)).pack(side='left', padx=(0, S(8)))
        ttk.Button(row, text='Uninstall', style='Primary.TButton', command=lambda: finish(True)).pack(side='left')
        win.protocol('WM_DELETE_WINDOW', lambda: finish(False)); win.grab_set(); self.root.wait_window(win)
        return answer['go'], bool(wipe.get() and answer['go'])

    def ask_diagnostics(self, dest):
        """(go, include a crash or hang dump, include the newest bot recording); both boxes start off. Tests replace this."""
        win = tk.Toplevel(self.root); win.title(TITLE); win.configure(bg=C['bg']); win.transient(self.root); win.resizable(False, False)
        S = self.t.S; dump = tk.BooleanVar(value=False); rec = tk.BooleanVar(value=False); answer = {'go': False}
        extras = precheck.diagnostics_extras()
        self.t.label(win, 'Collect diagnostics for a problem report', 'h2', 'text', bg='bg', wraplength=S(520), justify='left').pack(anchor='w', padx=S(20), pady=(S(16), S(6)))
        self.t.label(win, 'This saves one .zip on your Desktop with basic facts about this PC and whatever of these exists: the install record, your settings and the newest '
                     'game and installer logs. It works even if nothing is installed. Your user name is replaced in every text file.',
                     'body', 'muted', bg='bg', wraplength=S(520), justify='left').pack(anchor='w', padx=S(20))
        if extras['dump']:
            ttk.Checkbutton(win, text='Include the newest crash or hang dump (it can contain folder names and your Windows user name)', variable=dump).pack(anchor='w', padx=S(20), pady=(S(10), 0))
        if extras['botrec']:
            ttk.Checkbutton(win, text='Include the newest bot match recording', variable=rec).pack(anchor='w', padx=S(20), pady=(S(2), S(4)))
        row = tk.Frame(win, bg=C['bg']); row.pack(anchor='e', padx=S(20), pady=(S(8), S(16)))
        def finish(go): answer['go'] = go; win.destroy()
        ttk.Button(row, text='Cancel', command=lambda: finish(False)).pack(side='left', padx=(0, S(8)))
        ttk.Button(row, text='Collect', style='Primary.TButton', command=lambda: finish(True)).pack(side='left')
        win.protocol('WM_DELETE_WINDOW', lambda: finish(False)); win.grab_set(); self.root.wait_window(win)
        return answer['go'], bool(dump.get() and answer['go']), bool(rec.get() and answer['go'])

    def launch(self, what, path):
        if what == 'select':
            try: subprocess.Popen(['explorer.exe', '/select,' + str(path)])
            except OSError: pass
        elif what == 'open': open_path(path)
        else:
            try: subprocess.Popen([str(path)], cwd=str(Path(path).parent), close_fds=True)
            except OSError as e:                     # Smart App Control (4551), quarantined (225) or removed (2): say so
                self.show_error(install.launch_advice(path, e)); return False
        return True

    def show_error(self, text):
        try: messagebox.showerror(TITLE, text)
        except Exception: pass

    def params(self):
        """Installer arguments for the current choices."""
        action = self.action.get()
        tex = [] if self.texture.get() == 'original' else [self.texture.get()]
        return dict(xiso=precheck.clean_path(self.disc.get()) or None, dest=precheck.clean_path(self.dest.get()),
                    textures=tex, videos=self.videos.get(), update=action in ('update', 'repair'),
                    shortcuts=[k for k, v in self.sc.items() if v.get()] if action in ('install', 'update', 'repair') else [],
                    keep_awake=self.awake.get(), low_priority=self.lowprio.get(), merge_settings=self.merge.get(),
                    switch_textures=self.switch.get())

    def ui_error(self, exc, val, tb):
        """A Tk callback raised: the traceback goes to the installer log (stderr), the status line says so, the window stays usable."""
        traceback.print_exception(exc, val, tb)
        try:
            self.status['text'] = 'Internal error; details are in the installer log.'; self.status['fg'] = C['orange']
        except Exception:
            pass

    # -- event pump
    def pump(self):
        try:
            for _ in range(400):
                kind, value = self.q.get_nowait()
                if kind == 'call': value()
                else: self.pages['install'].event(kind, value)
        except queue.Empty:
            pass
        self.pages['install'].tick()
        self._after = self.root.after(100, self.pump)

    def stop_pump(self):
        try: self.root.after_cancel(self._after)
        except (tk.TclError, AttributeError): pass

    def close(self):
        if self.worker and self.worker.is_alive():
            if not self.confirm(TITLE, 'Stop installing? Everything finished so far is kept. Run the installer again and choose Resume to continue.'): return
            self.cancel_install(); self.worker.join(timeout=15)
        ui_theme.unload_fonts()
        self.stop_pump()
        self.root.destroy()

    def cancel_install(self):
        if self.inst: threading.Thread(target=self.inst.cancel, daemon=True).start()

# ---------------------------------------------------------------------------- pages

class Welcome(Page):
    key = 'welcome'
    back = False

    def build(self):
        t, S = self.t, self.t.S
        self.banner = t.panel(self, accent=True, bg='panel2')
        self.notice_text = None
        self.notice = t.panel(self, accent=True, bg='panel2')
        self.notice_l = t.label(self.notice, '', 'body', 'text', bg='panel2', wraplength=self.app.wrap - S(40), justify='left')
        self.notice_l.pack(anchor='w', padx=S(12), pady=S(8))
        self.intro = t.label(self, 'This builds a PC version of MechAssault 2 from your own disc image. It takes about 15-40 minutes and needs about 5 GB of space.',
                'body', 'text', wraplength=self.app.wrap, justify='left')
        self.intro.pack(anchor='w')
        legal = self.legal = t.panel(self, accent=True); legal.pack(fill='x', pady=(S(10), S(8)))
        t.label(legal, 'YOUR DISC, YOUR COPY', 'kicker', 'amber', bg='panel').pack(anchor='w', padx=S(14), pady=(S(8), 0))
        t.label(legal, 'You need your own, legally obtained copy of MechAssault 2 (original Xbox, US/NTSC retail disc), dumped to a disc image '
                '(.iso / .xiso). The game\'s program, art, sound and levels all come from that disc; "How does this work?" below says what the installer itself carries.',
                'body', 'text', bg='panel', wraplength=self.app.wrap - S(40), justify='left').pack(anchor='w', padx=S(14), pady=(0, S(10)))
        steps = self.steps = tk.Frame(self, bg=C['bg'])
        if not self.app.compact: steps.pack(fill='x')
        for n, txt in (('1', 'Reads your disc image.'), ('2', 'Translates the Xbox program to C and compiles it with Microsoft\'s free C++ tools.'),
                       ('3', 'Builds the PC menus and installs a ready-to-play folder.')):
            r = tk.Frame(steps, bg=C['bg']); r.pack(anchor='w', pady=S(1))
            t.label(r, n, 'chip', 'amber', width=2).pack(side='left')
            t.label(r, txt, 'body', 'text').pack(side='left')
        self.need = t.label(self, 'What you need:  64-bit Windows 10 (1903 or later) or 11  \u00b7  a DirectX 11 graphics card  \u00b7  about 5 GB free (plus about 8 GB on C: '
                'if the C++ Build Tools are needed)  \u00b7  8 GB RAM recommended  \u00b7  a US/NTSC retail disc image.',
                'small', 'muted', wraplength=self.app.wrap, justify='left')
        if not self.app.compact: self.need.pack(anchor='w', pady=(S(10), S(4)))
        links = self.links = tk.Frame(self, bg=C['bg']); links.pack(anchor='w')
        self.how = tk.Label(self, text=TECH + ('\n\n' + self.need.cget('text') if self.app.compact else ''), font=t.fonts['small'], fg=C['muted'], bg=C['bg'], wraplength=self.app.wrap, justify='left')
        self.smart = tk.Label(self, text=SMARTSCREEN, font=t.fonts['small'], fg=C['muted'], bg=C['bg'], wraplength=self.app.wrap, justify='left')
        ttk.Button(links, text='How does this work?', style='Link.TButton', command=lambda: self.toggle(self.how)).pack(side='left')
        ttk.Button(links, text='Windows warned me about this file', style='Link.TButton', command=lambda: self.toggle(self.smart)).pack(side='left', padx=S(12))
        # Always available, even with nothing installed. When a banner below offers its own Collect diagnostics button this one steps aside (detect_existing).
        self.diag_link = ttk.Button(links, text='Collect diagnostics', style='Link.TButton', command=lambda: self.diagnostics(self.diag_dest()))
        self.diag_link.pack(side='left'); self.in_banner = False
        self.ver = t.label(self, install.build_label(), 'small', 'steel')
        if precheck.MA2_SUPPORT_URL: self.ver['text'] += ' · Help: %s' % precheck.MA2_SUPPORT_URL
        if not self.app.compact: self.ver.pack(side='bottom', anchor='w')
        t.label(self, precheck.FAN_LINE, 'small', 'steel', wraplength=self.app.wrap, justify='left').pack(side='bottom', anchor='w', pady=(0, S(2)))

    def toggle(self, w):
        for other in (self.how, self.smart):
            if other is not w: other.pack_forget()
        if w.winfo_ismapped(): w.pack_forget()
        else: w.pack(anchor='w', pady=self.t.S(4))
        self.app.refresh()

    def on_show(self):
        self.notice_text = None
        self.detect_existing()

    def diag_dest(self):
        """The folder a diagnostics zip looks at: the install this PC remembers, else the folder chosen so far, else the default (it may not exist: that is fine)."""
        return precheck.remembered_install() or precheck.clean_path(self.app.dest.get()) or precheck.default_install_folder()

    def diagnostics(self, d):
        go, dump, rec = self.app.ask_diagnostics(d)
        if not go: return
        self.notice_text = 'Collecting diagnostics...'; self.detect_existing()
        def work():
            return precheck.collect_diagnostics(d, install.known_folder('desktop'), install.PUBLIC_VERSION, install.BUILD, self.app.facts.get('gpus'), dump, rec)
        self.app.bg(work, self.diagnostics_done)

    def diagnostics_done(self, res):
        if isinstance(res, Exception):
            self.notice_text = 'Could not collect diagnostics: %s' % res
        else:
            self.notice_text = 'Diagnostics saved to %s. Attach this file to your problem report (%s).' % (res['path'], precheck.support_where())
            self.app.launch('select', res['path'])
        self.detect_existing()

    def uninstall(self, d):
        go, wipe = self.app.ask_uninstall(d)
        if not go: return
        self.notice_text = 'Uninstalling...'; self.detect_existing()
        self.app.bg(lambda: install.uninstall(d, wipe), self.uninstalled)

    def uninstalled(self, res):
        if isinstance(res, Exception): self.notice_text = 'The uninstall stopped: %s' % res
        else: self.notice_text = res['text']
        self.detect_existing()

    def compact_banner(self, d):
        """Will detect_existing show a banner for the remembered install `d`?"""
        if not d: return False
        return precheck.read_state(d) is not None or install.install_relation(d) in ('older', 'same', 'newer')

    def detect_existing(self):
        """A banner for an interrupted install (Resume) or an older version (Update) in the remembered folder.
        One Collect diagnostics button is enough: the banner has its own, otherwise the links row below the intro shows it."""
        self.in_banner = False
        self._detect_existing()
        if self.in_banner: self.diag_link.pack_forget()
        elif not self.diag_link.winfo_ismapped(): self.diag_link.pack(side='left')

    def _detect_existing(self):
        t, S, app = self.t, self.t.S, self.app
        for w in self.banner.winfo_children(): w.destroy()
        self.banner.pack_forget()
        d = precheck.remembered_install()
        if not self.app.compact: self.steps.pack(fill='x', after=self.legal)
        self.notice.pack_forget()
        shown = bool(self.notice_text) or self.compact_banner(d)
        if self.app.compact:                          # a short screen: the banner replaces the one-line blurb
            if shown: self.intro.pack_forget()
            elif not self.intro.winfo_ismapped(): self.intro.pack(anchor='w', before=self.legal)
        if self.notice_text:                          # what Uninstall just did
            self.notice_l['text'] = self.notice_text
            self.notice.pack(fill='x', pady=(0, S(8)), before=self.pack_slaves()[0])
            self.steps.pack_forget(); self.need.pack_forget()
        elif not self.app.compact and not self.need.winfo_ismapped():
            self.need.pack(anchor='w', pady=(S(10), S(4)), before=self.links)
        if not d: return
        state, stamp = precheck.read_state(d), None
        rel = install.install_relation(d)
        stamp = precheck.read_stamp(d)
        text = None; buttons = []; one_row = False
        if state is not None and state.get('core_done') and install.is_install(d):
            # The game was installed; only its enhanced content (textures, videos) did not finish.
            text = ('MechAssault 2 Recomp is installed in %s and ready to play. Its enhanced content was not finished: Resume makes only that.'
                    % precheck.short(d, 60))
            buttons = [('Resume', lambda: self.choose(d, 'resume'), 'Primary.TButton'), ('Discard', lambda: self.start_over(d), 'TButton'),
                       ('Collect diagnostics', lambda: self.diagnostics(d), 'Link.TButton')]
            one_row = True
        elif state is not None:
            done = [s for s in state.get('done', []) if s not in ('check',)]
            last = state.get('last_title') or 'an earlier step'
            text = 'An install was interrupted (%d steps finished) in %s.' % (len(done), precheck.short(d, 60))
            buttons = [('Resume', lambda: self.choose(d, 'resume'), 'Primary.TButton'), ('Start over', lambda: self.start_over(d), 'TButton'),
                       ('Collect diagnostics', lambda: self.diagnostics(d), 'Link.TButton')]
            one_row = True                            # the quiet link shares the row: the banner stays as tall as before
        elif rel == 'older':
            s = stamp or {}
            have = ('%s (build %s)' % (s['public_version'], str(s.get('kit_id', ''))[:8]) if s.get('public_version') else
                    'from %s' % s['version'] if s.get('version') else 'an earlier version')
            text = 'MechAssault 2 Recomp %s is installed in %s. This installer is %s (build %s).' % (have, precheck.short(d, 60), install.PUBLIC_VERSION, install.BUILD)
            buttons = [('Update it', lambda: self.choose(d, 'update'), 'Primary.TButton'), ('Add enhanced content', lambda: self.choose(d, 'addons'), 'TButton'),
                       ('Install somewhere else', lambda: self.choose(precheck.default_install_folder(), 'install'), 'TButton'),
                       ('Uninstall', lambda: self.uninstall(d), 'Link.TButton'),
                       ('Collect diagnostics', lambda: self.diagnostics(d), 'Link.TButton')]
        elif rel == 'same':
            text = 'MechAssault 2 Recomp is already installed in %s and up to date.' % precheck.short(d, 60)
            buttons = [('Add enhanced content', lambda: self.choose(d, 'addons'), 'Primary.TButton'), ('Repair', lambda: self.choose(d, 'repair'), 'TButton'),
                       ('Uninstall', lambda: self.uninstall(d), 'Link.TButton'),
                       ('Collect diagnostics', lambda: self.diagnostics(d), 'Link.TButton')]
        elif rel == 'newer':
            text = 'MechAssault 2 Recomp is installed in %s. It was made by a newer installer than this one.' % precheck.short(d, 60)
            buttons = [('Add enhanced content', lambda: self.choose(d, 'addons'), 'Primary.TButton'), ('Uninstall', lambda: self.uninstall(d), 'Link.TButton'),
                       ('Collect diagnostics', lambda: self.diagnostics(d), 'Link.TButton')]
        if not text: return
        self.banner.pack(fill='x', pady=(0, S(8)), before=self.pack_slaves()[0])
        self.steps.pack_forget()                      # make room for the banner
        t.label(self.banner, text, 'body', 'text', bg='panel2', wraplength=app.wrap - S(40), justify='left').pack(anchor='w', padx=S(12), pady=(S(8), S(4)))
        row = tk.Frame(self.banner, bg=C['panel2']); row.pack(anchor='w', padx=S(10), pady=(0, S(8)))
        for label, cmd, style in buttons:
            if style != 'Link.TButton' or one_row: ttk.Button(row, text=label, command=cmd, style=style).pack(side='left', padx=(0, S(8)))
        self.in_banner = any(b[0] == 'Collect diagnostics' for b in buttons)
        more = [] if one_row else [b for b in buttons if b[2] == 'Link.TButton']          # Uninstall, Collect diagnostics: a quieter second row
        if more:
            row2 = tk.Frame(self.banner, bg=C['panel2']); row2.pack(anchor='w', padx=S(10), pady=(0, S(8)))
            for label, cmd, style in more: ttk.Button(row2, text=label, command=cmd, style=style).pack(side='left', padx=(0, S(8)))

    def choose(self, dest, action):
        self.app.dest.set(dest); self.app.action.set(action)
        if action == 'resume':
            self.app.show('install')
        else:
            self.app.show('systems')

    def start_over(self, d):
        if (precheck.read_state(d) or {}).get('core_done'):
            ask = ('Discard the unfinished enhanced content? This deletes its work in %s\\_work. The game stays installed; you can add the '
                   'content later with Add enhanced content.' % d, 'Discard', 'Cancel')
        else:
            ask = ('Start over? This deletes only the unfinished work in %s\\_work. Your extracted game files are kept.' % d, 'Start over', 'Cancel')
        if self.app.confirm(TITLE, *ask):
            install.start_over(d)
            self.detect_existing()

class Systems(Page):
    key = 'systems'

    def build(self):
        t, S = self.t, self.t.S
        self.rows = {}
        grid = tk.Frame(self, bg=C['bg']); grid.pack(fill='x')
        grid.columnconfigure(2, weight=1)
        for i, (k, name) in enumerate((('windows', 'Windows'), ('compiler', 'C++ Build Tools'), ('memory', 'Memory and CPU'), ('disk', 'Disk space'),
                                       ('graphics', 'Graphics'), ('internet', 'Internet'), ('power', 'Power'), ('sac', 'Smart App Control'),
                                       ('av', 'Antivirus'))):
            pad = S(3) if self.app.compact else S(5)
            chip = t.chip(grid, 'wait'); chip.grid(row=i, column=0, sticky='nw', pady=pad)
            t.label(grid, name, 'bold', 'text', bg='bg', width=16, anchor='w').grid(row=i, column=1, sticky='nw', pady=pad)
            cell = tk.Frame(grid, bg=C['bg']); cell.grid(row=i, column=2, sticky='nw', pady=pad)
            detail = t.label(cell, 'Checking...', 'body', 'muted', bg='bg', wraplength=S(400), justify='left', anchor='w'); detail.pack(anchor='w')
            acts = tk.Frame(cell, bg=C['bg']); acts.pack(anchor='w')
            self.rows[k] = (chip, detail, acts)
        self.state = {k: 'wait' for k in self.rows}
        self.busy = False

    def on_show(self):
        self.recheck()

    def recheck(self):
        if self.busy: return
        for k, (chip, detail, acts) in self.rows.items():
            self.t.set_chip(chip, 'wait'); detail['text'] = 'Checking...'; self.state[k] = 'wait'
        self.app.refresh()
        addons = self.app.action.get() == 'addons'        # Tk variables are read here, on the UI thread
        self.app.bg(lambda: self.run_checks(addons), self.checks_done)

    def checks_done(self, result):
        if not isinstance(result, Exception): return
        print('systems checks failed:\n%s' % ''.join(traceback.format_exception(type(result), result, result.__traceback__)), flush=True)
        for k, kind in list(self.state.items()):
            if kind == 'wait': self.set_row(k, 'info', 'Could not check (see the log).')

    def post(self, key, kind, text, actions=()):
        self.app.q.put(('call', lambda: self.set_row(key, kind, text, actions)))

    def set_row(self, key, kind, text, actions=()):
        chip, detail, acts = self.rows[key]
        self.t.set_chip(chip, kind); detail['text'] = text; self.state[key] = kind
        for w in acts.winfo_children(): w.destroy()
        for label, cmd in actions: ttk.Button(acts, text=label, command=cmd, style='TButton').pack(side='left', padx=(0, self.t.S(8)), pady=(self.t.S(4), 0))
        self.app.refresh()

    def guard(self, key, fn):
        """One failing check must not leave its row on 'Checking...' or hide the others: it says so and the log has the traceback."""
        try:
            fn()
        except Exception:
            print('systems check %r failed:\n%s' % (key, traceback.format_exc()), flush=True)
            self.post(key, 'info', 'Could not check (see the log).')

    def run_checks(self, addons):
        comp = {'state': 'unknown'}

        def windows():
            w = precheck.windows_info()
            self.post('windows', 'ready' if w['ok'] else 'blocked', '%s %s, build %d.' % (w['name'], '64-bit' if w['x64'] else '32-bit', w['build']) if w['ok']
                      else 'Windows 10 (1903 or later) or 11, 64-bit, is required.')

        def compiler():
            nonlocal comp
            if addons:
                self.post('compiler', 'info', 'Not needed to add content to an existing install.')
                comp = {'state': 'ready'}
            else:
                comp = install.compiler_state()
                self.app.facts['compiler'] = comp
                self.show_compiler(comp)

        def memory():
            total, avail = precheck.memory_info(); cpus = os.cpu_count() or 4
            jobs = precheck.build_jobs(cpus, avail, total); mins = precheck.estimate_minutes(cpus, avail, total)
            low = avail < 6 * precheck.GB
            self.post('memory', 'action' if low else 'ready', '%d threads, %.0f GB RAM (%.0f GB free), about %d parallel compiles. Estimated build time about %d min.%s' % (
                cpus, total / precheck.GB, avail / precheck.GB, jobs, mins, ' Close other programs before installing.' if low else ''))
            self.app.facts['minutes'] = mins

        def disk():
            sysdrive = os.environ.get('SystemDrive', 'C:') + '\\'
            try: free = shutil.disk_usage(sysdrive).free
            except OSError: free = None
            missing = comp['state'] == 'missing'
            if missing and free is not None and free < 8 * precheck.GB:
                self.post('disk', 'blocked', "%.1f GB free on %s; Microsoft's Build Tools need about 8 GB there. Free up space on %s."
                          % (free / precheck.GB, sysdrive, sysdrive))
            elif free is not None and free < 1 * precheck.GB:
                self.post('disk', 'action', 'Only %.1f GB free on %s. The game needs about 5 GB on the drive you install to '
                          '(checked on the Location page).' % (free / precheck.GB, sysdrive))
            else:
                self.post('disk', 'info', 'About 5 GB is needed on the drive you install to; it is checked on the Location page.%s'
                          % (' The Build Tools need about 8 GB on %s.' % sysdrive if missing else ''))

        def graphics():
            gpus = precheck.detect_gpus(); self.app.facts['gpus'] = gpus; reco = precheck.recommend_textures(gpus); self.app.facts['reco'] = reco
            d3d = precheck.d3d11_ok()
            if d3d is False:
                self.post('graphics', 'action', 'This graphics card or session (Remote Desktop / virtual machine) can\'t run the game: it needs DirectX 11. '
                          'You can still install, and play on this PC\'s own screen after updating the driver.')
            else:
                self.post('graphics', 'info', reco['reason'])

        def internet():
            ok = precheck.internet_ok()
            if comp['state'] == 'missing':
                self.post('internet', 'ready' if ok else 'action', 'Online: FFmpeg (cutscene playback) and the Build Tools can be downloaded.' if ok else
                          'No internet connection was found. The installer downloads FFmpeg for the cutscenes and the Build Tools (about 6 GB).')
            else:
                self.post('internet', 'ready' if ok else 'action', 'Online: FFmpeg (cutscene playback) is downloaded during the install.' if ok else
                          'No internet connection was found. The installer downloads FFmpeg for the cutscenes during the install '
                          '(or put %s in the install folder).' % install.ffmpeg_fetch.SPEC['archive'])

        def power():
            batt = precheck.on_battery()
            self.post('power', 'action' if batt else 'info', 'Running on battery: plug in your laptop; the build is CPU-heavy.' if batt else 'Plugged in. The installer keeps the PC awake while it works.')

        def sac():
            s = precheck.smart_app_control()
            if s == 'enforce':
                self.post('sac', 'action', 'Smart App Control is on. It may block the game this installer builds (it isn\'t signed). Turning it off is a one-way Windows '
                          'setting, so decide carefully. The installer never changes it.')
            else:
                self.post('sac', 'info', 'Not blocking.' if s in ('off', None) else 'In evaluation mode.')

        def av():
            self.post('av', 'info', 'Real-time antivirus scanning can slow the build. If your antivirus quarantines a file, the install stops with a clear message. '
                      'You never need to turn protection off.')

        for key, fn in (('windows', windows), ('compiler', compiler), ('memory', memory), ('disk', disk), ('graphics', graphics),
                        ('internet', internet), ('power', power), ('sac', sac), ('av', av)):
            self.guard(key, fn)

    def show_compiler(self, comp):
        st = comp['state']
        if st == 'ready':
            self.post('compiler', 'ready', '%s found.' % comp['name'])
        elif st == 'incomplete':
            names = [install.VS_PARTS[m][0] for m in comp.get('missing') or [] if m in install.VS_PARTS]
            msg = ('Visual Studio is missing %s.' % ' and '.join(names)) if names else comp['detail']
            self.post('compiler', 'action', msg + ' Add %s in the Visual Studio Installer, then re-check.' % ('them' if len(names) > 1 else 'it'),
                      [('Open Visual Studio Installer', self.open_vsi), ('Re-check', self.recheck_compiler)])
        else:
            self.post('compiler', 'action', BUILD_TOOLS_NOTICE,
                      [('Install for me', self.install_tools), ('I\'ll install them myself', self.manual_tools), ('Microsoft\'s licence terms', self.licence_terms)])

    def open_vsi(self):
        comp = self.app.facts.get('compiler', {})
        install.open_vs_installer(comp.get('vs'), comp.get('add'))       # adds exactly the parts that are missing

    def manual_tools(self):
        import webbrowser
        webbrowser.open('https://visualstudio.microsoft.com/visual-cpp-build-tools/')
        self.set_row('compiler', 'action', 'Install "Desktop development with C++" from the page that opened, then press Re-check.',
                     [('Re-check', self.recheck_compiler)])

    def recheck_compiler(self):
        self.set_row('compiler', 'wait', 'Checking...')
        self.app.bg(install.compiler_state, lambda comp: (self.app.facts.__setitem__('compiler', comp), self.show_compiler(comp)))

    def licence_terms(self):
        import webbrowser
        webbrowser.open(BUILD_TOOLS_TERMS)

    def install_tools(self):
        self.busy = True
        self.set_row('compiler', 'wait', 'Starting Microsoft\'s Build Tools installer: approve the administrator prompt when Windows asks; their own window shows progress (10-30 minutes).')
        work = Path(os.environ.get('TEMP', '.')) / 'MA2-buildtools'
        logf = None
        try:
            d = precheck.logs_dir(); d.mkdir(parents=True, exist_ok=True)
            logf = open(d / ('buildtools-%s.log' % time.strftime('%Y%m%d-%H%M%S')), 'a', encoding='utf-8', buffering=1)
        except OSError:
            pass
        def note(line):
            """Every line goes to buildtools-<time>.log; the two long waits also change the row text."""
            if logf:
                try: logf.write(line + '\n')
                except (OSError, ValueError): pass
            low = line.lower()
            if 'downloading' in low: self.post('compiler', 'wait', 'Downloading Microsoft\'s installer...')
            elif 'installing microsoft' in low: self.post('compiler', 'wait', 'Waiting for Microsoft\'s installer (approve the administrator prompt; their own window shows progress, 10-30 minutes)...')
        def go():
            try:
                note('MechAssault 2 Recomp installer, Build Tools one-click install, %s' % time.strftime('%Y-%m-%d %H:%M'))
                install.install_build_tools(work, note, lambda: False)
                res = install.compiler_state()
                note('compiler check afterwards: %s' % res.get('state'))
                return res
            except Exception as e:
                note('FAILED: %s' % e); raise
            finally:
                shutil.rmtree(work, ignore_errors=True)                  # the downloaded bootstrapper (never needed again)
                if logf: logf.close()
        def done(res):
            self.busy = False
            if isinstance(res, Exception):
                self.set_row('compiler', 'action', '%s (details: the Build Tools log in the Logs folder)' % res, [('Install for me', self.install_tools), ('I\'ll install them myself', self.manual_tools)])
            else:
                self.app.facts['compiler'] = res; self.show_compiler(res)
        self.app.bg(go, done)

    def valid(self):
        if 'wait' in self.state.values(): return False, 'Checking this PC...'
        if self.busy: return False, 'Waiting for the Build Tools installer...'
        if 'blocked' in self.state.values(): return False, 'Fix the items marked BLOCKED to continue.'
        if self.state.get('compiler') == 'action': return False, 'The C++ Build Tools are needed. Use the buttons on that row.'
        return True, ''

class Disc(Page):
    key = 'disc'

    def build(self):
        t, S = self.t, self.t.S
        self.drop = t.panel(self, accent=True)
        self.drop.pack(fill='x')
        t.label(self.drop, 'Choose your MechAssault 2 disc image', 'h2', 'text', bg='panel').pack(pady=(S(16), S(2)))
        t.label(self.drop, 'A .iso or .xiso, or a folder copied from the disc, made from your own US/NTSC retail disc. Both full-disc .iso and trimmed .xiso work.',
                'body', 'muted', bg='panel', wraplength=self.app.wrap - S(40), justify='center').pack()
        btns = tk.Frame(self.drop, bg=C['panel']); btns.pack(pady=(S(10), S(16)))
        ttk.Button(btns, text='Browse...', command=self.pick, style='Primary.TButton').pack(side='left')
        ttk.Button(btns, text='Choose a folder...', command=self.pick_folder, style='TButton').pack(side='left', padx=(S(8), 0))
        row = tk.Frame(self, bg=C['bg']); row.pack(fill='x', pady=(S(10), 0))
        self.entry = ttk.Entry(row, textvariable=self.app.disc); self.entry.pack(side='left', fill='x', expand=True)
        self.result = t.panel(self, cut=False); self.result.pack(fill='x', pady=(S(10), 0))
        self.chip = t.chip(self.result, 'info', 'WAITING'); self.chip.pack(side='left', padx=(S(10), 0), pady=S(10))
        col = tk.Frame(self.result, bg=C['panel']); col.pack(side='left', fill='x', expand=True, padx=S(8))
        self.head = t.label(col, 'No file chosen yet.', 'bold', 'text', bg='panel', anchor='w'); self.head.pack(anchor='w', pady=(S(8), 0))
        self.msg = t.label(col, '', 'body', 'muted', bg='panel', wraplength=self.app.wrap - S(100), justify='left', anchor='w'); self.msg.pack(anchor='w', pady=(0, S(8)))
        t.label(self, 'The installer never links to download sites. To make a disc image, dump your own disc.', 'small', 'steel').pack(anchor='w', pady=(S(8), 0))
        self.skip = t.label(self, '', 'body', 'ok', wraplength=self.app.wrap, justify='left')
        self.job = None; self.token = 0
        self.app.disc.trace_add('write', lambda *a: self.changed())

    def pick(self):
        p = filedialog.askopenfilename(title='MechAssault 2 disc image', filetypes=[('Xbox disc image', '*.xiso *.iso'), ('All files', '*.*')])
        if p: self.app.disc.set(os.path.normpath(p))

    def pick_folder(self):
        p = filedialog.askdirectory(title='Folder copied from the MechAssault 2 disc', mustexist=True)
        if p: self.app.disc.set(os.path.normpath(p))

    def on_show(self):
        self.changed()

    def changed(self):
        if self.job: self.after_cancel(self.job)
        self.job = self.after(300, self.validate)
        self.t.set_chip(self.chip, 'wait'); self.head['text'] = 'Checking...' if self.app.disc.get().strip() else 'No file chosen yet.'; self.msg['text'] = ''
        self.app.image_result = None
        self.app.refresh()

    def validate(self):
        text = precheck.clean_path(self.app.disc.get())
        self.job = None
        if not text:
            self.t.set_chip(self.chip, 'info', 'WAITING'); self.head['text'] = 'No file chosen yet.'; self.msg['text'] = ''; self.app.refresh(); return
        if not precheck.is_absolute(text):
            self.finish(text, precheck.ImageResult(False, 'relative', 'Enter a full path, e.g. D:\\Games\\MechAssault2.iso', 'Not a full path')); return
        self.token += 1; token = self.token
        self.app.bg(lambda: precheck.validate_image(text, install.RELEASE), lambda r: self.finish(text, r, token))

    def finish(self, text, res, token=None):
        if token is not None and token != self.token: return
        if isinstance(res, Exception):
            res = precheck.ImageResult(False, 'read_error', 'Couldn\'t read the file: %s.' % res, 'Can\'t read the file')
        self.app.image_result = res
        self.t.set_chip(self.chip, 'ok' if res.ok else 'error')
        self.head['text'] = res.headline or ('Recognised' if res.ok else 'Problem'); self.msg['text'] = res.message
        self.app.refresh()

    def valid(self):
        r = self.app.image_result
        if r is None: return False, 'Choose your disc image.'
        if not r.ok: return False, r.message
        return True, ''

class Location(Page):
    key = 'location'

    def build(self):
        t, S = self.t, self.t.S
        row = tk.Frame(self, bg=C['bg']); row.pack(fill='x')
        self.entry = ttk.Entry(row, textvariable=self.app.dest); self.entry.pack(side='left', fill='x', expand=True)
        ttk.Button(row, text='Browse...', command=self.pick).pack(side='left', padx=(S(8), 0))
        self.gauge = tk.Canvas(self, height=S(10), bg=C['panel2'], highlightthickness=1, highlightbackground=C['line']); self.gauge.pack(fill='x', pady=(S(10), 0))
        self.gtext = t.label(self, '', 'mono', 'muted', bg='bg', anchor='w'); self.gtext.pack(anchor='w', pady=(S(2), S(6)))
        self.list = tk.Frame(self, bg=C['bg']); self.list.pack(fill='x')
        self.choice = t.panel(self, accent=True, bg='panel2')
        self.job = None; self.token = 0; self.report = None
        self.app.dest.trace_add('write', lambda *a: self.changed())

    def pick(self):
        p = filedialog.askdirectory(title='Install folder', mustexist=False)
        if p: self.app.dest.set(os.path.normpath(p))

    def on_show(self):
        self.changed(now=True)

    def changed(self, now=False):
        if self.job: self.after_cancel(self.job)
        self.job = self.after(1 if now else 300, self.check)
        self.report = None
        self.app.refresh()

    def need_bytes(self):
        opts = dict(textures=[] if self.app.texture.get() == 'original' else [self.app.texture.get()], videos=self.app.videos.get())
        done = {'extract', 'build'} if self.app.action.get() == 'addons' else ({'extract'} if self.app.game_files_ok() and self.app.action.get() in ('update', 'repair') else set())
        return install.needed_space(opts, done)

    def check(self):
        self.job = None
        self.token += 1; token = self.token
        enhanced = self.app.texture.get() != 'original' or self.app.videos.get()
        text, need = self.app.dest.get(), self.need_bytes()
        self.app.bg(lambda: precheck.check_folder(text, need, enhanced, install.KIT), lambda r: self.show_report(r, token, need))

    def show_report(self, r, token, need):
        if token != self.token: return
        if isinstance(r, Exception): return
        t, S, app = self.t, self.t.S, self.app
        self.report = r
        for w in self.list.winfo_children(): w.destroy()
        for f in r.findings:
            row = tk.Frame(self.list, bg=C['bg']); row.pack(fill='x', pady=S(2))
            t.chip(row, {'error': 'error', 'warn': 'warn', 'info': 'info'}[f.level]).pack(side='left', anchor='n')
            t.label(row, f.text, 'body', 'text', bg='bg', wraplength=app.wrap - S(110), justify='left').pack(side='left', anchor='n')
            if f.fix: ttk.Button(row, text='Use ...\\MechAssault 2 inside it', style='Link.TButton', command=lambda p=f.fix: app.dest.set(p)).pack(side='left')
        if r.ok and r.existing in ('none', 'empty'):
            row = tk.Frame(self.list, bg=C['bg']); row.pack(fill='x', pady=S(2))
            t.chip(row, 'ready').pack(side='left', anchor='n')
            t.label(row, 'This folder is fine%s.' % (' (it will be created)' if r.existing == 'none' else ''), 'body', 'text', bg='bg').pack(side='left', anchor='n')
        # gauge
        free = r.free or 0
        w = max(1, self.gauge.winfo_width()); self.gauge.delete('all')
        if free:
            frac = min(1.0, need / float(free)) if need else 0
            self.gauge.create_rectangle(0, 0, w * frac, S(10), fill=C['danger'] if need * 1.1 > free else C['amber'], outline='')
        drive = os.path.splitdrive(precheck.clean_path(app.dest.get()))[0] or '?'
        self.gtext['text'] = ('Needs %.1f GB \u00b7 %.0f GB free on %s' % (need / precheck.GB, free / precheck.GB, drive)) if free else ''
        # existing install choices
        for w2 in self.choice.winfo_children(): w2.destroy()
        self.choice.pack_forget()
        opts = []
        if r.existing == 'unfinished':
            opts = [('resume', 'Finish the enhanced content (the game is already installed)' if (r.state or {}).get('core_done') else
                     'Resume the interrupted install (finished steps are kept)')]
            if app.action.get() not in ('resume',): app.action.set('resume')
        elif r.existing == 'install':
            rel = install.install_relation(precheck.clean_path(app.dest.get()))
            if rel == 'older': opts = [('update', 'Update to this version (about %d min, no disc needed; your settings and saves are kept)' % app.facts.get('minutes', 15)),
                                       ('addons', 'Add enhanced content only')]
            elif rel == 'same': opts = [('addons', 'Add enhanced content'), ('repair', 'Repair (rebuild the game)')]
            else: opts = [('addons', 'Add enhanced content'), ('repair', 'Install anyway (the installed version is newer than this installer)')]
            if app.action.get() not in [o[0] for o in opts]: app.action.set(opts[0][0])
        else:
            if app.action.get() in ('resume', 'update', 'repair', 'addons'): app.action.set('install')
        if opts:
            self.choice.pack(fill='x', pady=(S(8), 0))
            t.label(self.choice, 'MechAssault 2 is already here', 'kicker', 'amber', bg='panel2').pack(anchor='w', padx=S(12), pady=(S(8), 0))
            for val, label in opts:
                ttk.Radiobutton(self.choice, text=label, value=val, variable=app.action, command=app.refresh, style='P2.TRadiobutton').pack(anchor='w', padx=S(12), pady=(0, S(2)))
        app.update_rail(); app.refresh()

    def valid(self):
        if self.report is None: return False, 'Checking the folder...'
        err = self.report.first_error()
        if err: return False, err.text
        if self.app.needs_disc() and not (self.app.image_result and self.app.image_result.ok):
            return False, 'Choose your disc image first (Back).'
        return True, ''

class Loadout(Page):
    key = 'loadout'
    next_text = 'Install'

    def build(self):
        t, S, app = self.t, self.t.S, self.app
        self.tex = tk.Frame(self, bg=C['bg']); self.tex.pack(fill='x')
        t.label(self.tex, 'TEXTURE QUALITY', 'kicker', 'amber').pack(anchor='w')
        self.opts = {}
        for key, title, sub, size, when in (('original', 'Original', 'Ready right away', '0 GB', ''),
                                            ('2x', EXPECTED['2x'][0], EXPECTED['2x'][1], EXPECTED['2x'][2], EXPECTED['2x'][3]),
                                            ('4x', EXPECTED['4x'][0], EXPECTED['4x'][1], EXPECTED['4x'][2], EXPECTED['4x'][3])):
            row = tk.Frame(self.tex, bg=C['bg']); row.pack(fill='x')
            rb = ttk.Radiobutton(row, text='%s  \u2014  %s \u00b7 %s%s' % (title, sub, size, (' \u00b7 ' + when) if when else ''), value=key, variable=app.texture, command=app.refresh)
            rb.pack(side='left')
            tag = t.label(row, '', 'chip', 'amber', bg='bg'); tag.pack(side='left', padx=S(8))
            self.opts[key] = (rb, tag)
        self.why = t.label(self.tex, '', 'small', 'muted', wraplength=app.wrap, justify='left'); self.why.pack(anchor='w')
        self.later = t.label(self.tex, 'Play now and add enhanced textures later: run this installer again and pick Add enhanced content.', 'small', 'muted',
                             wraplength=app.wrap, justify='left')
        self.later.pack(anchor='w')
        self.vid = ttk.Checkbutton(self, text='Enhanced cutscenes (2x)  \u2014  +3.5 GB, needs 25 GB free while it runs, many hours', variable=app.videos, command=app.refresh)
        self.vid.pack(anchor='w', pady=(S(6), 0))
        cols = tk.Frame(self, bg=C['bg']); cols.pack(fill='x', pady=(S(8), 0))
        left = tk.Frame(cols, bg=C['bg']); left.pack(side='left', anchor='n', fill='x', expand=True)
        right = tk.Frame(cols, bg=C['bg']); right.pack(side='left', anchor='n', fill='x', expand=True)
        t.label(left, 'SHORTCUTS', 'kicker', 'amber').pack(anchor='w')
        self.sc_boxes = []
        for k, label in (('desktop', 'Desktop shortcut'), ('start', 'Start menu entry "MechAssault 2 (Recomp)"'), ('editor', 'Scenario Editor shortcut')):
            b = ttk.Checkbutton(left, text=label, variable=app.sc[k]); b.pack(anchor='w'); self.sc_boxes.append(b)
        t.label(right, 'WHILE INSTALLING', 'kicker', 'amber').pack(anchor='w')
        ttk.Checkbutton(right, text='Keep this PC awake while installing', variable=app.awake).pack(anchor='w')
        ttk.Checkbutton(right, text='Lower priority (keep the PC responsive)', variable=app.lowprio, command=app.refresh).pack(anchor='w')
        self.upd = tk.Frame(right, bg=C['bg']); self.upd.pack(anchor='w', fill='x')
        self.merge_box = ttk.Checkbutton(self.upd, text='Add new settings from this version (backup kept)', variable=app.merge)
        self.switch_box = ttk.Checkbutton(self.upd, text='Switch to the new enhanced textures now', variable=app.switch)
        self.summary = t.panel(self, accent=True); self.summary.pack(side='bottom', fill='x', pady=(S(8), 0))
        self.sum_l = t.label(self.summary, '', 'mono_mid', 'text', bg='panel', anchor='w', justify='left'); self.sum_l.pack(anchor='w', padx=S(12), pady=S(8))

    def on_show(self):
        app = self.app
        act = app.action.get()
        d = Path(precheck.clean_path(app.dest.get()))
        have = {'2x': (d / 'game_files/texture_packs/2x/index.json').is_file(), '4x': (d / 'game_files/texture_packs/4x/index.json').is_file()}
        self.have_videos = (d / 'game_files/Movie/Enhanced/2x').is_dir()
        self.have = have if act in ('addons', 'update', 'repair') else {'2x': False, '4x': False}
        for w in (self.merge_box, self.switch_box): w.pack_forget()
        if act in ('update', 'repair'): self.merge_box.pack(anchor='w')
        if act in ('update', 'repair', 'addons'): self.switch_box.pack(anchor='w')
        for b in self.sc_boxes: b['state'] = 'normal' if act in ('install', 'update', 'repair') else 'disabled'
        self.on_gpus()

    def on_gpus(self):
        app, S = self.app, self.t.S
        reco = app.facts.get('reco') or {'rec': 'original', 'can': False, 'reason': 'Checking your graphics card...'}
        for key, (rb, tag) in self.opts.items():
            locked = key != 'original' and (not reco['can'] or self.have.get(key))
            rb['state'] = 'disabled' if locked else 'normal'
            tag['text'] = ('RECOMMENDED FOR YOUR GPU' if reco['rec'] == key and reco['can'] and not self.have.get(key) else
                           'installed' if self.have.get(key) else ('locked' if locked else ''))
            if locked and app.texture.get() == key: app.texture.set('original')
        can_vid = reco.get("videos", reco["can"])        # NVIDIA: RealBasicVSR; any other card: Real-ESRGAN through Vulkan (1.1.3)
        self.vid['state'] = 'disabled' if (not can_vid or self.have_videos) else 'normal'
        if (not can_vid or self.have_videos) and app.videos.get(): app.videos.set(False)
        self.why['text'] = reco['reason'] + ('' if reco['can'] else ' The enhanced options need an NVIDIA GPU with 8 GB or more.')
        self.later.pack_forget() if reco['can'] and False else None
        self.update_summary()

    def update_summary(self):
        app = self.app
        opts = dict(textures=[] if app.texture.get() == 'original' else [app.texture.get()], videos=app.videos.get())
        act = app.action.get()
        done = {'extract', 'build'} if act == 'addons' else ({'extract'} if app.game_files_ok() and act in ('update', 'repair') else set())
        need = install.needed_space(opts, done, (app.facts.get('reco') or {}).get('engine', 'pytorch')) / precheck.GB
        mins = app.facts.get('minutes', 15)
        mins = 1 if act == 'addons' else mins
        extra = '  +  hours for the enhanced content' if (opts['textures'] or opts['videos']) else ''
        verb = {'install': 'Install', 'update': 'Update', 'repair': 'Repair', 'addons': 'Add content to'}.get(act, 'Install')
        self.sum_l['text'] = '%s %s\nabout %.1f GB \u00b7 about %d min%s' % (verb, precheck.short(precheck.clean_path(app.dest.get()), 46), need, mins, extra)

    def buttons(self, ok):
        self.update_summary()
        b = super().buttons(ok)
        verb = {'update': 'Update', 'repair': 'Repair', 'addons': 'Add content'}.get(self.app.action.get(), 'Install')
        return [x if x[0] == 'Back' else (verb,) + x[1:] for x in b]

    def valid(self):
        app = self.app
        if app.action.get() == 'addons' and app.texture.get() == 'original' and not app.videos.get():
            return False, 'Tick the enhanced content you want to add.'
        if app.needs_disc() and not (app.image_result and app.image_result.ok): return False, 'Choose your disc image first (go Back).'
        return True, ''

class Install(Page):
    key = 'install'
    back = False

    def build(self):
        t, S, app = self.t, self.t.S, self.app
        self.failed = None; self.model = None; self.lines = []; self.finished = False; self.stage_rows = {}
        self.top_mode = 'full'; self._fitting = False
        # Packing order is priority order. The details row, the log and its note come first (at the bottom), so a short window
        # squeezes the stage list above them and never hides the note ("Lines marked normal ...") or the Copy log row.
        self.area = tk.Frame(self, bg=C['bg']); self.area.pack(side='bottom', fill='both', expand=True)
        self.upper = tk.Frame(self, bg=C['bg']); self.upper.pack(side='top', fill='x')
        top = self.top = tk.Frame(self.upper, bg=C['bg']); top.pack(fill='x')
        left = self.left = tk.Frame(top, bg=C['bg']); left.pack(side='left', anchor='n')
        self.ring = ui_theme.Reticle(left, t, 150); self.ring.pack()
        self.eta = t.label(left, 'Estimating...', 'mono_mid', 'amber', bg='bg'); self.eta.pack(pady=(S(6), 0))
        self.elapsed = t.label(left, 'Elapsed 0:00', 'mono', 'muted', bg='bg'); self.elapsed.pack()
        self.right = tk.Frame(top, bg=C['bg']); self.right.pack(side='left', fill='x', expand=True, padx=(S(24), 0), anchor='n')
        self.fail_panel = t.panel(self.upper, accent=True)
        self.fail_head = t.label(self.fail_panel, '', 'h2', 'danger', bg='panel', anchor='w'); self.fail_head.pack(anchor='w', padx=S(14), pady=(S(8), 0))
        self.fail_msg = t.label(self.fail_panel, '', 'body', 'text', bg='panel', wraplength=app.wrap - S(40), justify='left'); self.fail_msg.pack(anchor='w', padx=S(14), pady=(0, S(4)))
        self.hide_user = tk.BooleanVar(value=True)
        ttk.Checkbutton(self.fail_panel, text='Hide my user name in the copied log', variable=self.hide_user, style='Panel.TCheckbutton').pack(anchor='w', padx=S(10), pady=(0, S(8)))
        self.fail_note = t.label(self.fail_panel, '', 'small', 'muted', bg='panel', wraplength=app.wrap - S(40), justify='left')     # shown after Collect diagnostics
        self.detail = t.label(self.upper, '', 'body', 'muted', anchor='w'); self.detail.pack(fill='x', pady=(S(8), 0), side='top')
        self.show_log = tk.BooleanVar(value=False)
        bar = self.bar = tk.Frame(self.area, bg=C['bg']); bar.pack(fill='x', side='top')
        ttk.Checkbutton(bar, text='Show details', variable=self.show_log, command=self.toggle_log).pack(side='left')
        self.copy_b = ttk.Button(bar, text='Copy log', style='Link.TButton', command=self.copy_log); self.open_b = ttk.Button(bar, text='Open log folder', style='Link.TButton', command=self.open_logs)
        self.copy_b.pack(side='left', padx=S(10)); self.open_b.pack(side='left')
        self.logf = tk.Frame(self.area, bg=C['bg'])
        # The note is packed before the box, so it always gets its row; the box takes what is left (it asks for 1 line and grows with the window).
        self.note = t.label(self.logf, 'Lines marked normal are expected and don\'t mean something went wrong.', 'small', 'steel', bg='bg')
        self.note.pack(side='top', anchor='w', pady=(0, S(2)))
        box = tk.Frame(self.logf, bg=C['bg']); box.pack(side='top', fill='both', expand=True)
        self.log = tk.Text(box, height=1, font=t.fonts['mono'], bg=C['panel'], fg=C['text'], bd=0, highlightthickness=1, highlightbackground=C['line'],
                           state='disabled', wrap='none')
        sb = ttk.Scrollbar(box, orient='vertical', command=self.log.yview); self.log['yscrollcommand'] = sb.set
        sb.pack(side='right', fill='y'); self.log.pack(side='left', fill='both', expand=True)
        self.log.tag_configure('dim', foreground=C['steel'])
        self.rows_built = False
        self.bind('<Configure>', lambda e: self.fit() if e.widget is self else None)
        self.fail_panel.bind('<Configure>', self.wrap_failure)

    def kicker(self):
        if self.failed and self.failed[0] == 'extras': return ('INSTALLED', 'The extras were not finished')
        return ('SYSTEM FAULT', 'The install stopped') if self.failed else KICKER['install']

    # -- stage list
    def build_rows(self, names):
        t, S = self.t, self.t.S
        for w in self.right.winfo_children(): w.destroy()
        self.stage_rows = {}
        self.model = progress.ProgressModel(names)
        for sid, title, steps in self.model.stages:
            row = tk.Frame(self.right, bg=C['bg']); row.pack(fill='x', pady=S(2))
            bar = tk.Frame(row, bg=C['bg'], width=S(3)); bar.pack(side='left', fill='y')
            glyph = tk.Label(row, text='\u25cb', font=t.fonts['rail'], fg=C['steel'], bg=C['bg'], width=2); glyph.pack(side='left')
            name = tk.Label(row, text=title, font=t.fonts['rail'], fg=C['muted'], bg=C['bg'], anchor='w'); name.pack(side='left')
            det = tk.Label(row, text='', font=t.fonts['mono'], fg=C['muted'], bg=C['bg'], anchor='e'); det.pack(side='right')
            self.stage_rows[sid] = (row, bar, glyph, name, det)
        self.render()

    def render(self):
        m = self.model
        if not m: return
        t = self.t
        for sid, (row, bar, glyph, name, det) in self.stage_rows.items():
            st = m.stage_status(sid)
            col = {'active': C['amber'], 'done': C['ok'], 'kept': C['ok'], 'failed': C['danger']}.get(st, C['steel'])
            glyph['text'] = {'active': '\u25b6', 'done': '\u2713', 'kept': '\u2713', 'failed': '\u2715'}.get(st, '\u25cb'); glyph['fg'] = col
            name['fg'] = C['text'] if st in ('active', 'failed') else C['muted']
            bar['bg'] = col if st in ('active', 'failed') else C['bg']
            det['text'] = m.stage_detail(sid) if st == 'active' else ('kept from last time' if st == 'kept' else '')
        frac = m.fraction()
        self.ring.set(frac)
        self.eta['text'] = progress.fmt_eta(m.eta()) if not self.finished and not self.failed else ('Done' if self.finished else 'Stopped')
        self.elapsed['text'] = 'Elapsed ' + progress.fmt_elapsed(m.elapsed())

    def tick(self):
        if self.model and not self.finished and not self.failed and self.app.cur is self: self.render()

    # -- running
    def on_show(self):
        app = self.app
        if self.failed or self.finished or (app.worker and app.worker.is_alive()): return
        self.start()

    def start(self):
        app = self.app
        self.failed = None; self.finished = False; self.lines = []
        self.fail_note['text'] = ''; self.fail_note.pack_forget()
        self.top_mode = 'full'; self.place(); self.after_idle(self.fit)
        self.log.configure(state='normal'); self.log.delete('1.0', 'end'); self.log.configure(state='disabled')
        p = app.params()
        if app.action.get() == 'resume':
            st = precheck.read_state(p['dest']) or {}
            opts = st.get('options') or {}
            p.update(textures=opts.get('textures', []), videos=opts.get('videos', False), update=bool(st.get('update')), xiso=p['xiso'] or st.get('xiso'))
        fake = os.environ.get('MA2_INSTALLER_FAKE')
        cb = dict(log=lambda s: app.q.put(('log', s)), step=lambda i, n, title, name, skipped=False: app.q.put(('step', (i, n, title, name, skipped))))
        if fake:
            import fakeinstall
            f = os.environ.get('MA2_FAKE_FAIL')
            app.inst = fakeinstall.FakeInstaller(fake, speed=float(os.environ.get('MA2_FAKE_SPEED', '40')), fail=tuple(f.split(':')) if f else None,
                                                 extra=[x for x in os.environ.get('MA2_FAKE_EXTRA', '').split(',') if x], dest=p['dest'], **cb)
            app.inst.on_plan = lambda names: app.q.put(('plan', names))
        else:
            app.inst = install.Installer(p['xiso'], p['dest'], p['textures'], p['videos'], install_build_tools=False, update=p['update'],
                                         shortcuts=p['shortcuts'], keep_awake=p['keep_awake'], low_priority=p['low_priority'],
                                         merge_settings=p['merge_settings'], switch_textures=p['switch_textures'], **cb)
            app.inst.on_plan = lambda names: app.q.put(('plan', names))
            app.inst.until = os.environ.get('MA2_INSTALLER_UNTIL')          # maintainers: stop after this step
        self.started = time.monotonic(); app.started = self.started
        app.worker = threading.Thread(target=self.work, daemon=True); app.worker.start()
        app.refresh()

    def work(self):
        q, inst = self.app.q, self.app.inst
        try:
            inst.run(); q.put(('done', None))
        except install.Stop as e:
            q.put(('stopped', (e.kind, str(e))))
        except Exception:
            q.put(('log', traceback.format_exc())); q.put(('stopped', ('unexpected', 'Unexpected error (details in the log).')))

    def event(self, kind, value):
        if kind == 'plan':
            self.build_rows(list(value)); return
        if kind == 'log':
            self.lines.append(value)
            if self.model: self.model.feed(value)
            self.append(value); return
        if kind == 'step':
            i, n, title, name, skipped = value
            if not self.model: self.build_rows([name])
            self.model.step(name, skipped)
            self.detail['text'] = title if not skipped else title + ' (kept from last time)'
            self.render(); return
        if kind == 'done':
            self.finished = True
            if self.model: self.model.complete()
            self.render(); self.flash()
            self.app.pages['done'].prepare()
            self.app.show('done'); return
        if kind == 'stopped':
            self.fail(*value)

    def append(self, line):
        dim = any(w in line for w in ('unimplemented', 'stubbed', 'warning C', 'unresolved'))
        self.log.configure(state='normal')
        self.log.insert('end', line + '\n', 'dim' if dim else '')
        if int(self.log.index('end-1c').split('.')[0]) > 4000: self.log.delete('1.0', '1000.0')
        self.log.see('end'); self.log.configure(state='disabled')

    def toggle_log(self):
        if self.show_log.get():
            self.logf.pack(side='top', fill='both', expand=True, pady=(self.t.S(2), 0))
        else:
            self.logf.pack_forget()
        self.app.refresh(); self.fit()

    def wrap_failure(self, event):
        """The failure text wraps to the panel (the smallest window is narrower than the usual wrap width, which cut the left of the message)."""
        S = self.t.S; w = max(S(300), min(self.app.wrap - S(40), event.width - S(40)))
        if int(str(self.fail_msg.cget('wraplength'))) != w:
            self.fail_msg['wraplength'] = w; self.fail_note['wraplength'] = w; self.after_idle(self.fit)

    def place(self):
        """(Re)pack the part above the log: the stage list (with the ring while it fits and nothing failed), then the failure message or the step line."""
        S = self.t.S
        for w in (self.top, self.fail_panel, self.detail, self.left): w.pack_forget()
        if self.top_mode != 'none':
            self.top.pack(fill='x')
            if self.top_mode == 'full' and not self.failed: self.left.pack(side='left', anchor='n', before=self.right)
        if self.failed: self.fail_panel.pack(fill='x', pady=(S(8), 0))
        else: self.detail.pack(fill='x', pady=(S(8), 0))

    def fit(self):
        """Keep the log's note and the Copy log row on screen in a short window (a 720p laptop, the smallest window, a long failure message):
        what gives way, in order, is the ring, then the stage list, then the log's own height (down to 1 line)."""
        if self._fitting or not self.winfo_ismapped(): return
        self._fitting = True
        try:
            S = self.t.S
            self.update_idletasks()
            used = self.bar.winfo_reqheight() + S(8)
            used += (self.fail_panel.winfo_reqheight() if self.failed else self.detail.winfo_reqheight()) + S(8)
            room = self.winfo_height() - used
            line = self.t.fonts['mono'].metrics('linespace')
            least = want = 0
            if self.show_log.get():
                least = self.note.winfo_reqheight() + S(8) + 3 * line + S(8)
                want = self.note.winfo_reqheight() + S(8) + 6 * line + S(8)
            right = self.right.winfo_reqheight()
            modes = [('slim', right)] if self.failed else [('full', max(right, self.left.winfo_reqheight())), ('slim', right)]
            pick = 'none'
            for cost in (want, least):
                for name, h in modes:
                    if room - cost >= h: pick = name; break
                if pick != 'none': break
            if pick != self.top_mode:
                self.top_mode = pick; self.place()
        finally:
            self._fitting = False

    def fail(self, kind, msg):
        app = self.app
        self.failed = (kind, msg)
        if self.model: self.model.fail(app.inst.stage if app.inst else None)
        head, advice = FAILURES.get(kind, FAILURES['unexpected'])
        drive = os.path.splitdrive(precheck.clean_path(app.dest.get()))[0] or 'the drive'
        self.fail_head['text'] = head
        if kind == 'extras' and msg.startswith('MechAssault 2 is installed. '): msg = msg[len('MechAssault 2 is installed. '):]   # the headline says it
        self.fail_msg['text'] = advice.format(drive=drive, report=precheck.report_hint()) + ('\n\n' + msg if kind not in ('cancelled',) and msg and msg != head else '')
        self.detail['text'] = ''
        self.place()                                   # room for the explanation: no ring; the stage list keeps the failed row if it fits (fit)
        app.kicker_l['text'], app.title_l['text'] = self.kicker()
        self.render(); app.update_rail(fault=True); app.refresh(); self.fit(); self.flash()

    def flash(self):
        try:
            class FW(ctypes.Structure):
                _fields_ = [('cb', ctypes.c_uint), ('hwnd', ctypes.c_void_p), ('flags', ctypes.c_uint), ('count', ctypes.c_uint), ('timeout', ctypes.c_uint)]
            f = FW(); f.cb = ctypes.sizeof(FW); f.hwnd = self.app.root.winfo_id(); f.flags = 0x0F; f.count = 3
            ctypes.windll.user32.FlashWindowEx(ctypes.byref(f))
        except Exception:
            pass

    # -- actions
    def bundle(self):
        app = self.app
        return precheck.support_bundle(self.lines, (self.failed and app.inst and app.inst.stage) or '', self.hide_user.get(), install.PUBLIC_VERSION, install.BUILD,
                                       app.facts.get('gpus') or [], (app.facts.get('compiler') or {}).get('vs') or '')

    def copy_log(self):
        self.app.root.clipboard_clear(); self.app.root.clipboard_append(self.bundle()); self.app.root.update()
        self.app.status['text'] = 'Log copied to the clipboard.'; self.app.status['fg'] = C['ok']

    def open_logs(self):
        d = precheck.logs_dir(); d.mkdir(parents=True, exist_ok=True)
        self.app.launch('open', d)

    def diagnostics(self):
        """Collect diagnostics from the failure screen: the zip names the step that failed and the reason, and holds the newest install log
        (plus the last lines this window showed)."""
        app = self.app
        dest = precheck.clean_path(app.dest.get()) or precheck.default_install_folder()
        go, dump, rec = app.ask_diagnostics(dest)
        if not go: return
        stage = (app.inst and app.inst.stage) or ''
        failure = '%s: %s' % self.failed if self.failed else ''
        lines = list(self.lines)
        self.show_note('Collecting diagnostics...')
        def work():
            return precheck.collect_diagnostics(dest, install.known_folder('desktop'), install.PUBLIC_VERSION, install.BUILD, app.facts.get('gpus'), dump, rec, stage, failure, lines)
        app.bg(work, self.diagnostics_done)

    def diagnostics_done(self, res):
        if isinstance(res, Exception):
            self.show_note('Could not collect diagnostics: %s' % res)
        else:
            self.show_note('Diagnostics saved to %s. Attach this file to your problem report (%s).' % (res['path'], precheck.support_where()))
            self.app.launch('select', res['path'])

    def show_note(self, text):
        self.fail_note['text'] = text
        if not self.fail_note.winfo_manager(): self.fail_note.pack(anchor='w', padx=self.t.S(14), pady=(0, self.t.S(8)))
        self.fit()

    def leave(self, key):
        """Back from the failure screen to a page where the cause can be fixed ('systems' for the compiler, 'disc' for the image)."""
        self.failed = None; self.finished = False
        self.app.worker = None
        self.app.update_rail(); self.app.show(key)

    def report_problem(self):
        self.app.launch('open', precheck.MA2_SUPPORT_URL)

    def resume(self):
        self.app.worker = None
        self.app.action.set('resume' if precheck.read_state(precheck.clean_path(self.app.dest.get())) is not None else self.app.action.get())
        self.failed = None; self.finished = False
        self.app.kicker_l['text'], self.app.title_l['text'] = self.kicker()
        self.app.update_rail(); self.start()

    def stop(self):
        if self.app.confirm(TITLE, 'Stop installing? Everything finished so far is kept. Run the installer again and choose Resume to continue.'):
            self.app.cancel_install()

    def buttons(self, ok):
        if self.failed:
            out = [('Close', self.app.close, 'TButton', True), ('Open log folder', self.open_logs, 'TButton', True), ('Copy log', self.copy_log, 'TButton', True),
                   ('Collect diagnostics', self.diagnostics, 'TButton', True)]
            kind = self.failed[0]
            if kind == 'compiler': out.append(('Systems check', lambda: self.leave('systems'), 'TButton', True))
            if kind == 'disc': out.append(('Choose disc image', lambda: self.leave('disc'), 'TButton', True))
            if precheck.MA2_SUPPORT_URL: out.append(('Report a problem', self.report_problem, 'TButton', True))     # D-11: hidden until a URL is set
            return out + [('Resume', self.resume, 'Primary.TButton', True)]
        return [('Cancel', self.stop, 'TButton', not self.finished)]

class Done(Page):
    key = 'done'
    back = False

    def build(self):
        t, S, app = self.t, self.t.S, self.app
        top = tk.Frame(self, bg=C['bg']); top.pack(fill='x')
        self.where = t.label(top, '', 'body', 'muted', anchor='w', wraplength=app.wrap, justify='left'); self.where.pack(anchor='w')
        row = tk.Frame(self, bg=C['bg']); row.pack(fill='x', pady=(S(10), S(6)))
        self.play_b = ttk.Button(row, text='\u25b6  PLAY', style='Big.Primary.TButton', command=self.play); self.play_b.pack(side='left')
        ttk.Checkbutton(row, text='Close the installer when the game starts', variable=app.close_on_play).pack(side='left', padx=S(14))
        row2 = tk.Frame(self, bg=C['bg']); row2.pack(fill='x', pady=(0, S(8)))
        ttk.Button(row2, text='Open install folder', command=lambda: app.launch('open', precheck.clean_path(app.dest.get()))).pack(side='left')
        self.ed_b = ttk.Button(row2, text='Open Scenario Editor', command=self.editor); self.ed_b.pack(side='left', padx=S(8))
        self.ctl_b = ttk.Button(row2, text='Controls', command=lambda: app.launch('open', Path(precheck.clean_path(app.dest.get())) / 'Controls.txt')); self.ctl_b.pack(side='left')
        self.good = t.panel(self); self.good.pack(fill='x')
        self.notes = t.label(self, '', 'small', 'muted', wraplength=app.wrap, justify='left')
        self.notes.pack(anchor='w', pady=(S(6), 0))
        t.label(self, precheck.FAN_LINE, 'small', 'steel', wraplength=app.wrap, justify='left').pack(side='bottom', anchor='w')

    def prepare(self):
        t, S, app = self.t, self.t.S, self.app
        dest = Path(precheck.clean_path(app.dest.get()))
        el = time.monotonic() - (app.started or time.monotonic())
        self.where['text'] = '%s  \u00b7  installed in %s  \u00b7  took %s' % (install.build_label(), precheck.short(dest, 50), precheck.fmt_duration(el))
        for w in self.good.winfo_children(): w.destroy()
        t.label(self.good, 'GOOD TO KNOW', 'kicker', 'amber', bg='panel').grid(row=0, column=0, sticky='w', padx=S(12), pady=(S(8), S(2)), columnspan=2)
        local = precheck.local_dir()
        rows = [('Settings', local / 'pc_settings.ini', 'Created on first launch; also in the game\'s Player Options'),
                ('Saves and profiles', local / 'Saved Games', ''), ('Logs', precheck.logs_dir(), 'If the game ever closes unexpectedly, look here'),
                ('Add more later', None, 'Run this installer again and choose Add enhanced content'),
                ('Uninstall', None, 'Run this installer again and press Uninstall (your settings and saves are kept unless you tick the box)')]
        for i, (what, path, note) in enumerate(rows, 1):
            t.label(self.good, what, 'bold', 'text', bg='panel', anchor='w').grid(row=i, column=0, sticky='nw', padx=(S(12), S(10)), pady=1)
            if path:
                l = tk.Label(self.good, text=precheck.short(path, 44), font=t.fonts['mono'], fg=C['amber'], bg=C['panel'], cursor='hand2', anchor='w')
                l.bind('<Button-1>', lambda e, p=path: app.launch('open', p if Path(p).exists() else Path(p).parent)); l.grid(row=i, column=1, sticky='w')
                t.label(self.good, note, 'small', 'muted', bg='panel', anchor='w').grid(row=i, column=2, sticky='w', padx=S(8)) if note else None
            else:
                t.label(self.good, note, 'small', 'muted', bg='panel', anchor='w').grid(row=i, column=1, columnspan=2, sticky='w')
        tk.Frame(self.good, bg=C['panel'], height=S(6)).grid(row=len(rows) + 1)
        extra = []
        for note in (app.inst.notes if app.inst else []): extra.append(note)
        tex = (dest / 'game_files/texture_packs/2x/index.json').is_file() or (dest / 'game_files/texture_packs/4x/index.json').is_file()
        if tex and not app.switch.get() and app.action.get() in ('addons', 'update', 'repair'): extra.append('Turn the new textures on in Video Settings.')
        extra.append('The first match on each map may stutter briefly while shaders are prepared.')
        self.notes['text'] = '\n'.join(extra)
        self.ed_b['state'] = 'normal' if any(dest.glob('MA2 Scenario Editor*.exe')) else 'disabled'
        self.ctl_b['state'] = 'normal' if (dest / 'Controls.txt').is_file() else 'disabled'

    def kicker(self):
        return KICKER['done']

    def play(self):
        app = self.app
        ok = app.launch('play', Path(precheck.clean_path(app.dest.get())) / 'MechAssault 2.exe')
        if ok is not False and app.close_on_play.get(): app.close()

    def editor(self):
        d = Path(precheck.clean_path(self.app.dest.get()))
        ed = next(iter(sorted(d.glob('MA2 Scenario Editor*.exe'))), None)
        if ed: self.app.launch('play', ed)

    def buttons(self, ok):
        return [('Close', self.app.close, 'Primary.TButton', True)]

def message_box(text):
    """A plain Windows message box (the window may not exist yet). Tests replace this."""
    try: ctypes.windll.user32.MessageBoxW(None, text, TITLE, 0x10)
    except Exception: pass

def open_ui_log():
    """%LOCALAPPDATA%\\MechAssault2\\Logs\\installer-ui-<time>.log; becomes stdout and stderr (pythonw has neither), so a crash leaves a trace."""
    try:
        d = precheck.logs_dir(); d.mkdir(parents=True, exist_ok=True)
        for f in sorted(d.glob('installer-ui-*.log'), key=lambda f: f.stat().st_mtime)[:-9]:
            try: f.unlink()
            except OSError: pass
        path = d / ('installer-ui-%s.log' % time.strftime('%Y%m%d-%H%M%S'))
        return path, open(path, 'a', encoding='utf-8', buffering=1)
    except OSError:
        return None, None

def main():
    log_path, log = STARTUP_LOG if STARTUP_LOG[1] else open_ui_log()     # opened at the top of this file when run as the window
    if log: sys.stdout = sys.stderr = log
    try:
        if os.environ.get('MA2_UI_CRASH_TEST'): raise RuntimeError('MA2_UI_CRASH_TEST: simulated start-up failure')
        ui_theme.load_fonts()
        root = tk.Tk()
        theme = ui_theme.Theme(root)
        App(root, theme)
        root.mainloop()
    except Exception:
        traceback.print_exc()
        message_box('The installer could not start.\n\nDetails were saved to:\n%s' % (log_path or 'the installer could not write a log'))
        sys.exit(3)
    if log and log_path and log_path.is_file() and log_path.stat().st_size == 0:      # nothing went wrong: no empty log left behind
        try: faulthandler.disable()
        except Exception: pass
        log.close()
        try: log_path.unlink()
        except OSError: pass

if __name__ == '__main__':
    main()
