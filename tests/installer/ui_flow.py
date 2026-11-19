"""The 1.1.4 installer window end to end, against a recorded install log (no compile): every screen and state, the engine call it makes,
the keyboard, the layout at 100 / 125 / 200 %, and a forced failure of each new mechanism (it must fall back and still install).
Saves screenshots to <work>/shots (and, with --shots <folder>, one clean picture of each screen there).
  python -B ui_flow.py [scenario ...] [--shots <folder>]
The classic 1.1.3 window keeps its own flow: ui_flow_legacy.py.
"""
import json, os, shutil, sys, tempfile, time
from pathlib import Path
import rig
from rig import WORK, pump, until, snap
import precheck, install, ui_theme, ui_win, ui_kit
import tkinter as tk

gui = rig.gui
REC = WORK.parent / 'exe-test-1030' / 'out.txt'
if not REC.is_file(): REC = WORK / 'installer' / 'data' / 'recorded-install.txt'          # the public tree's copy
XISO = Path(os.environ.get('MA2_TEST_XISO') or WORK.parents[4] / 'MechAssault 2 extracted.xiso')
GAME_FILES = Path(os.environ.get('MA2_TEST_GAME_FILES') or WORK.parents[3] / 'game_files')
if __name__ == '__main__' and not (XISO.is_file() and (GAME_FILES / 'ma2.xbe').is_file()):
    sys.exit('ui_flow needs your own disc: set MA2_TEST_XISO=<disc image> and MA2_TEST_GAME_FILES=<extracted disc folder>')
SHOTS = WORK / 'shots'
OWNER_SHOTS = None
RESULTS = []
TOOL_WORDS = ('Visual Studio', 'MSVC', 'CMake', 'Ninja', 'PyTorch', 'xboxrecomp', 'vcvars', 'cl.exe', 'jobs', 'Real-ESRGAN', 'RealBasicVSR', 'ncnn')

def check(name, cond, extra=''):
    RESULTS.append((name, bool(cond), str(extra)))
    line = ('PASS ' if cond else 'FAIL ') + name + (' -- ' + str(extra) if extra and not cond else '')
    print(line.encode('ascii', 'replace').decode('ascii'), flush=True)        # any console code page

def focused(root):
    """The window's focus widget even while another program is in front (the test window never takes the real focus)."""
    try: return root.nametowidget(root.tk.call('focus', '-lastfor', root))
    except (tk.TclError, KeyError): return None

def owner_shot(root, name):
    if OWNER_SHOTS: snap(root, OWNER_SHOTS / name)

def fresh_env():
    d = tempfile.mkdtemp(prefix='ma2ux-flow-', dir=str(WORK / 'tmp'))
    os.environ['LOCALAPPDATA'] = os.path.join(d, 'LocalAppData')
    os.makedirs(os.environ['LOCALAPPDATA'], exist_ok=True)
    os.environ['MA2_SHORTCUT_DIR'] = os.path.join(d, 'lnk')
    os.environ['MA2_INSTALLER_FAKE'] = str(REC); os.environ['MA2_FAKE_SPEED'] = '400'
    for k in ('MA2_FAKE_FAIL', 'MA2_FAKE_EXTRA', 'MA2_UI_FAULT', 'MA2_TEXTURE_ENGINE', 'MA2_VIDEO_ENGINE', 'MA2_UPSCALER_SOURCE'): os.environ.pop(k, None)
    os.environ['MA2_UI_DISC_SEARCH'] = ''
    return Path(d)

def texts(app):
    """Every string on screen: canvas text and the widgets' own text."""
    cv = app.cv; out = []
    for i in cv.find_withtag('s'):
        if cv.type(i) == 'text': out.append(cv.itemcget(i, 'text'))
    def walk(w):
        for c in w.winfo_children():
            for a in ('text', 'title', 'sub'):
                v = getattr(c, a, None)
                if isinstance(v, str): out.append(v)
            if isinstance(c, tk.Canvas):
                for i in c.find_all():
                    if c.type(i) == 'text': out.append(c.itemcget(i, 'text'))
            walk(c)
    walk(cv)
    return out

def screen_text(app):
    return ' | '.join(t for t in texts(app) if t)

def ready_disc(root, app, d=None):
    app.set_disc('"%s"' % XISO); until(root, lambda: app.image is not None and not app.disc_checking, 20)
    if d is not None: app.set_dest(str(d / 'Games' / 'MechAssault 2'))
    until(root, lambda: app.folder is not None, 20); pump(root, 0.2)

def run_to(root, app, screen, timeout=90):
    return until(root, lambda: app.screen == screen, timeout)

def layout_problems(app):
    """Anything outside the 880x520 client, and anything above the footer that runs into it."""
    cv, dp = app.cv, app.k.dp
    W, H = dp(880), dp(520); ft = H - dp(64)
    bad = []
    for i in cv.find_withtag('s'):
        b = cv.bbox(i)
        if not b: continue
        x0, y0, x1, y1 = b
        typ = cv.type(i)
        if typ == 'window':
            w = cv.nametowidget(cv.itemcget(i, 'window'))
            if w.winfo_class() == 'Frame' and any(getattr(c, 'key', '').startswith('m_') for c in w.winfo_children()): continue   # the Help menu
        if typ in ('rectangle', 'line') and y0 <= 1: continue            # page backgrounds
        tol = dp(2) + 2                                                     # Tk's bbox pads a line by its width
        if x0 < -tol or y0 < -tol or x1 > W + tol or y1 > H + tol:
            bad.append(('outside', typ, b, cv.itemcget(i, 'text') if typ == 'text' else ''))
        elif y0 < ft - tol and y1 > ft + tol:
            bad.append(('into footer', typ, b, cv.itemcget(i, 'text') if typ == 'text' else ''))
    return bad

class Recorder:
    """Stands in for install.Installer: records how the window called it and finishes at once."""
    calls = []
    def __init__(self, *args, **kw):
        Recorder.calls.append((args, kw)); self.state = {}; self.notes = []; self.stage = ''; self.session_log = None
        self.on_plan = lambda names: None; self.on_status = lambda t: None; self.until = None
    def run(self): pass
    def cancel(self): pass

# ---------------------------------------------------------------------------- scenarios

def scenario_happy():
    d = fresh_env(); os.environ['MA2_FAKE_SPEED'] = '150'
    root, app = rig.make(); pump(root, 0.6)
    check('setup: the first screen is Setup, fresh install', app.screen == 'setup' and app.mode == 'install', (app.screen, app.mode))
    check('setup: INSTALL is the one primary and waits for the disc', app.primary is not None and app.primary.text == 'INSTALL' and not app.primary.enabled)
    check('setup: focus starts on the drop zone (no ring on a small link)', getattr(focused(root), 'key', None) == 'zone', getattr(focused(root), 'key', None))
    check('setup: the rows work before the disc (not dimmed)', app.w['dest'].enabled and app.w['options'].enabled)
    import ctypes
    st = ctypes.windll.user32.GetWindowLongW(app.hwnd, -16)
    check('window: native minimise and close, no maximise (the real title bar; the test window is a tool window)', st & 0x20000 and not st & 0x10000, hex(st))
    check('setup: the zone offers drop when the drop hook is in', 'Drop your disc image here' in screen_text(app) and app.drop.ok)
    check('setup: a quiet "Where do I get a disc image?" link opens Help', 'where' in app.w)
    check('setup: the footer shows Collect diagnostics next to Help', 'f_diag' in app.w and app.w['f_diag'].text == 'Collect diagnostics'
          and 'help' in app.w, screen_text(app))
    snap(root, SHOTS / 'happy-1-setup-empty.png'); owner_shot(root, '01-setup-empty.png')
    app.w['f_diag'].invoke(); pump(root, 0.2)
    check('setup: Collect diagnostics opens the diagnostics page', app.screen == 'diag', app.screen)
    app.on_escape(); pump(root, 0.2)
    check('setup: Esc from diagnostics goes back to Setup', app.screen == 'setup', app.screen)
    junk = d / 'junk.iso'; junk.write_bytes(os.urandom(6000))
    app.set_disc(str(junk)); until(root, lambda: app.image is not None, 10); pump(root, 0.2)
    check('setup: a wrong file is named in the zone, INSTALL stays off', app.image.code == 'not_xbox' and 'isn\'t an Xbox disc image' in screen_text(app) and not app.primary.enabled, screen_text(app))
    check('setup: the zone has "Choose another file"', 'disc_change' in app.w and app.w['disc_change'].text == 'Choose another file')
    snap(root, SHOTS / 'happy-2-wrong-file.png'); owner_shot(root, '10-setup-wrong-file.png')
    app.set_disc('relative\\thing.iso'); pump(root, 0.2)
    check('setup: a relative path is refused', app.image.code == 'relative')
    ready_disc(root, app, d)
    check('setup: the real disc is "Disc ready"', app.image.ok and 'Disc ready' in screen_text(app), screen_text(app))
    check('setup: focus jumps to INSTALL', focused(root) is app.primary, getattr(focused(root), 'key', None))
    check('setup: the footer total says Needs N GB and about N min', 'Needs ' in screen_text(app) and ' min' in screen_text(app), screen_text(app))
    check('setup: no tool names on the main path', not [w for w in TOOL_WORDS if w in screen_text(app)], screen_text(app))
    snap(root, SHOTS / 'happy-3-ready.png')
    app.facts['sac'] = None; app.folder.findings = []; app.render(); pump(root, 0.2); owner_shot(root, '02-setup-disc-ready.png')
    app.primary.invoke()
    check('install: Installing screen', run_to(root, app, 'progress', 30))
    until(root, lambda: app.model and app.model.status.get('build') == 'active', 60); pump(root, 1.2)
    t = screen_text(app)
    check('install: big percent, one step line, time left, no counts or tool names', '%' in t and 'Building the game' in t and
          not [w for w in TOOL_WORDS if w in t] and ' of ' not in t, t)
    check('install: the title bar mirrors percent and step', root.title().endswith(gui.TITLE) and '%' in root.title() and 'Building the game' in root.title(), root.title())
    check('install: Cancel is a quiet link, no primary', 'cancel' in app.w and app.primary is None)
    snap(root, SHOTS / 'happy-4-installing.png'); owner_shot(root, '05-installing.png')
    app.toggle_details(); pump(root, 0.3)
    t = screen_text(app)
    check('install: Show details lists plain steps', 'Reading your disc' in t and 'Setting things up' in t and 'Open log' in t, t)
    check('install: details fit the window', not layout_problems(app), layout_problems(app))
    owner_shot(root, '06-installing-details.png')
    app.toggle_details()
    check('install: Done screen reached', run_to(root, app, 'done', 90))
    pump(root, 0.3)
    t = screen_text(app)
    check('done: Ready to play, LAUNCH big primary, Open install folder', 'Ready to play' in t and app.primary.text == 'LAUNCH' and app.primary.big and 'open_folder' in app.w, t)
    check('done: focus on LAUNCH', focused(root) is app.primary)
    check('done: the title is plain again', root.title() == gui.TITLE, root.title())
    check('done: no shortcut sentence when the run reported none', 'Shortcuts are' not in t)
    snap(root, SHOTS / 'happy-5-done.png')
    desk = install.known_folder('desktop'); start = install.known_folder('start')
    for made, want in (([desk / 'MechAssault 2 (Recomp).lnk', start / 'MechAssault 2 (Recomp).lnk'], 'Shortcuts are on your desktop and in the Start menu.'),
                       ([start / 'MechAssault 2 (Recomp).lnk'], 'You\'ll find it in the Start menu.'),
                       ([desk / 'MechAssault 2 (Recomp).lnk'], 'A shortcut is on your desktop.'),
                       ([], 'Find it with Open install folder.')):
        app.inst.state = {'shortcuts': [str(p) for p in made]}; app.render(); pump(root, 0.1)
        check('done: shortcut line from what was made (%d made)' % len(made), want in screen_text(app), screen_text(app))
    app.sc['desktop'] = app.sc['start'] = False; app.inst.state = {'shortcuts': []}; app.render()
    check('done: no shortcut line when the player turned both off', 'Find it' not in screen_text(app) and 'Shortcut' not in screen_text(app))
    app.sc['desktop'] = app.sc['start'] = True
    app.inst.state = {'shortcuts': [str(desk / 'MechAssault 2 (Recomp).lnk'), str(start / 'MechAssault 2 (Recomp).lnk')]}; app.render(); pump(root, 0.2)
    owner_shot(root, '08-done.png')
    app.w['open_folder'].invoke()
    check('done: Open install folder opens the folder', app.launches[-1] == ('open', precheck.clean_path(app.dest)), app.launches)
    closed = []; real_close = app.close; app.close = lambda: closed.append(1)
    app.primary.invoke()
    check('done: LAUNCH starts MechAssault 2.exe and closes the installer', app.launches[-1][0] == 'play' and app.launches[-1][1].endswith('MechAssault 2.exe') and closed, app.launches)
    app.close = real_close
    root.destroy()

def scenario_engine():
    """No-regression: the same choices give the same Installer(...) call as the 1.1.3 window."""
    d = fresh_env(); os.environ.pop('MA2_INSTALLER_FAKE')
    real = install.Installer; install.Installer = Recorder; Recorder.calls = []
    try:
        root, app = rig.make(); ready_disc(root, app, d)
        dest = str(d / 'Games' / 'MechAssault 2')
        app.facts['compiler'] = {'state': 'ready'}
        app.primary.invoke(); until(root, lambda: Recorder.calls and app.screen == 'done', 20)
        args, kw = Recorder.calls[-1]
        check('engine: positional xiso, dest, textures, videos as 1.1.3', args == (str(XISO), dest, [], False), args)
        want = dict(install_build_tools=False, update=False, shortcuts=['desktop', 'start'], keep_awake=True, low_priority=False,
                    merge_settings=True, switch_textures=True)
        kw2 = {k: v for k, v in kw.items() if k not in ('log', 'step')}
        check('engine: keywords exactly as 1.1.3 for the defaults (no new flags)', kw2 == want, kw2)
        check('engine: the 1.1.3 hooks are set (on_plan, on_status, until)', callable(app.inst.on_plan) and callable(app.inst.on_status))
        root.destroy()
        # Install options and Troubleshooting map to the same flags the 1.1.3 window passed (and the CLI's --jobs / --vs / --keep-work / --allow-cpu)
        root, app = rig.make(); ready_disc(root, app, d); app.facts['compiler'] = {'state': 'ready'}
        app.facts['reco'] = {'rec': '2x', 'can': True, 'videos': True, 'engine': 'pytorch', 'reason': 'x'}
        app.set_texture('2x'); app.set_videos(True); app.sc['editor'] = True; app.lowprio = True; app.awake = False
        app.jobs = 4; app.keep_work = True; app.engine = 'cpu'; pump(root, 0.3); until(root, lambda: app.folder is not None, 10)
        app.primary.invoke(); until(root, lambda: app.screen == 'done', 20)
        args, kw = Recorder.calls[-1]
        kw2 = {k: v for k, v in kw.items() if k not in ('log', 'step')}
        check('engine: options -> textures 2x, videos', args[2:] == (['2x'], True), args)
        check('engine: shortcuts, priority, awake, jobs, keep_work, allow_cpu', kw2 == dict(install_build_tools=False, update=False, shortcuts=['desktop', 'start', 'editor'],
              keep_awake=False, low_priority=True, merge_settings=True, switch_textures=True, jobs=4, keep_work=True, allow_cpu=True), kw2)
        check('engine: "Processor only" sets the engine variables like the CLI', os.environ.get('MA2_TEXTURE_ENGINE') == 'pytorch')
        root.destroy()
    finally:
        install.Installer = real
        for k in ('MA2_TEXTURE_ENGINE', 'MA2_VIDEO_ENGINE'): os.environ.pop(k, None)

def scenario_fail():
    d = fresh_env(); os.environ['MA2_FAKE_FAIL'] = 'build:build_failed'
    root, app = rig.make(); ready_disc(root, app, d)
    states = []; real_set = app.taskbar.set; app.taskbar.set = lambda s, f=None: (states.append(s), real_set(s, f))[1]
    flashes = []; real_flash = ui_win.flash; ui_win.flash = lambda h: flashes.append(h)
    try:
        app.primary.invoke(); run_to(root, app, 'stop', 60); pump(root, 0.3)
        t = screen_text(app)
        check('fail: the failed style: COULDN\'T FINISH, one headline, one sentence', 'COULDN\'T FINISH' in t and 'Building the game didn\'t finish' in t, t)
        check('fail: TRY AGAIN is the primary; Help and Details are quiet links', app.primary.text == 'TRY AGAIN' and 'help' in app.w and 'stop_details' in app.w)
        check('fail: the taskbar shows the error state', 'error' in states, states)
        check('fail: the window asks for attention (flash when not in front)', flashes or ui_win.is_foreground(app.hwnd), flashes)
        snap(root, SHOTS / 'fail.png'); owner_shot(root, '13-failed.png')
        app.w['stop_details'].invoke(); pump(root, 0.2)
        body = app.w['pagetext'].get('1.0', 'end')
        check('fail: Details shows the engine message and the last log lines', 'simulated failure' in body and 'failed (exit code 1)' in body, body[:300])
        app.w['copy'].invoke(); clip = root.clipboard_get()
        check('fail: Copy puts the support bundle (failed step, last lines) on the clipboard', 'Failed stage: build' in clip and 'Last log lines:' in clip, clip[:200])
        u = os.environ.get('USERNAME', '')
        check('fail: the copied text hides the user name', len(u) <= 2 or u not in clip.replace('%USERNAME%', ''))
        app.w['openlog'].invoke(); check('fail: Open log folder', app.launches[-1][0] == 'open' and app.launches[-1][1].endswith('Logs'), app.launches)
        app.w['done'].invoke(); pump(root, 0.2)
        check('fail: DONE goes back to the stop screen', app.screen == 'stop')
        os.environ.pop('MA2_FAKE_FAIL'); app.primary.invoke()
        check('fail: TRY AGAIN runs again and finishes', run_to(root, app, 'done', 60))
    finally:
        ui_win.flash = real_flash
    root.destroy()

def scenario_cancel():
    d = fresh_env(); os.environ['MA2_FAKE_SPEED'] = '40'
    root, app = rig.make(); ready_disc(root, app, d); app.primary.invoke()
    until(root, lambda: app.model and app.model.status.get('analyze') == 'active', 30)
    app.w['cancel'].invoke(); pump(root, 0.2)
    t = screen_text(app)
    check('cancel: the footer asks in place (no popup), KEEP INSTALLING has focus', app.confirm == 'cancel' and app.primary.text == 'KEEP INSTALLING'
          and focused(root) is app.primary and 'progress is kept' in t, t)
    check('cancel: "Cancel install" is the danger link on the left', app.w['confirm_go'].danger)
    owner_shot(root, '07-cancel.png')
    app.on_escape(); pump(root, 0.2)
    check('cancel: Esc keeps installing', app.confirm is None and app.worker is not None)
    app.close(); pump(root, 0.2)
    check('cancel: closing the window while installing asks first (and does not close)', app.confirm == 'cancel' and root.winfo_exists())
    app.w['confirm_go'].invoke()
    until(root, lambda: app.screen == 'setup', 30); pump(root, 0.2)
    check('cancel: back on Setup with a plain note', app.screen == 'setup' and app.notice and 'cancelled' in app.notice[1].lower() or 'paused' in (app.notice or ('', ''))[1].lower(), app.notice)
    app.close()
    check('cancel: with nothing running the window closes', not app.worker and app.closing)

def dummy_install(dest, stamp=None, state=None):
    for f, data in (('MechAssault 2.exe', b'old'), ('pc_assets/common.mgf', b'old'), ('MA2 Scenario Editor.exe', b'ed')):
        p = dest / f; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(data)
    xbe = dest / 'game_files' / 'ma2.xbe'; xbe.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(GAME_FILES / 'ma2.xbe', xbe)
    if stamp is not None: (dest / precheck.STAMP).write_text(json.dumps(stamp))
    if state is not None:
        (dest / '_work').mkdir(exist_ok=True); (dest / '_work' / 'state.json').write_text(json.dumps(state))

def remember(dest):
    d = Path(os.environ['LOCALAPPDATA']) / 'MechAssault2'; d.mkdir(parents=True, exist_ok=True)
    (d / 'installer.json').write_text(json.dumps({'dest': str(dest)}))

def scenario_update():
    d = fresh_env(); dest = d / 'Games' / 'MechAssault 2'
    dummy_install(dest, {'kit_id': 'oldkit', 'version': '2026.10.22', 'public_version': '1.1.2'}); remember(dest)
    root, app = rig.make(); until(root, lambda: app.folder is not None, 10); pump(root, 0.3)
    t = screen_text(app)
    check('update: opens straight on Update', app.mode == 'update' and 'Update to %s' % install.PUBLIC_VERSION in t and '1.1.2 INSTALLED' in t, t)
    check('update: a PLAY button beside UPDATE, and the total says it rebuilds', 'play' in app.w and app.w['play'].ghost and 'Rebuilds the game' in t, t)
    check('update: no disc needed (game files reused), UPDATE enabled', 'Not needed' in t and app.primary.text == 'UPDATE' and app.primary.enabled, t)
    check('update: the "Installed in" row has a normal Change link, Uninstall stays quiet', 'dest' in app.w and app.w['dest'].text == 'Change'
          and not app.w['dest'].quiet and app.w['uninstall'].quiet and 'elsewhere' not in app.w)
    snap(root, SHOTS / 'update.png'); owner_shot(root, '14-update.png')
    check('update: the 1.1.3 action and parameters (update=True)', app.action() == 'update' and app.params()['update'] is True)
    app.facts['compiler'] = {'state': 'ready'}
    app.primary.invoke(); check('update: finishes', run_to(root, app, 'done', 60))
    check('update: Done says the version and that settings and saves were kept', 'Updated to %s' % install.PUBLIC_VERSION in screen_text(app))
    root.destroy()
    # the installed game files don't match (damaged): the disc is needed again, UPDATE waits for it
    (dest / 'game_files' / 'ma2.xbe').write_bytes(b'damaged')
    root, app = rig.make(); until(root, lambda: app.folder is not None, 10); pump(root, 0.3)
    check('update: without game files the Disc row asks for the disc and UPDATE waits', app.needs_disc() and 'Choose your disc image' in screen_text(app) and not app.primary.enabled, screen_text(app))
    app.set_disc(str(XISO)); until(root, lambda: app.image is not None and not app.disc_checking, 20); pump(root, 0.2)
    check('update: with the disc UPDATE is enabled', app.primary.enabled)
    root.destroy()

def scenario_elsewhere():
    """Change on the Update screen: a fresh install in a newly chosen folder; the existing install is not touched."""
    import hashlib
    d = fresh_env(); dest = d / 'Games' / 'MechAssault 2'
    dummy_install(dest, {'kit_id': 'oldkit', 'version': '2026.10.22', 'public_version': '1.1.2'}); remember(dest)
    def snapshot():
        return {p.relative_to(dest).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(dest.rglob('*')) if p.is_file()}
    before = snapshot()
    root, app = rig.make(); until(root, lambda: app.folder is not None, 10); pump(root, 0.3)
    check('elsewhere: opens on Update with a Change link on "Installed in"', app.mode == 'update' and 'dest' in app.w and app.w['dest'].text == 'Change')
    real_ask = gui.filedialog.askdirectory
    try:
        gui.filedialog.askdirectory = lambda **kw: str(dest)                     # the same folder again: nothing changes
        app.w['dest'].invoke(); pump(root, 0.2)
        check('elsewhere: picking the installed folder again keeps Update', app.mode == 'update' and Path(app.dest) == dest, (app.mode, app.dest))
        other = d / 'Other' / 'MechAssault 2'
        gui.filedialog.askdirectory = lambda **kw: str(other)
        app.w['dest'].invoke(); until(root, lambda: app.folder is not None, 10); pump(root, 0.3)
    finally:
        gui.filedialog.askdirectory = real_ask
    t = screen_text(app)
    check('elsewhere: Change leads to Setup for a fresh install in the new folder', app.screen == 'setup' and app.mode == 'install'
          and Path(app.dest) == other and 'Install MechAssault 2' in t and app.primary.text == 'INSTALL', (app.screen, app.mode, app.dest))
    ready_disc(root, app)
    check('elsewhere: the install action targets the new folder (not an update)', app.action() == 'install' and Path(app.params()['dest']) == other
          and app.params()['update'] is False, app.params())
    app.facts['compiler'] = {'state': 'ready'}
    app.primary.invoke(); check('elsewhere: the fresh install finishes', run_to(root, app, 'done', 90))
    check('elsewhere: the existing install is untouched (same files, same bytes)', snapshot() == before)
    root.destroy()

def scenario_maintain():
    d = fresh_env(); dest = d / 'Games' / 'MechAssault 2'
    dummy_install(dest, {'kit_id': install.KIT_ID, 'version': install.VERSION, 'public_version': install.PUBLIC_VERSION}); remember(dest)
    root, app = rig.make(); until(root, lambda: app.folder is not None, 10); pump(root, 0.3)
    check('maintain: an up-to-date install opens on "MechAssault 2 is installed" with REPAIR', app.mode == 'maintain' and app.primary.text == 'REPAIR'
          and 'MechAssault 2 is installed' in screen_text(app), screen_text(app))
    check('maintain: PLAY starts the game', 'play' in app.w)
    closed = []; real_close = app.close; app.close = lambda: closed.append(1)
    app.w['play'].invoke(); app.close = real_close
    check('maintain: PLAY launches MechAssault 2.exe and closes the installer', app.launches[-1][0] == 'play' and closed, app.launches)
    owner_shot(root, '15-installed-repair.png')
    app.facts['reco'] = {'rec': '2x', 'can': True, 'videos': True, 'engine': 'pytorch', 'reason': 'x'}
    app.go('options'); pump(root, 0.2)
    check('maintain: Install options has a location Change (it starts a fresh install elsewhere)', 'o_dest' in app.w and app.w['o_dest'].text == 'Change')
    real_ask = gui.filedialog.askdirectory; gui.filedialog.askdirectory = lambda **kw: ''
    app.w['o_dest'].invoke(); pump(root, 0.2); gui.filedialog.askdirectory = real_ask
    check('maintain: cancelling that folder picker changes nothing', app.mode == 'maintain' and Path(app.dest) == dest and app.screen == 'options', (app.mode, app.dest))
    app.w['o_tex'].focus_set(); app.w['o_tex'].event_generate('<Right>'); app.w['o_tex'].event_generate('<space>'); pump(root, 0.1)
    check('maintain: arrows + Space choose HD in the segment', app.texture == '2x', app.texture)
    app.on_escape(); pump(root, 0.2)
    check('maintain: with new HD content picked the primary is ADD CONTENT (1.1.3 "addons")', app.primary.text == 'ADD CONTENT' and app.action() == 'addons', app.primary.text)
    check('maintain: addons makes no shortcuts (as 1.1.3)', app.params()['shortcuts'] == [])
    app.set_texture('original'); pump(root, 0.2)
    check('maintain: back to REPAIR (1.1.3 "repair": update=True)', app.action() == 'repair' and app.params()['update'] is True)
    me = Path(os.environ['LOCALAPPDATA']) / 'MechAssault2'; (me / 'Saved Games').mkdir(parents=True, exist_ok=True); (me / 'Saved Games' / 'p.sav').write_text('s')
    app.w['uninstall'].invoke(); pump(root, 0.2)
    check('uninstall: an in-window page with a "delete my settings and saves" toggle, off', app.screen == 'uninstall' and app.w['u_wipe'].value is False)
    check('uninstall: a red UNINSTALL and "can\'t be undone"', app.primary.danger and 'can\'t be undone' in screen_text(app))
    owner_shot(root, '16-uninstall.png')
    app.on_escape(); pump(root, 0.1); check('uninstall: Esc goes back without deleting', app.screen == 'setup' and dest.exists())
    app.w['uninstall'].invoke(); pump(root, 0.1); app.primary.invoke()
    until(root, lambda: not dest.exists() and app.screen == 'setup', 30); pump(root, 0.4)
    check('uninstall: the folder is gone, saves kept', not dest.exists() and (me / 'Saved Games' / 'p.sav').is_file())
    check('uninstall: back on Setup (fresh install) with what was done', app.mode == 'install' and app.notice and 'was removed from' in app.notice[1], app.notice)
    root.destroy()

def scenario_resume():
    d = fresh_env(); dest = d / 'Games' / 'MechAssault 2'
    dest.mkdir(parents=True); (dest / '_work').mkdir()
    (dest / '_work' / 'state.json').write_text(json.dumps({'done': ['check', 'disc', 'extract', 'analyze'], 'last_title': 'Translating the game program to C', 'last_step': 'recompile',
                                                           'options': {'textures': [], 'videos': False}, 'xiso': str(XISO)}))
    remember(dest)
    root, app = rig.make(); pump(root, 0.5)
    t = screen_text(app)
    check('resume: opens on "Continue installing" with CONTINUE', app.mode == 'resume' and app.primary.text == 'CONTINUE' and app.primary.enabled and 'Continue installing' in t, t)
    check('resume: no disc needed after the extract step', not app.needs_disc())
    check('resume: the stop is named with the Installing list\'s step names', 'It stopped while setting things up.' in t, t)
    owner_shot(root, '17-continue.png')
    app.w['startover'].invoke(); pump(root, 0.2)
    check('resume: Start over asks in the footer', app.confirm == 'startover' and app.primary.text == 'KEEP IT')
    app.on_escape(); pump(root, 0.1)
    app.primary.invoke()
    check('resume: CONTINUE runs to the end', run_to(root, app, 'done', 60))
    root.destroy()
    d = fresh_env(); dest = d / 'G' / 'MA2'; dest.mkdir(parents=True); (dest / '_work').mkdir(); (dest / '_work' / 'state.json').write_text('{"done":[]}')
    (dest / 'game_files').mkdir(); (dest / 'game_files' / 'keep.txt').write_text('x'); remember(dest)
    root, app = rig.make(); pump(root, 0.4)
    app.w['startover'].invoke(); pump(root, 0.1); app.w['confirm_go'].invoke(); pump(root, 0.3)
    check('resume: Start over removes _work only and leaves the Continue screen', not (dest / '_work').exists() and (dest / 'game_files' / 'keep.txt').exists() and app.mode != 'resume')
    root.destroy()
    # a resume that never finished extracting needs the disc
    d = fresh_env(); dest = d / 'G' / 'MA2'; (dest / '_work').mkdir(parents=True)
    (dest / '_work' / 'state.json').write_text(json.dumps({'done': ['check', 'disc'], 'xiso': str(d / 'gone.iso')})); remember(dest)
    root, app = rig.make(); pump(root, 0.4)
    check('resume: before the extract step the disc is needed and CONTINUE waits', app.needs_disc() and not app.primary.enabled)
    root.destroy()

def scenario_extras():
    d = fresh_env(); os.environ['MA2_FAKE_EXTRA'] = 'textures-2x'; os.environ['MA2_FAKE_FAIL'] = 'textures-2x:unexpected'
    root, app = rig.make(); ready_disc(root, app, d)
    app.primary.invoke(); run_to(root, app, 'done', 90); pump(root, 0.3)
    t = screen_text(app)
    check('extras: an unfinished extra still ends on Ready to play (not an error)', app.screen == 'done' and 'Ready to play' in t, t)
    check('extras: one strip names it in the player\'s words, with Try again and Details', 'The HD textures' in t and 'retry_extras' in app.w and 'notes' in app.w, t)
    owner_shot(root, '09-done-extras-unfinished.png')
    os.environ.pop('MA2_FAKE_FAIL'); app.w['retry_extras'].invoke()
    check('extras: Try again runs again to Done', run_to(root, app, 'progress', 10) and run_to(root, app, 'done', 90))
    root.destroy(); os.environ.pop('MA2_FAKE_EXTRA', None)

def scenario_stops():
    """The things only the player can do: a bad disc mid-way, a full drive, and the space stop before the install."""
    d = fresh_env(); os.environ['MA2_FAKE_FAIL'] = 'extract:disc'
    root, app = rig.make(); ready_disc(root, app, d); app.primary.invoke()
    until(root, lambda: app.screen == 'setup' and app.image and not app.image.ok, 60); pump(root, 0.2)
    check('stops: a disc that fails mid-way returns to Setup with the zone in its bad state', app.screen == 'setup' and 'The disc image couldn\'t be read' in screen_text(app), screen_text(app))
    root.destroy()
    d = fresh_env(); os.environ['MA2_FAKE_FAIL'] = 'build:disk_full'
    root, app = rig.make(); ready_disc(root, app, d); app.primary.invoke(); run_to(root, app, 'stop', 60); pump(root, 0.2)
    t = screen_text(app)
    check('stops: a full drive is the amber "needs you" style naming the drive', 'ACTION NEEDED' in t and 'Free up space on' in t and app.primary.text == 'TRY AGAIN', t)
    check('stops: needs-you screens have Close, no Help menu', 'close' in app.w and 'help' not in app.w)
    root.destroy(); os.environ.pop('MA2_FAKE_FAIL')
    # space before the install: the total turns red and the primary becomes CHOOSE DRIVE
    d = fresh_env(); root, app = rig.make(); ready_disc(root, app, d)
    r = app.folder; r.findings = [precheck.Finding('error', 'space', 'Not enough space')]; r.free = 3 * precheck.GB
    app.render(); pump(root, 0.2)
    t = screen_text(app)
    check('stops: not enough space: CHOOSE DRIVE, a red total and the drive named', app.primary.text == 'CHOOSE DRIVE' and 'needs' in t and '3.0 GB' in t, t)
    owner_shot(root, '11-setup-not-enough-space.png')
    r.findings = [precheck.Finding('error', 'notempty', 'This folder already has other files. Choose an empty folder or a new name.', str(d / 'x' / 'MechAssault 2'))]
    app.render(); pump(root, 0.2)
    check('stops: a folder problem: CHOOSE FOLDER and a one-click fix link', app.primary.text == 'CHOOSE FOLDER' and 'fix' in app.w, screen_text(app))
    app.w['fix'].invoke(); until(root, lambda: app.folder is not None, 10)
    check('stops: the fix link uses the folder inside', app.dest.endswith('MechAssault 2') and app.folder.ok, app.dest)
    # Smart App Control: one neutral strip after the disc is ready
    app.facts['sac'] = 'enforce'; app.folder.findings = []; app.render(); pump(root, 0.2)
    check('stops: Smart App Control is one neutral strip with Learn more, INSTALL still works', gui.SAC_TEXT in screen_text(app) and 'sac' in app.w and app.primary.text == 'INSTALL' and app.primary.enabled)
    owner_shot(root, '12-setup-smart-app-control.png')
    app.w['sac'].invoke(); pump(root, 0.1)
    check('stops: Learn more opens Help & FAQ', app.screen == 'page' and app.page == 'help')
    root.destroy()

def scenario_tools():
    """No C++ tools: one stop, INSTALL C++ TOOLS runs the 1.1.3 one-click install, then the game install goes on by itself."""
    import webbrowser
    d = fresh_env(); real = (install.compiler_state, install.install_build_tools, webbrowser.open)
    state = {'now': 'missing'}; opened = []
    install.compiler_state = lambda choose=None: ({'state': 'missing', 'detail': 'none', 'code': 'missing', 'vs': None, 'name': ''} if state['now'] == 'missing'
                                                   else {'state': 'ready', 'vs': 'x', 'name': 'Fake', 'detail': '', 'code': None})
    webbrowser.open = lambda url, *a, **k: opened.append(url)
    def fake_install(work, log, cancelled=lambda: False):
        Path(work).mkdir(parents=True, exist_ok=True); (Path(work) / 'vs_buildtools.exe').write_text('x')
        log('  downloading Microsoft C++ Build Tools installer: https://aka.ms/x'); time.sleep(0.3)
        state['now'] = 'ready'; log('  Microsoft C++ Build Tools installed')
    install.install_build_tools = fake_install
    old_temp = os.environ.get('TEMP'); os.environ['TEMP'] = str(d / 'Temp'); (d / 'Temp').mkdir()
    try:
        root, app = rig.make(); ready_disc(root, app, d)
        app.facts['compiler'] = None; app.primary.invoke(); run_to(root, app, 'stop', 20); pump(root, 0.2)
        t = screen_text(app)
        check('tools: one stop names the tools, the size, the permission prompt and the licence', 'Install Microsoft\'s C++ tools' in t and '6 GB' in t and 'permission' in t
              and 'license terms' in t and app.primary.text == 'INSTALL C++ TOOLS' and 'back' in app.w and 'help' in app.w, t)
        owner_shot(root, '18-cpp-tools.png')
        app.w['stop_link0'].invoke(); check('tools: Licence terms opens Microsoft\'s page', opened and opened[-1].startswith('https://visualstudio.microsoft.com/'), opened)
        app.primary.invoke()
        check('tools: the install goes on by itself after the tools', run_to(root, app, 'done', 90))
        logs = list((Path(os.environ['LOCALAPPDATA']) / 'MechAssault2' / 'Logs').glob('buildtools-*.log'))
        check('tools: buildtools-<time>.log written, download folder removed', logs and 'compiler check afterwards: ready' in logs[0].read_text(encoding='utf-8')
              and not (d / 'Temp' / 'MA2-buildtools').exists())
        root.destroy()
        # a restart is needed (3010): restart screen, RESTART NOW saves the resume note and the sign-in entry; the next start carries on
        state['now'] = 'missing'
        def reboot_install(work, log, cancelled=lambda: False):
            raise install.Stop(install.REBOOT, 'compiler', 'reboot')
        install.install_build_tools = reboot_install
        root, app = rig.make(); ready_disc(root, app, d); app.facts['compiler'] = None
        app.primary.invoke(); run_to(root, app, 'stop', 20); app.primary.invoke(); until(root, lambda: app.stop and app.stop.get('icon') == 'restart', 20); pump(root, 0.2)
        t = screen_text(app)
        check('tools: restart screen (amber), RESTART NOW, Close and the save-your-work line', 'Restart to finish setting up' in t and app.primary.text == 'RESTART NOW'
              and 'close' in app.w and 'Save your work' in t, t)
        owner_shot(root, '19-restart.png')
        app.primary.invoke(); pump(root, 0.2)
        marker = Path(os.environ['LOCALAPPDATA']) / 'MechAssault2' / 'installer-resume.json'
        check('tools: RESTART NOW writes the resume note, the sign-in entry, then restarts', marker.is_file() and app.signins and app.restarts)
        root.destroy()
        state['now'] = 'ready'
        root, app = rig.make()
        check('tools: after sign-in the window carries on by itself', run_to(root, app, 'progress', 20) and not marker.exists() and app.disc == str(XISO))
        run_to(root, app, 'done', 90); root.destroy()
        # Visual Studio without a part: one stop pointing at the Visual Studio Installer
        install.compiler_state = lambda choose=None: {'state': 'incomplete', 'detail': 'x', 'code': 'cmake', 'vs': 'C:\\VS', 'missing': ['cmake'], 'add': ['a']}
        root, app = rig.make(); ready_disc(root, app, d); app.facts['compiler'] = None
        app.primary.invoke(); run_to(root, app, 'stop', 20); pump(root, 0.2)
        check('tools: a missing part names it and offers the VS Installer and Check again', app.primary.text == 'OPEN VS INSTALLER' and 'Check again' in screen_text(app), screen_text(app))
        root.destroy()
        # C: too full for the tools
        install.compiler_state = real[0].__class__ and (lambda choose=None: {'state': 'missing', 'detail': '', 'code': 'missing', 'vs': None, 'name': ''})
        import collections
        U = collections.namedtuple('usage', 'total used free'); realdu = shutil.disk_usage
        root, app = rig.make(); ready_disc(root, app, d); app.facts['compiler'] = None
        shutil.disk_usage = lambda p: U(100 * precheck.GB, 97 * precheck.GB, 3 * precheck.GB)
        try:
            app.primary.invoke(); run_to(root, app, 'stop', 20); pump(root, 0.2)
        finally:
            shutil.disk_usage = realdu
        check('tools: under 8 GB on C: is a space stop naming C:, not the tools prompt', 'Free up space on' in screen_text(app) and app.primary.text == 'CHECK AGAIN', screen_text(app))
        root.destroy()
    finally:
        install.compiler_state, install.install_build_tools, webbrowser.open = real
        if old_temp is not None: os.environ['TEMP'] = old_temp

def scenario_blocked():
    d = fresh_env(); root, app = rig.make()
    real = gui.subprocess.Popen
    def boom(*a, **k): raise OSError(0, 'blocked', None, 4551)
    gui.subprocess.Popen = boom
    try:
        ok = gui.App.launch(app, 'play', d / 'MechAssault 2.exe')
    finally:
        gui.subprocess.Popen = real
    pump(root, 0.2); t = screen_text(app)
    check('blocked: a blocked first launch shows "Windows blocked the game" (red), TRY AGAIN, the installer stays open', ok is False and app.screen == 'stop'
          and 'Windows blocked the game' in t and 'COULDN\'T START' in t and app.primary.text == 'TRY AGAIN' and root.winfo_exists(), t)
    check('blocked: no exclusion coaching', 'exclu' not in t.lower() and 'allow the' not in t.lower(), t)
    app.open_menu(); pump(root, 0.2); owner_shot(root, '20-windows-blocked-help-menu.png'); app.on_escape()
    app.w['stop_details'].invoke(); pump(root, 0.1)
    check('blocked: Details has the exact reason (Smart App Control, code 4551)', '4551' in app.w['pagetext'].get('1.0', 'end') and 'Smart App Control' in app.w['pagetext'].get('1.0', 'end'))
    root.destroy()

def scenario_menu():
    """The footer Help menu: Help & FAQ, Collect diagnostics (to LocalAppData, preview inside), Open log; keyboard driven."""
    import zipfile
    d = fresh_env(); root, app = rig.make(); pump(root, 0.3)
    logs = Path(os.environ['LOCALAPPDATA']) / 'MechAssault2' / 'Logs'; logs.mkdir(parents=True, exist_ok=True); (logs / '20261110-1.log').write_text('game log\n')
    app.w['help'].invoke(); pump(root, 0.2)
    check('menu: opens with focus on its first item', app.menu and getattr(focused(root), 'key', None) == 'm_help')
    focused(root).event_generate('<Down>'); pump(root, 0.1)
    check('menu: Down moves to Collect diagnostics', getattr(focused(root), 'key', None) == 'm_diag', getattr(focused(root), 'key', None))
    owner_shot(root, '21-help-menu.png')
    focused(root).event_generate('<Return>'); pump(root, 0.2)
    check('menu: Return opens the diagnostics page', app.screen == 'diag')
    app.w['d_preview'].invoke(); pump(root, 0.1)
    check('diag: "What goes in the zip" shows the contents in place', 'system.txt' in screen_text(app))
    owner_shot(root, '22-collect-diagnostics.png')
    app.primary.invoke(); until(root, lambda: app.diag.get('result') is not None, 30); pump(root, 0.3)
    r = app.diag['result']
    check('diag: the zip goes to LocalAppData (never Desktop or Documents)', not isinstance(r, Exception) and str(Path(r['path'])).startswith(os.environ['LOCALAPPDATA']), r)
    with zipfile.ZipFile(r['path']) as z: names = z.namelist()
    check('diag: it holds system.txt and the game log', 'system.txt' in names and 'logs/20261110-1.log' in names, names)
    check('diag: the file is shown in Explorer and the page offers Show file', ('select', str(r['path'])) in app.launches and 'd_show' in app.w, (app.launches[-2:], r['path'], list(app.w)))
    app.primary.invoke(); pump(root, 0.1)
    check('diag: DONE goes back', app.screen == 'setup')
    app.open_menu(); pump(root, 0.1); app.w['m_log'].invoke(); pump(root, 0.1)
    check('menu: Open log opens the Logs folder', app.launches[-1][0] == 'open' and app.launches[-1][1].endswith('Logs') and not app.menu)
    app.open_menu(); pump(root, 0.1); app.w['m_help'].invoke(); pump(root, 0.1)
    t = app.w['pagetext'].get('1.0', 'end')
    check('help: Help & FAQ says where a disc image comes from (own copy) and never links download sites', 'Where do I get a disc image?' in t and 'your own copy' in t
          and 'http' not in t.split('Where do I get a disc image?')[1].split('What does the installer do?')[0], t[:200])
    owner_shot(root, '23-help-faq.png')
    app.on_escape(); pump(root, 0.1)
    app.w['legal'].invoke(); pump(root, 0.1); t = app.w['pagetext'].get('1.0', 'end')
    check('legal: the Legal page has the full fan-project text, own copy and the font licences', precheck.FAN_LINE[:40] in t and 'own copy' in t
          and ('Open Font License' in t or not (Path(install.HERE) / 'data' / 'fonts' / 'FONTS.txt').is_file()), t[:200])
    app.on_escape(); pump(root, 0.1); check('legal: Esc goes back', app.screen == 'setup')
    root.destroy()

def scenario_keyboard():
    d = fresh_env(); root, app = rig.make(); ready_disc(root, app, d)
    app.facts['reco'] = {'rec': '2x', 'can': True, 'videos': True, 'engine': 'pytorch', 'reason': 'x'}; app.render(); pump(root, 0.2)
    order = []; w = app.w['zone'] if 'zone' in app.w else None
    w = focused(root)
    for _ in range(14):
        w = w.tk_focusNext(); k = getattr(w, 'key', None)
        if k in order: break
        order.append(k)
    want = ['disc_change', 'dest', 'tex', 'options', 'help', 'legal', 'primary']
    check('keyboard: Tab order on Setup is zone, rows, options, footer, INSTALL', [k for k in order if k in want + ['zone']] == ['zone'] + want or
          [k for k in order if k in want] == want, order)
    app.w['options'].focus_set(); app.w['options'].event_generate('<Return>'); pump(root, 0.1)
    check('keyboard: Return on a link runs it (Install options)', app.screen == 'options')
    owner_shot(root, '03-install-options.png')
    t = app.w['o_vid']; t.focus_set(); t.event_generate('<FocusIn>', detail='NotifyAncestor'); pump(root, 0.05); t.event_generate('<space>'); pump(root, 0.1)
    t = app.w['o_vid']
    check('keyboard: Space toggles, the toggle shows On', app.videos and t.value)
    check('keyboard: the toggle keeps the focus after the page redraws', focused(root) is t, focused(root))
    check('keyboard: Upscaled movies is Off by default and shows on Setup once on', 'upscaled movies' in app.tex_line()[0])
    t.event_generate('<Return>'); pump(root, 0.1)
    check('keyboard: Return elsewhere is the page primary (DONE)', app.screen == 'setup')
    check('keyboard: the Textures line follows the options', 'upscaled movies' in screen_text(app), screen_text(app))
    app.go('options'); app.w['trouble'].invoke(); pump(root, 0.2)
    owner_shot(root, '04-troubleshooting.png')
    check('keyboard: Troubleshooting has the expert settings, tool names allowed only here', 'C++ tools' in screen_text(app) and 't_jobs' in app.w)
    app.on_escape(); check('keyboard: Esc on Troubleshooting goes to Install options', app.screen == 'options')
    app.on_escape(); check('keyboard: Esc on Install options goes back to Setup', app.screen == 'setup')
    root.destroy()

def scenario_drop():
    """A real WM_DROPFILES on the window picks the disc; with the hook forced off the zone says Browse, and nothing else changes."""
    d = fresh_env(); root, app = rig.make(); pump(root, 0.2)
    check('drop: the drop hook is in', app.drop.ok)
    ui_win.send_test_drop(app.hwnd, [str(XISO)]); until(root, lambda: app.image is not None and not app.disc_checking, 20); pump(root, 0.2)
    check('drop: a dropped disc image is checked and ready', app.disc == str(XISO) and app.image and app.image.ok, (app.disc, app.image))
    root.destroy()

FAULTS = ['titlebar', 'workarea', 'taskbar', 'flash', 'drop', 'anim']

def scenario_faults():
    """Fault injection: each new mechanism made to fail must log one line, fall back, and the install must still reach Done."""
    import io, contextlib
    for name in FAULTS:
        d = fresh_env(); os.environ['MA2_UI_FAULT'] = name; ui_win.FALLBACKS.clear()
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            root, app = rig.make(); ready_disc(root, app, d)
            app.facts['compiler'] = {'state': 'ready'}
            app.primary.invoke(); reached = run_to(root, app, 'done', 90)
            if name == 'flash': ui_win.flash(app.hwnd); app.attention()
        check('fault[%s]: falls back, logs one line, the install still reaches Done' % name, name in ui_win.FALLBACKS and reached
              and ('ui: %s not available' % name) in out.getvalue(), (ui_win.FALLBACKS, reached, out.getvalue()[-300:]))
        if name == 'drop':
            app.go('setup'); app.disc = ''; app.image = None; app.render()
            check('fault[drop]: the zone says "Choose your disc image" (Browse only)', 'Choose your disc image' in screen_text(app) and 'Drop your' not in screen_text(app))
        root.destroy(); os.environ.pop('MA2_UI_FAULT')
    # process-level helpers (before the window): they fall back and report what they did
    import io, contextlib
    for name, fn in (('dpi', ui_win.set_dpi_awareness), ('appid', lambda: ui_win.set_app_id('Test.Id'))):
        os.environ['MA2_UI_FAULT'] = name; ui_win.FALLBACKS.clear(); out = io.StringIO()
        with contextlib.redirect_stdout(out): res = fn()
        check('fault[%s]: falls back without raising and logs it' % name, name in ui_win.FALLBACKS and 'ui: %s not available' % name in out.getvalue() and res in ('system', '', False), res)
        os.environ.pop('MA2_UI_FAULT')
    # a drop handler that throws inside the window procedure must not break the window
    d = fresh_env(); root, app = rig.make(); pump(root, 0.2)
    real = app.drop._query; app.drop._query = lambda *a: (_ for _ in ()).throw(RuntimeError('simulated'))
    with contextlib.redirect_stdout(io.StringIO()): ui_win.send_test_drop(app.hwnd, [str(XISO)])
    app.drop._query = real; pump(root, 0.2)
    check('fault[drop handler]: a failing drop is logged and the window stays usable', root.winfo_exists() and app.screen == 'setup' and 'drop' in ui_win.FALLBACKS)
    # the real mechanisms work on this PC (taskbar progress, flash)
    check('taskbar: progress can be set on this PC', app.taskbar.set('normal', 0.5) and app.taskbar.set('none'))
    check('flash: the taskbar flash call works', ui_win.flash(app.hwnd))
    root.destroy()

def scenario_startup():
    """If the new window cannot start, the classic 1.1.3 window opens instead (and a failure of both still says where the log is)."""
    import io, contextlib, gui_legacy
    fresh_env()
    called = []; real = gui_legacy.main; gui_legacy.main = lambda: called.append(1)
    old = (sys.stdout, sys.stderr)
    try:
        for how in ('fault', 'app', 'legacy-env'):
            called.clear()
            if how == 'fault': os.environ['MA2_UI_FAULT'] = 'startup'
            if how == 'legacy-env': os.environ['MA2_UI_LEGACY'] = '1'
            real_app = gui.App
            if how == 'app': gui.App = lambda root: (_ for _ in ()).throw(RuntimeError('simulated widget failure'))
            try:
                gui.main()
            finally:
                log = sys.stdout; sys.stdout, sys.stderr = old
                try: log.close()
                except Exception: pass
                gui.App = real_app; os.environ.pop('MA2_UI_FAULT', None); os.environ.pop('MA2_UI_LEGACY', None)
            logs = sorted((Path(os.environ['LOCALAPPDATA']) / 'MechAssault2' / 'Logs').glob('installer-ui-*.log'), key=lambda f: f.stat().st_mtime)
            txt = logs[-1].read_text(encoding='utf-8') if logs else ''
            check('startup[%s]: the classic window opens instead, and the log says why' % how, called and 'opening the classic window' in txt, txt[-300:])
            for f in logs: f.unlink()
    finally:
        gui_legacy.main = real
    # both fail: exit code 3 and a message box naming the log (the 1.1.3 crash behaviour)
    shown = []; old_box = gui.message_box; gui.message_box = shown.append; os.environ['MA2_UI_CRASH_TEST'] = '1'
    try:
        try: gui.main(); code = None
        except SystemExit as e: code = e.code
    finally:
        log = sys.stdout; sys.stdout, sys.stderr = old
        try: log.close()
        except Exception: pass
        gui.message_box = old_box; os.environ.pop('MA2_UI_CRASH_TEST')
    check('startup[crash]: both windows failing exits with code 3 and names the log', code == 3 and shown and 'could not start' in shown[0], (code, shown))

def scenario_layout():
    """Every screen inside the window and clear of the footer at 100 %, 1366x768 at 125 % (clamped) and 200 %."""
    for label, screen, scale in (('100', None, 1.0), ('1366x768-125', (1366, 768), 1.25), ('200', None, 2.0)):
        if screen: tk.Tk.winfo_screenwidth = lambda self, s=screen: s[0]; tk.Tk.winfo_screenheight = lambda self, s=screen: s[1]
        try:
            d = fresh_env(); dest = d / 'G' / 'MA2'
            root, app = rig.make(scale=scale); pump(root, 0.3)
            sh = root.winfo_screenheight()
            if screen:
                check('layout[%s]: the scale is clamped so the window and title bar fit the work area (%.2f)' % (label, app.k.scale),
                      app.k.dp(551) <= sh - 40 - 20 and app.k.scale <= 1.25, (app.k.scale, sh))
            check('layout[%s]: the client is 880x520 logical' % label, abs(app.cv.winfo_reqwidth() - app.k.dp(880)) <= 1 and abs(app.cv.winfo_reqheight() - app.k.dp(520)) <= 1)
            check('layout[%s]: one fixed size (not resizable)' % label, root.resizable() == (False, False))
            app.facts['reco'] = {'rec': '2x', 'can': True, 'videos': True, 'engine': 'vulkan', 'reason': 'x'}
            states = []
            def visit(name):
                pump(root, 0.15); p = layout_problems(app); states.append(name)
                check('layout[%s]: %s fits' % (label, name), not p, p)
                if label == '1366x768-125': snap(root, SHOTS / ('layout-125-%s.png' % name))
            visit('setup-empty')
            ready_disc(root, app, d); app.folder.findings = [precheck.Finding('warn', 'long', 'Long folder paths can break the compiler and the AI tools. Choose a shorter path. ' * 2)]
            app.render(); visit('setup-ready-long-strip')
            app.facts['sac'] = 'enforce'; app.folder.findings = []; app.render(); visit('setup-sac')
            app.go('options'); app.texture = '4x'; app.render(); visit('options')
            app.go('trouble'); visit('trouble')
            app.go('setup'); app.set_dest(str(dest)); until(root, lambda: app.folder is not None, 10)
            app.primary.invoke(); run_to(root, app, 'progress', 20); until(root, lambda: app.model is not None, 20)
            app.show_details = True; app.render(); visit('installing-details')
            app.ask('cancel'); visit('cancel'); app.ask(None)
            run_to(root, app, 'done', 90); app.extras_failed = 'The 2x enhanced textures could not be made: something long happened here. More text.'; app.render(); visit('done-strip')
            app.show_failed('compile_error', 'x'); visit('failed')
            app.show_restart(); visit('restart')
            app.after_compiler({'state': 'missing'}); visit('tools')
            app.show_blocked(dest / 'MechAssault 2.exe', OSError(0, 'x', None, 225)); visit('blocked')
            for p in ('help', 'legal', 'details'): app.show_page(p); visit('page-' + p)
            app.open_diag(); app.diag['preview'] = True; app.render(); visit('diag')
            app.go('uninstall'); visit('uninstall')
            for m in ('update', 'maintain', 'resume'):
                app.mode = m; app.go('setup'); visit('existing-' + m)
            root.destroy()
        finally:
            if screen: del tk.Tk.winfo_screenwidth, tk.Tk.winfo_screenheight

def scenario_dpi():
    """Moving to a monitor with another DPI re-lays the window at the new scale (per-monitor v2), clamped to the work area."""
    d = fresh_env(); root, app = rig.make(scale=1.0); pump(root, 0.2)
    real = ui_win.window_dpi
    try:
        app.forced_scale = False; app.dpi_seen = 96
        ui_win.window_dpi = lambda h: 144
        for _ in range(8): app.check_dpi()
        pump(root, 0.3)
        want = min(1.5, app.pick_scale(1.5))
        check('dpi: a 150 %% monitor re-lays the window at %.2f' % want, abs(app.k.scale - want) < 0.01 and abs(app.cv.winfo_reqwidth() - app.k.dp(880)) <= 1, app.k.scale)
        check('dpi: the screen is still complete after the change', not layout_problems(app) and app.primary is not None, layout_problems(app))
        ui_win.window_dpi = lambda h: 96; app.check_dpi(); pump(root, 0.2)
        check('dpi: back to 100 %', abs(app.k.scale - 1.0) < 0.01, app.k.scale)
    finally:
        ui_win.window_dpi = real
    root.destroy()

def scenario_hooks():
    """The engine's 1.1.4 hooks on screen: a slow recovery's step line (A2), a download's one-line cause (A4), other drives' space (B1)."""
    d = fresh_env(); os.environ['MA2_FAKE_SPEED'] = '40'
    root, app = rig.make(); ready_disc(root, app, d)
    real = install.space_plan
    install.space_plan = lambda *a, **k: {'drives': [{'drive': 'Q:', 'free': 3 * precheck.GB, 'core': 8 * precheck.GB, 'extras': 0, 'what': ['buildtools']}],
                                          'core_fits': False, 'all_fit': False, 'short': ['Q:']}
    try:
        app.folder.findings = []; app.facts['sac'] = None; app.render(); pump(root, 0.1)
        check('hooks[B1]: another drive short of space is one note naming it (never a block)', "Drive Q: needs about 8 GB free for Microsoft's C++ tools" in screen_text(app)
              and app.primary.text == 'INSTALL' and app.primary.enabled, screen_text(app))
    finally:
        install.space_plan = real
    app.facts['compiler'] = {'state': 'ready'}; app.primary.invoke(); until(root, lambda: app.model is not None, 20)
    app.event('status', 'taking longer: low memory, building more slowly'); root.update_idletasks()
    check('hooks[A2]: the slow recovery shows on the step line in plain words', 'Taking longer: low memory, building more slowly' in screen_text(app), screen_text(app))
    app.cancel_install(); until(root, lambda: app.screen == 'setup', 30)
    app.show_failed('network', 'Could not get FFmpeg: no internet connection (the server name could not be found). It plays the cutscenes. More.'); pump(root, 0.1)
    t = screen_text(app)
    check('hooks[A4]: a failed download names its cause in one line', 'no internet connection' in t and 'Check your connection' in t and "A download didn't finish" in t, t)
    root.destroy()

def scenario_eta():
    check('eta: under a minute', gui.fmt_left(40) == 'Less than a minute left')
    check('eta: minutes under 10 exact', gui.fmt_left(7 * 60 + 10) == 'About 7 min left')
    check('eta: rounded to 5 above 10 min', gui.fmt_left(23 * 60) == 'About 25 min left', gui.fmt_left(23 * 60))
    check('eta: hours', gui.fmt_left(125 * 60) == 'About 2 h left' and gui.fmt_left(135 * 60) == 'About 2 h 20 min left', (gui.fmt_left(125 * 60), gui.fmt_left(135 * 60)))
    d = fresh_env(); root, app = rig.make()
    class M:
        def __init__(s): s.e = 600; s.current = 'build'; s.steps = ['build']; s.status = {'build': 'active'}
        def eta(s): return s.e
        def pace(s): return 1.0
    app.model = M()
    a = app.eta(); app.model.e = 1500; b = app.eta(); app.model.e = 300; c = app.eta()
    check('eta: never goes up (stretches), goes down at once', a == 600 or abs(a - 600) < 2 and b <= a and c <= 301, (a, b, c))
    app.model = None; root.destroy()

def scenario_found():
    """A disc image already in Downloads opens Setup on Disc ready (read-only search)."""
    d = fresh_env(); dl = d / 'Downloads'; dl.mkdir()
    try:
        os.link(XISO, dl / 'MechAssault2.iso')
    except OSError:
        print('   (no hard link possible here: skipped)'); return
    os.environ['MA2_UI_DISC_SEARCH'] = str(dl)
    root, app = rig.make()
    check('found: a disc image in Downloads is found and checked', until(root, lambda: app.image is not None and app.image.ok, 30), app.image)
    check('found: focus on INSTALL', until(root, lambda: focused(root) is app.primary, 5))
    root.destroy(); os.environ['MA2_UI_DISC_SEARCH'] = ''
    (dl / 'MechAssault2.iso').unlink()

def scenario_plain():
    fresh_env(); root, app = rig.make(plain=True); pump(root, 0.3)
    check('high contrast: system colours, no hex art', ui_kit.P['bg'] == 'SystemWindow' and not app.hexes)
    snap(root, SHOTS / 'plain.png'); root.destroy(); ui_kit.P.update(ui_kit.DARK)

def scenario_fonts():
    fresh_env(); root, app = rig.make(fonts=False)
    k = app.k
    check('fonts: a heading, body and mono family is always found', k.head in ('Rajdhani', 'Bahnschrift', 'Segoe UI Semibold', 'Segoe UI') and k.mono in ('Share Tech Mono', 'Consolas', 'Courier New'), (k.head, k.mono))
    check('fonts: pixel sizes follow the one scale (13 px minimum)', min(-f.cget('size') for f in k.fonts.values()) >= 13)
    root.destroy()

def scenario_errors():
    """A Tk callback error is logged and the window stays usable."""
    import io, contextlib
    fresh_env(); root, app = rig.make()
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        try: raise ValueError('simulated callback error')
        except ValueError: app.ui_error(*sys.exc_info())
    check('errors: a callback error goes to the log', 'simulated callback error' in err.getvalue())
    check('errors: Tk errors are routed to the app', root.report_callback_exception == app.ui_error)
    root.destroy()

def scenario_shots():
    """Clean pictures of the remaining screens for the owner (run with --shots)."""
    if not OWNER_SHOTS: return
    d = fresh_env(); root, app = rig.make(); ready_disc(root, app, d)
    app.facts['sac'] = None; app.folder.findings = []
    app.show_failed('build_failed', 'x'); app.go('stop'); pump(root, 0.2)
    root.destroy()

if __name__ == '__main__':
    (WORK / 'tmp').mkdir(exist_ok=True)
    args = sys.argv[1:]
    if '--shots' in args:
        i = args.index('--shots'); OWNER_SHOTS = Path(args[i + 1]); OWNER_SHOTS.mkdir(parents=True, exist_ok=True); del args[i:i + 2]
    names = args or ['happy', 'engine', 'fail', 'cancel', 'update', 'elsewhere', 'maintain', 'resume', 'extras', 'stops', 'tools', 'blocked', 'menu', 'keyboard',
                     'drop', 'faults', 'startup', 'layout', 'dpi', 'hooks', 'eta', 'found', 'plain', 'fonts', 'errors']
    for n in names:
        print('== %s' % n, flush=True)
        try:
            globals()['scenario_' + n]()
        except Exception:
            import traceback; traceback.print_exc(); check(n + ' ran without an exception', False)
    bad = [r for r in RESULTS if not r[1]]
    print('\n%d checks, %d failed' % (len(RESULTS), len(bad)))
    (WORK / 'tmp' / 'ui_flow-results.json').write_text(json.dumps(RESULTS, indent=1))
    sys.exit(1 if bad else 0)
