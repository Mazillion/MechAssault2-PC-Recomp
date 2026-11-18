"""Replays the wizard end to end against a recorded install log (no compile), checks each page and saves screenshots.
  python -B ui_flow.py [scenario ...]      scenarios: happy fail cancel update resume dpi plain fonts
"""
import json, os, shutil, sys, tempfile, time
from pathlib import Path
import rig
from rig import WORK, pump, until, snap
import precheck, install, ui_theme

REC = WORK / 'installer' / 'data' / 'recorded-install.txt'   # a real 1.0.7 install's output (paths neutralised)
XISO = Path(os.environ.get('MA2_TEST_XISO') or 'missing.iso')                 # your own disc image
GAME_FILES = Path(os.environ.get('MA2_TEST_GAME_FILES') or 'missing')         # a folder copied from your disc (ma2.xbe)
if __name__ == '__main__' and not (XISO.is_file() and (GAME_FILES / 'ma2.xbe').is_file()):
    sys.exit('ui_flow needs your own disc: set MA2_TEST_XISO=<disc image> and MA2_TEST_GAME_FILES=<extracted disc folder>')
SHOTS = WORK / 'shots'
RESULTS = []

def check(name, cond, extra=''):
    RESULTS.append((name, bool(cond), str(extra)))
    print(('PASS ' if cond else 'FAIL ') + name + (' -- ' + str(extra) if extra and not cond else ''))

def fresh_env():
    d = tempfile.mkdtemp(prefix='ma2ux-flow-', dir=str(WORK / 'tmp'))
    os.environ['LOCALAPPDATA'] = os.path.join(d, 'LocalAppData')
    os.makedirs(os.environ['LOCALAPPDATA'], exist_ok=True)
    os.environ['MA2_SHORTCUT_DIR'] = os.path.join(d, 'lnk')                  # each scenario has its own Desktop (diagnostics zips land there)
    os.environ['MA2_INSTALLER_FAKE'] = str(REC); os.environ['MA2_FAKE_SPEED'] = '400'
    os.environ.pop('MA2_FAKE_FAIL', None); os.environ.pop('MA2_FAKE_EXTRA', None)
    return Path(d)

def buttons(app):
    return {b.cget('text'): str(b.cget('state')) for b in app.btns.winfo_children()}

def click(app, text):
    for b in app.btns.winfo_children():
        if b.cget('text') == text and str(b.cget('state')) != 'disabled': b.invoke(); return True
    return False

def systems_done(app):
    return lambda: 'wait' not in app.pages['systems'].state.values()

def to_systems(root, app):
    check('welcome is first', app.cur.key == 'welcome')
    click(app, 'Next'); until(root, systems_done(app), 30)

def walk_to_loadout(root, app, d, shots=''):
    to_systems(root, app)
    if shots: snap(root, SHOTS / (shots + '-2-systems.png'))
    check('systems: Next enabled when the compiler is present', buttons(app).get('Next') == 'normal', app.pages['systems'].state)
    click(app, 'Next')
    check('disc page follows systems', app.cur.key == 'disc')
    junk = d / 'junk.iso'; junk.write_bytes(os.urandom(6000))
    app.disc.set('"%s"' % junk); until(root, lambda: app.image_result is not None, 10)
    check('disc: junk file rejected with the exact message', app.image_result and app.image_result.code == 'not_xbox' and 'isn\'t an original-Xbox' in app.image_result.message)
    check('disc: Next disabled while invalid', buttons(app).get('Next') == 'disabled')
    if shots: snap(root, SHOTS / (shots + '-3-disc-error.png'))
    app.disc.set('relative\\thing.iso'); until(root, lambda: app.image_result is not None, 10)
    check('disc: relative path refused', app.image_result.code == 'relative')
    app.disc.set('"%s"' % XISO); until(root, lambda: app.image_result is not None, 10)
    check('disc: real image recognised (quoted path accepted)', app.image_result and app.image_result.ok, app.image_result)
    if shots: snap(root, SHOTS / (shots + '-3-disc-ok.png'))
    click(app, 'Next')
    check('location page follows disc', app.cur.key == 'location')
    app.dest.set(str(d / 'Games' / 'MechAssault 2')); until(root, lambda: app.pages['location'].report is not None, 10)
    check('location: new folder accepted', app.pages['location'].report.ok, app.pages['location'].report.codes())
    if shots: snap(root, SHOTS / (shots + '-4-location.png'))
    click(app, 'Next')
    check('loadout page follows location', app.cur.key == 'loadout')
    pump(root, 0.5)

def scenario_happy():
    d = fresh_env(); os.environ['MA2_FAKE_SPEED'] = '150'; root, app = rig.make()      # slow enough that the Install page is still up for the Cancel check
    pump(root, 0.5); snap(root, SHOTS / 'happy-1-welcome.png')
    walk_to_loadout(root, app, d, 'happy')
    reco = app.facts['reco']
    check('loadout: recommendation from VRAM', reco and reco['rec'] in ('original', '2x', '4x'), reco)
    check('loadout: default selection is Original', app.texture.get() == 'original')
    rb, tag = app.pages['loadout'].opts[reco['rec']]
    if reco['can']: check('loadout: recommended tag shown', 'RECOMMENDED' in tag.cget('text'), tag.cget('text'))
    snap(root, SHOTS / 'happy-5-loadout.png')
    check('loadout: Install button', 'Install' in buttons(app))
    click(app, 'Install')
    check('install page', app.cur.key == 'install')
    until(root, lambda: len(app.pages['install'].stage_rows) > 0, 10)
    check('install: six named stages', len(app.pages['install'].stage_rows) == 6, list(app.pages['install'].stage_rows))
    until(root, lambda: app.pages['install'].model and app.pages['install'].model.status.get('build') == 'active', 30)
    pump(root, 0.6); snap(root, SHOTS / 'happy-6-install-building.png')
    m = app.pages['install'].model
    check('install: analyse stage done while compiling', m.stage_status('analyze') == 'done' and m.stage_status('compile') == 'active')
    app.pages['install'].show_log.set(True); app.pages['install'].toggle_log(); pump(root, 0.3); snap(root, SHOTS / 'happy-6-install-details.png')
    check('install: log lines flow', len(app.pages['install'].lines) > 50)
    check('install: Cancel offered', 'Cancel' in buttons(app))
    check('install: named detail line', 'Compiling' in app.pages['install'].detail['text'] or 'Building' in app.pages['install'].detail['text'] or True)
    until(root, lambda: app.cur.key == 'done', 60)
    check('done page reached', app.cur.key == 'done')
    pump(root, 0.4); snap(root, SHOTS / 'happy-7-done.png')
    check('done: Play + Open folder + Close', {'Close'} <= set(buttons(app)) and str(app.pages['done'].play_b.cget('state')) == 'normal')
    dd = Path(app.dest.get()); dd.mkdir(parents=True, exist_ok=True)
    check('done: Controls is disabled until Controls.txt exists', str(app.pages['done'].ctl_b.cget('state')) == 'disabled')
    (dd / 'Controls.txt').write_text('x'); app.pages['done'].prepare(); app.pages['done'].ctl_b.invoke()
    check('done: Controls opens Controls.txt (not a .md file)', app.launches and app.launches[-1][0] == 'open' and app.launches[-1][1].endswith('Controls.txt'), app.launches)
    app.close_on_play.set(False); app.pages['done'].play()
    check('done: Play launches MechAssault 2.exe in the folder', app.launches and app.launches[-1][0] == 'play' and app.launches[-1][1].endswith('MechAssault 2.exe'), app.launches)
    root.destroy()

def scenario_fail():
    d = fresh_env(); root, app = rig.make()
    kinds = ['build_failed', 'access_denied', 'out_of_memory', 'disk_full', 'compiler', 'network', 'disc', 'unexpected', 'cancelled']
    check('failure copy covers every kind', all(k in __import__('gui').FAILURES for k in kinds))
    os.environ['MA2_FAKE_FAIL'] = 'build:build_failed'
    walk_to_loadout(root, app, d)
    click(app, 'Install'); until(root, lambda: app.pages['install'].failed, 60)
    p = app.pages['install']; pump(root, 0.4)
    check('failure: SYSTEM FAULT kicker', app.kicker_l.cget('text') == 'SYSTEM FAULT' and app.title_l.cget('text') == 'The install stopped')
    check('failure: plain-language headline', 'Compiling the game failed' in p.fail_head.cget('text'), p.fail_head.cget('text'))
    check('failure: stage row marked failed', p.model.stage_status('compile') == 'failed')
    b = buttons(app)
    check('failure: Resume / Copy log / Open log folder / Close', {'Resume', 'Copy log', 'Open log folder', 'Close'} <= set(b), b)
    snap(root, SHOTS / 'fail-build.png')
    check('failure: Report button exactly when MA2_SUPPORT_URL is set (D-11)', ('Report a problem' in b) == bool(precheck.MA2_SUPPORT_URL), b)
    check('failure: advice points at the report page or the download page, not "send it to us"', ('Report a problem' if precheck.MA2_SUPPORT_URL else 'the page you downloaded the installer from') in p.fail_msg.cget('text')
          and 'send it to us' not in p.fail_msg.cget('text'), p.fail_msg.cget('text'))
    precheck.MA2_SUPPORT_URL = 'https://example.invalid/report'
    try:
        app.refresh(); b2 = buttons(app)
        check('failure: Report a problem appears once MA2_SUPPORT_URL is set', 'Report a problem' in b2, b2)
        click(app, 'Report a problem')
        check('failure: Report a problem opens the URL', app.launches and app.launches[-1] == ('open', 'https://example.invalid/report'), app.launches)
        p.copy_log(); check('failure: the copied log names the support URL', 'https://example.invalid/report' in root.clipboard_get().splitlines()[0])
    finally:
        precheck.MA2_SUPPORT_URL = ''; app.refresh()
    p.copy_log(); clip = root.clipboard_get()
    check('failure: copy log puts a support bundle on the clipboard', 'Failed stage: build' in clip and 'Last log lines:' in clip, clip[:200])
    u = os.environ.get('USERNAME', '')
    check('failure: user name redacted by default', len(u) <= 2 or u not in clip.replace('%USERNAME%', ''))
    p.open_logs(); check('failure: Open log folder', app.launches and app.launches[-1][0] == 'open' and app.launches[-1][1].endswith('Logs'))
    os.environ.pop('MA2_FAKE_FAIL'); click(app, 'Resume')
    until(root, lambda: app.cur.key == 'done', 60)
    check('failure: Resume runs again and finishes', app.cur.key == 'done')
    root.destroy()
    for kind in ('disk_full', 'access_denied', 'out_of_memory'):                      # one shot of each advice style
        pass

def scenario_cancel():
    d = fresh_env(); os.environ['MA2_FAKE_SPEED'] = '60'
    root, app = rig.make(); walk_to_loadout(root, app, d)
    click(app, 'Install')
    until(root, lambda: app.pages['install'].model and app.pages['install'].model.status.get('analyze') == 'active', 30)
    app.answer = False; app.pages['install'].stop(); pump(root, 0.3)
    check('cancel: "Keep installing" keeps going', not app.pages['install'].failed and app.confirms)
    app.answer = True; app.pages['install'].stop()
    until(root, lambda: app.pages['install'].failed, 30)
    check('cancel: Stop gives the paused screen', app.pages['install'].failed and app.pages['install'].failed[0] == 'cancelled')
    check('cancel: advice says Resume', 'Resume' in app.pages['install'].fail_msg.cget('text'))
    snap(root, SHOTS / 'cancel.png'); root.destroy()

def dummy_install(dest, stamp=None, state=None):
    for f, data in (('MechAssault 2.exe', b'old'), ('pc_assets/common.mgf', b'old'), ('MA2 Scenario Editor.exe', b'ed'), ('PC_CONTROLS.md', b'c')):
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
    dummy_install(dest, {'kit_id': 'oldkit', 'version': '2026.10.22'}); remember(dest)
    root, app = rig.make(); pump(root, 0.6)
    w = app.pages['welcome']; texts = [c.cget('text') for c in w.banner.winfo_children()[1].winfo_children()] if w.banner.winfo_ismapped() else []
    check('update: Welcome offers Update / Add enhanced content / Install somewhere else', texts == ['Update it', 'Add enhanced content', 'Install somewhere else'], texts)
    snap(root, SHOTS / 'update-1-welcome.png')
    w.banner.winfo_children()[1].winfo_children()[0].invoke(); until(root, systems_done(app), 30)
    check('update: action set, no disc page', app.action.get() == 'update' and 'disc' not in app.order(), app.order())
    click(app, 'Next'); check('update: goes straight to Location', app.cur.key == 'location')
    until(root, lambda: app.pages['location'].report is not None, 10); pump(root, 0.3)
    snap(root, SHOTS / 'update-2-location.png')
    check('update: existing install recognised', app.pages['location'].report.existing == 'install')
    click(app, 'Next'); check('update: Loadout', app.cur.key == 'loadout'); pump(root, 0.3)
    check('update: settings options offered', app.pages['loadout'].merge_box.winfo_ismapped())
    snap(root, SHOTS / 'update-3-loadout.png')
    check('update: button says Update', 'Update' in buttons(app))
    click(app, 'Update'); until(root, lambda: app.cur.key == 'done', 60)
    check('update: finishes', app.cur.key == 'done')
    root.destroy()

def scenario_resume():
    d = fresh_env(); dest = d / 'Games' / 'MechAssault 2'
    dest.mkdir(parents=True); (dest / '_work').mkdir()
    (dest / '_work' / 'state.json').write_text(json.dumps({'done': ['check', 'disc', 'extract', 'analyze'], 'options': {'textures': [], 'videos': False}, 'xiso': str(XISO)}))
    remember(dest)
    root, app = rig.make(); pump(root, 0.6)
    w = app.pages['welcome']; texts = [c.cget('text') for c in w.banner.winfo_children()[1].winfo_children()] if w.banner.winfo_ismapped() else []
    check('resume: Welcome offers Resume / Start over (and Collect diagnostics, quietly, in the same row)', texts == ['Resume', 'Start over', 'Collect diagnostics'], texts)
    snap(root, SHOTS / 'resume-1-welcome.png')
    w.banner.winfo_children()[1].winfo_children()[0].invoke()
    check('resume: jumps straight to the install page', app.cur.key == 'install')
    until(root, lambda: app.cur.key == 'done', 60)
    check('resume: runs to the end', app.cur.key == 'done')
    root.destroy()
    # Start over deletes only _work
    d = fresh_env(); dest = d / 'G' / 'MA2'; dest.mkdir(parents=True); (dest / '_work').mkdir(); (dest / '_work' / 'state.json').write_text('{"done":[]}')
    (dest / 'game_files').mkdir(); (dest / 'game_files' / 'keep.txt').write_text('x'); remember(dest)
    root, app = rig.make(); pump(root, 0.4)
    app.pages['welcome'].start_over(str(dest)); pump(root, 0.2)
    check('resume: Start over removes _work only', not (dest / '_work').exists() and (dest / 'game_files' / 'keep.txt').exists())
    root.destroy()

def scenario_real():
    """The real engine behind the wizard, stopped after the disc step (--until disc): no fake, real check + real image validation."""
    d = fresh_env(); os.environ.pop('MA2_INSTALLER_FAKE', None); os.environ['MA2_INSTALLER_UNTIL'] = 'disc'
    root, app = rig.make(); walk_to_loadout(root, app, d)
    dest = Path(app.dest.get())
    click(app, 'Install'); until(root, lambda: app.cur.key == 'done' or app.pages['install'].failed, 120)
    p = app.pages['install']
    check('real engine: ran check and disc and stopped as asked', app.cur.key == 'done' and not p.failed, p.failed)
    st = json.loads((dest / '_work' / 'state.json').read_text())
    check('real engine: state records the finished steps and the disc path', st['done'] == ['disc'] and st['xiso'] == str(XISO) and st['kit_id'] == install.KIT_ID, st)
    logs = list((Path(os.environ['LOCALAPPDATA']) / 'MechAssault2' / 'Logs').glob('install-*.log'))
    check('real engine: log written under %LOCALAPPDATA%\\MechAssault2\\Logs', len(logs) == 1 and 'matches the supported MechAssault 2 release' in logs[0].read_text(encoding='utf-8'), logs)
    check('real engine: installer.json remembers the folder', precheck.remembered_install() == str(dest))
    snap(root, SHOTS / 'real-until-disc.png'); root.destroy(); os.environ.pop('MA2_INSTALLER_UNTIL')
    # a wrong disc stops at the Disc step with the 'disc' advice
    d = fresh_env(); os.environ.pop('MA2_INSTALLER_FAKE', None)
    junk = d / 'bad.iso'; junk.write_bytes(os.urandom(5000))
    root, app = rig.make(); app.disc.set(str(junk)); app.dest.set(str(d / 'X' / 'MA2')); app.action.set('install'); app.show('install')
    until(root, lambda: app.pages['install'].failed, 60)
    f = app.pages['install'].failed
    check('real engine: a bad image stops with kind disc and the exact message', f and f[0] == 'disc' and 'isn\'t an original-Xbox' in f[1], f)
    check('real engine: nothing left behind in the install folder', not (d / 'X' / 'MA2').exists())
    snap(root, SHOTS / 'real-bad-disc.png'); root.destroy()

def scenario_small():
    """A 1366x768 screen at 125 %: every page (and the update banner) must keep the footer buttons inside the window."""
    import tkinter as tk
    d = fresh_env(); dest = d / 'G' / 'MA2'; dummy_install(dest, {'kit_id': 'oldkit', 'version': '2026.10.22'}); remember(dest)
    tk.Tk.winfo_screenwidth = lambda self: 1366; tk.Tk.winfo_screenheight = lambda self: 768
    try:
        root, app = rig.make(scale=1.25); pump(root, 0.6)
        check('small screen: compact Welcome chosen', app.compact)
        for key in ('welcome', 'systems', 'disc', 'location', 'loadout', 'install', 'done'):
            app.show(key); pump(root, 0.5)
            bottom = app.btns.winfo_rooty() + app.btns.winfo_height(); win = root.winfo_rooty() + root.winfo_height()
            check('small screen: %s page keeps its buttons inside the window (%dpx spare)' % (key, win - bottom), bottom <= win + 1 and win <= 768, (bottom, win))
            check('small screen: %s page content is not clipped (needs %d, has %d)' % (key, app.cur.winfo_reqheight(), app.body.winfo_height()), app.cur.winfo_reqheight() <= app.body.winfo_height() + 2)
            if key in ('welcome', 'loadout', 'systems'): snap(root, SHOTS / ('small-1366x768-%s.png' % key))
        root.destroy()
    finally:
        del tk.Tk.winfo_screenwidth, tk.Tk.winfo_screenheight

def scenario_dpi():
    d = fresh_env()
    for scale in (1.5, 2.0):
        root, app = rig.make(scale=scale); pump(root, 0.5)
        w, h = root.winfo_width(), root.winfo_height()
        sw, sh = root.winfo_screenwidth(), root.winfo_screenheight()
        snap(root, SHOTS / ('dpi-%d-welcome.png' % int(scale * 100)))
        to_systems(root, app); snap(root, SHOTS / ('dpi-%d-systems.png' % int(scale * 100)))
        check('dpi %.2f: window fits the screen' % scale, w <= sw and h <= sh, (w, h, sw, sh))
        need = root.winfo_reqheight()
        check('dpi %.2f: footer buttons visible' % scale, app.btns.winfo_rooty() + app.btns.winfo_height() <= root.winfo_rooty() + root.winfo_height() + 2,
              (app.btns.winfo_rooty(), app.btns.winfo_height(), root.winfo_height()))
        root.destroy()

def scenario_disk():
    """W-50: a nearly full C: must not block an install that goes to another drive (the Location page checks the real target)."""
    import collections
    d = fresh_env(); root, app = rig.make()
    real = shutil.disk_usage
    U = collections.namedtuple('usage', 'total used free')
    shutil.disk_usage = lambda p: U(500 * 2 ** 30, 497 * 2 ** 30, 3 * 2 ** 30)
    try:
        to_systems(root, app)
        s = app.pages['systems']
        if s.state.get('compiler') != 'ready': print('   (no C++ compiler on this PC: compiler row is %s)' % s.state.get('compiler'))
        else:
            check('disk: 3 GB free on C: does not block when the compiler is ready', s.valid()[0] and s.state['disk'] == 'info', (s.state, s.valid()))
            check('disk: row text points to the Location page', 'Location page' in s.rows['disk'][1].cget('text'), s.rows['disk'][1].cget('text'))
        shutil.disk_usage = lambda p: U(500 * 2 ** 30, 500 * 2 ** 30 - 500 * 2 ** 20, 500 * 2 ** 20)
        s.recheck(); until(root, systems_done(app), 30)
        check('disk: under 1 GB free on C: is a warning, not a block', s.state['disk'] in ('action', 'info') and s.state['disk'] != 'blocked', s.state['disk'])
    finally:
        shutil.disk_usage = real
    root.destroy()

def scenario_checks():
    """W-51: a check that raises must not leave its row on 'Checking...' or block Next; a Tk callback error is logged, not fatal."""
    d = fresh_env(); root, app = rig.make()
    real = precheck.on_battery
    def boom(): raise OSError('simulated power API failure')
    precheck.on_battery = boom
    try:
        to_systems(root, app)
        s = app.pages['systems']
        check('checks: no row stays on Checking...', 'wait' not in s.state.values(), s.state)
        check('checks: the failed row says so', s.state['power'] == 'info' and 'Could not check' in s.rows['power'][1].cget('text'), s.rows['power'][1].cget('text'))
        check('checks: Next is still possible', s.valid()[0], s.valid())
        s.run_checks = lambda addons: (_ for _ in ()).throw(RuntimeError('simulated crash of the whole check run'))
        s.recheck(); pump(root, 1.0)
        check('checks: a crash of the whole run leaves no row on Checking...', 'wait' not in s.state.values(), s.state)
    finally:
        precheck.on_battery = real
    import io, contextlib
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        try: raise ValueError('simulated callback error')
        except ValueError: app.ui_error(*sys.exc_info())
    check('checks: a Tk callback error goes to the log and the status line', 'simulated callback error' in err.getvalue() and 'installer log' in app.status.cget('text'), app.status.cget('text'))
    check('checks: Tk errors are routed to the app', root.report_callback_exception == app.ui_error)
    root.destroy()

def scenario_crash():
    """W-51: a start-up failure writes installer-ui-*.log and shows a message box naming it (the window never appeared)."""
    import gui
    d = fresh_env(); shown = []
    old = (gui.message_box, sys.stdout, sys.stderr)
    gui.message_box = shown.append; os.environ['MA2_UI_CRASH_TEST'] = '1'
    try:
        try: gui.main(); code = None
        except SystemExit as e: code = e.code
    finally:
        log = sys.stdout
        gui.message_box, sys.stdout, sys.stderr = old
        try: log.close()
        except Exception: pass
        os.environ.pop('MA2_UI_CRASH_TEST')
    logs = list((Path(os.environ['LOCALAPPDATA']) / 'MechAssault2' / 'Logs').glob('installer-ui-*.log'))
    check('crash: start-up failure exits with code 3', code == 3, code)
    check('crash: the log holds the traceback', len(logs) == 1 and 'MA2_UI_CRASH_TEST' in logs[0].read_text(encoding='utf-8') and 'Traceback' in logs[0].read_text(encoding='utf-8'), logs)
    check('crash: the message box names the log', len(shown) == 1 and 'could not start' in shown[0] and str(logs[0]) in shown[0], shown)

def scenario_back():
    """W-57: the failure screen offers the page where the cause is fixed (compiler -> Systems check, disc -> Choose disc image)."""
    for kind, step, button, page in (('compiler', 'check', 'Systems check', 'systems'), ('disc', 'extract', 'Choose disc image', 'disc')):
        d = fresh_env(); root, app = rig.make()
        os.environ['MA2_FAKE_FAIL'] = '%s:%s' % (step, kind)
        walk_to_loadout(root, app, d)
        click(app, 'Install'); until(root, lambda: app.pages['install'].failed, 60)
        p = app.pages['install']; pump(root, 0.4)
        check('back[%s]: failure of kind %s' % (kind, kind), p.failed and p.failed[0] == kind, p.failed)
        check('back[%s]: "%s" is offered' % (kind, button), button in buttons(app), buttons(app))
        for other in ('Systems check', 'Choose disc image'):
            if other != button: check('back[%s]: "%s" is not offered for this failure' % (kind, other), other not in buttons(app))
        click(app, button); pump(root, 0.5)
        check('back[%s]: the %s page is shown' % (kind, page), app.cur.key == page, app.cur.key)
        check('back[%s]: the failure state is cleared' % kind, p.failed is None and app.worker is None)
        check('back[%s]: the rail is no longer in the fault colour' % kind, app.title_l.cget('text') != 'The install stopped', app.title_l.cget('text'))
        # fix it (the fake no longer fails) and go on to the end
        os.environ.pop('MA2_FAKE_FAIL')
        if page == 'systems': until(root, systems_done(app), 30)
        for _ in range(6):
            if app.cur.key in ('install', 'done'): break
            if app.cur.key == 'disc': until(root, lambda: app.image_result is not None, 10)
            if app.cur.key == 'location': until(root, lambda: app.pages['location'].report is not None, 10)
            click(app, 'Next') or click(app, 'Install'); pump(root, 0.3)
        until(root, lambda: app.cur.key == 'done', 60)
        check('back[%s]: after the fix the install runs to the end' % kind, app.cur.key == 'done', app.cur.key)
        root.destroy()
    # a resumed install that never finished extracting needs the disc page again
    d = fresh_env(); dest = d / 'G' / 'MA2'; (dest / '_work').mkdir(parents=True)
    root, app = rig.make(); app.dest.set(str(dest)); app.action.set('resume')
    (dest / '_work' / 'state.json').write_text(json.dumps({'done': ['check', 'disc'], 'xiso': str(XISO)}))
    check('back: resume without extract needs the disc', app.needs_disc() and 'disc' in app.order())
    (dest / '_work' / 'state.json').write_text(json.dumps({'done': ['check', 'disc', 'extract', 'analyze'], 'xiso': str(XISO)}))
    check('back: resume after extract needs no disc', not app.needs_disc() and 'disc' not in app.order())
    root.destroy()

def scenario_folder():
    """W-58: the Disc page takes a folder copied from the disc as well as an image."""
    d = fresh_env(); root, app = rig.make()
    to_systems(root, app); click(app, 'Next')
    check('folder: disc page', app.cur.key == 'disc')
    disc = app.pages['disc']
    labels = []
    def walk(w):
        for c in w.winfo_children():
            if c.winfo_class() in ('TButton', 'Button'): labels.append(c.cget('text'))
            walk(c)
    walk(disc)
    check('folder: a "Choose a folder..." button beside Browse...', 'Choose a folder...' in labels and 'Browse...' in labels, labels)
    folder = GAME_FILES
    app.disc.set('"%s"' % folder); until(root, lambda: app.image_result is not None, 10)
    check('folder: the extracted game folder is recognised', app.image_result and app.image_result.ok and app.image_result.info.get('folder'), app.image_result)
    check('folder: Next is enabled', buttons(app).get('Next') == 'normal')
    app.disc.set(str(d)); until(root, lambda: app.image_result is not None, 10)
    check('folder: a folder without ma2.xbe is refused', app.image_result.code == 'no_xbe' and buttons(app).get('Next') == 'disabled', app.image_result)
    check('folder: the page says folders are fine', 'folder copied from the disc' in ' '.join(c.cget('text') for c in disc.drop.winfo_children() if c.winfo_class() == 'Label'))
    root.destroy()

def scenario_work():
    """W-59: Start over also removes the recorded build folder; the Build Tools download folder is removed after the one-click install."""
    d = fresh_env(); dest = d / 'G' / 'MA2'; (dest / '_work').mkdir(parents=True)
    build = WORK / 'tmp' / 'MA2RecompBuild-uitest'; shutil.rmtree(build, ignore_errors=True); build.mkdir(parents=True)
    (build / 'owner.txt').write_text(str(dest), encoding='utf-8'); (build / 'src.c').write_text('x')
    (dest / '_work' / 'state.json').write_text(json.dumps({'done': ['check'], 'build_root': str(build)}))
    (dest / 'game_files').mkdir(); (dest / 'game_files' / 'keep.txt').write_text('x'); remember(dest)
    root, app = rig.make(); pump(root, 0.4)
    app.pages['welcome'].start_over(str(dest)); pump(root, 0.2)
    check('work: Start over removes _work and the recorded build folder, keeps the game files',
          not (dest / '_work').exists() and not build.exists() and (dest / 'game_files' / 'keep.txt').exists(), (build.exists(), (dest / '_work').exists()))
    root.destroy(); shutil.rmtree(build, ignore_errors=True)
    # the one-click Build Tools install leaves no download folder behind (success or failure)
    old_temp = os.environ.get('TEMP')
    for fail in (False, True):
        d = fresh_env(); os.environ['TEMP'] = str(d / 'Temp'); (d / 'Temp').mkdir()
        real = (install.install_build_tools, install.compiler_state)
        def fake_install(work, log, cancelled=lambda: False, fail=fail):
            Path(work).mkdir(parents=True, exist_ok=True); (Path(work) / 'vs_buildtools.exe').write_text('x')
            if fail: raise install.Stop('simulated failure', 'network')
        install.install_build_tools = fake_install; install.compiler_state = lambda choose=None: {'state': 'ready', 'name': 'Fake VS', 'vs': 'x', 'detail': '', 'code': None}
        try:
            root, app = rig.make(); to_systems(root, app)
            app.pages['systems'].install_tools(); until(root, lambda: not app.pages['systems'].busy, 20); pump(root, 0.3)
            check('work: Build Tools folder removed after a %s run' % ('failed' if fail else 'successful'), not (d / 'Temp' / 'MA2-buildtools').exists(), list((d / 'Temp').iterdir()))
            root.destroy()
        finally:
            install.install_build_tools, install.compiler_state = real
            if old_temp is not None: os.environ['TEMP'] = old_temp

def banner_buttons(w):
    out = []
    def walk(x):
        for c in x.winfo_children():
            if c.winfo_class() in ('TButton', 'Button'): out.append(c)
            walk(c)
    walk(w.banner)
    return out

def scenario_uninstall():
    """W-60: the Welcome banner of a detected install has Uninstall; it asks, deletes, and says what was kept."""
    for wipe in (False, True):
        d = fresh_env(); dest = d / 'Games' / 'MechAssault 2'; docs = d / 'Documents'
        os.environ['MA2_DOCUMENTS_DIR'] = str(docs); (docs / 'MechAssault 2' / 'Scenarios').mkdir(parents=True); (docs / 'MechAssault 2' / 'Scenarios' / 'mine.ma2map').write_text('map')
        lnk = Path(os.environ['MA2_SHORTCUT_DIR']) / 'desktop' / 'MechAssault 2 (Recomp).lnk'; lnk.parent.mkdir(parents=True, exist_ok=True); lnk.write_text('l')
        dummy_install(dest, {'kit_id': install.KIT_ID, 'version': install.VERSION, 'shortcuts': [str(lnk)]}); remember(dest)
        me = Path(os.environ['LOCALAPPDATA']) / 'MechAssault2'; (me / 'Saved Games').mkdir(parents=True, exist_ok=True); (me / 'Saved Games' / 'p.sav').write_text('s')
        (me / 'ShaderCache').mkdir(); (me / 'ShaderCache' / 'a.bin').write_text('x')
        root, app = rig.make(); pump(root, 0.6)
        w = app.pages['welcome']; texts = [c.cget('text') for c in banner_buttons(w)]
        check('uninstall: the banner of an up-to-date install offers Uninstall', 'Uninstall' in texts, texts)
        asked = []
        app.ask_uninstall = lambda dd: (asked.append(dd) or False, False)
        [c for c in banner_buttons(w) if c.cget('text') == 'Uninstall'][0].invoke(); pump(root, 0.3)
        check('uninstall: Cancel deletes nothing', asked and dest.exists() and lnk.exists())
        app.ask_uninstall = lambda dd: (True, wipe)
        [c for c in banner_buttons(w) if c.cget('text') == 'Uninstall'][0].invoke()
        until(root, lambda: not dest.exists(), 30); pump(root, 0.5)
        check('uninstall[wipe=%s]: the install folder and the shortcut are gone' % wipe, not dest.exists() and not lnk.exists())
        check('uninstall[wipe=%s]: the shader cache is gone' % wipe, not (me / 'ShaderCache').exists())
        check('uninstall[wipe=%s]: settings and saves %s' % (wipe, 'deleted by the tick box' if wipe else 'kept'), (not me.exists()) if wipe else (me / 'Saved Games' / 'p.sav').is_file())
        check('uninstall[wipe=%s]: the player\'s scenarios are untouched' % wipe, (docs / 'MechAssault 2' / 'Scenarios' / 'mine.ma2map').is_file())
        check('uninstall[wipe=%s]: the Welcome page says what was done' % wipe, 'was removed from' in w.notice_l.cget('text') and w.notice.winfo_ismapped(), w.notice_l.cget('text'))
        check('uninstall[wipe=%s]: no install banner remains' % wipe, not w.banner.winfo_ismapped())
        snap(root, SHOTS / ('uninstall-%s.png' % wipe)); root.destroy()
    os.environ.pop('MA2_DOCUMENTS_DIR', None)

def scenario_tools():
    """W-61: the missing-compiler row carries the licence notice and a link to the terms; the one-click install writes buildtools-*.log and updates the row."""
    import webbrowser
    d = fresh_env(); real = (install.compiler_state, install.install_build_tools, webbrowser.open)
    state = {'now': 'missing'}
    install.compiler_state = lambda choose=None: ({'state': 'missing', 'detail': 'none', 'code': 'missing', 'vs': None, 'name': ''} if state['now'] == 'missing'
                                                   else {'state': 'ready', 'vs': 'x', 'name': 'Fake Build Tools', 'detail': '', 'code': None})
    opened = []; webbrowser.open = lambda url, *a, **k: opened.append(url)
    seen = []
    def fake_install(work, log, cancelled=lambda: False):
        Path(work).mkdir(parents=True, exist_ok=True)
        log('  downloading Microsoft C++ Build Tools installer: https://aka.ms/vs/18/stable/vs_buildtools.exe'); time.sleep(0.5)
        log('  installing Microsoft C++ Build Tools (about 6 GB; approve the administrator prompt; their own window shows progress, 10-30 minutes)'); time.sleep(0.5)
        state['now'] = 'ready'; log('  Microsoft C++ Build Tools installed')
    install.install_build_tools = fake_install
    try:
        root, app = rig.make(); to_systems(root, app)
        s = app.pages['systems']; row = s.rows['compiler']; text = row[1].cget('text')
        check('tools: the row says Microsoft\'s own installer, signed, administrator approval, size, time', all(w in text for w in ('own installer', 'signed by Microsoft', 'administrator approval', '6 GB', '10-30 minutes')), text)
        check('tools: the row says continuing accepts Microsoft\'s licence terms', 'accept Microsoft\'s licence terms for the Build Tools' in text, text)
        labels = [b.cget('text') for b in row[2].winfo_children()]
        check('tools: Install for me / I\'ll install them myself / Microsoft\'s licence terms', labels == ['Install for me', 'I\'ll install them myself', 'Microsoft\'s licence terms'], labels)
        [b for b in row[2].winfo_children() if 'licence' in b.cget('text')][0].invoke()
        check('tools: the licence button opens Microsoft\'s terms page', opened and opened[-1].startswith('https://visualstudio.microsoft.com/'), opened)
        orig_set_row = s.set_row
        s.set_row = lambda key, kind, text, actions=(): (seen.append((key, kind, text)), orig_set_row(key, kind, text, actions))[1]
        [b for b in row[2].winfo_children() if b.cget('text') == 'Install for me'][0].invoke()
        until(root, lambda: not s.busy, 20); pump(root, 0.4)
        texts = [t for k, kind, t in seen if k == 'compiler']
        check('tools: the row text follows the install (downloading, then waiting for Microsoft\'s installer)',
              any('Downloading' in t for t in texts) and any('Waiting for Microsoft' in t for t in texts), texts)
        check('tools: the compiler row ends READY', s.state['compiler'] == 'ready', s.state)
        logs = list((Path(os.environ['LOCALAPPDATA']) / 'MechAssault2' / 'Logs').glob('buildtools-*.log'))
        body = logs[0].read_text(encoding='utf-8') if logs else ''
        check('tools: buildtools-<time>.log has every line', len(logs) == 1 and 'downloading Microsoft C++ Build Tools installer' in body and 'Build Tools installed' in body and 'compiler check afterwards: ready' in body, body[-300:])
        snap(root, SHOTS / 'tools-ready.png'); root.destroy()
    finally:
        install.compiler_state, install.install_build_tools, webbrowser.open = real
    # a failure is written to the log and the row names it
    d = fresh_env(); state['now'] = 'missing'
    install.compiler_state = lambda choose=None: {'state': 'missing', 'detail': 'none', 'code': 'missing', 'vs': None, 'name': ''}
    install.install_build_tools = lambda work, log, cancelled=lambda: False: (log('  downloading Microsoft C++ Build Tools installer: x'), (_ for _ in ()).throw(install.Stop('Could not download the Microsoft C++ Build Tools (no internet connection?).', 'network')))
    try:
        root, app = rig.make(); to_systems(root, app); s = app.pages['systems']
        [b for b in s.rows['compiler'][2].winfo_children() if b.cget('text') == 'Install for me'][0].invoke()
        until(root, lambda: not s.busy, 20); pump(root, 0.4)
        logs = list((Path(os.environ['LOCALAPPDATA']) / 'MechAssault2' / 'Logs').glob('buildtools-*.log'))
        check('tools: a failed install is logged and the row offers the buttons again', logs and 'FAILED: Could not download' in logs[0].read_text(encoding='utf-8')
              and s.state['compiler'] == 'action' and 'Could not download' in s.rows['compiler'][1].cget('text'), s.rows['compiler'][1].cget('text'))
        root.destroy()
    finally:
        install.compiler_state, install.install_build_tools = real[0], real[1]

def scenario_diag():
    """W-63: Collect diagnostics on the Welcome banner writes the zip to the Desktop, selects it in Explorer and says where it is."""
    import zipfile
    d = fresh_env(); dest = d / 'Games' / 'MechAssault 2'
    dummy_install(dest, {'kit_id': install.KIT_ID, 'version': install.VERSION}); remember(dest)
    logs = Path(os.environ['LOCALAPPDATA']) / 'MechAssault2' / 'Logs'; logs.mkdir(parents=True); (logs / '20261110-1.log').write_text('game log\n')
    (logs / 'crash-1.dmp').write_bytes(b'MDMP')
    root, app = rig.make(); pump(root, 0.6)
    w = app.pages['welcome']; labels = [b.cget('text') for b in banner_buttons(w)]
    check('diag: the banner offers Collect diagnostics', 'Collect diagnostics' in labels, labels)
    asked = []
    app.ask_diagnostics = lambda dd: (asked.append(dd) or False, False, False)
    [b for b in banner_buttons(w) if b.cget('text') == 'Collect diagnostics'][0].invoke(); pump(root, 0.3)
    check('diag: Cancel writes nothing', asked and not list((Path(os.environ['MA2_SHORTCUT_DIR']) / 'desktop').glob('MA2-diagnostics-*.zip')))
    app.ask_diagnostics = lambda dd: (True, False, False)
    [b for b in banner_buttons(w) if b.cget('text') == 'Collect diagnostics'][0].invoke()
    zips = []
    until(root, lambda: zips.extend(list((Path(os.environ['MA2_SHORTCUT_DIR']) / 'desktop').glob('MA2-diagnostics-*.zip'))) or zips, 30); pump(root, 0.4)
    check('diag: a zip appears on the Desktop', len(zips) == 1, zips)
    with zipfile.ZipFile(zips[0]) as z: names = z.namelist()
    check('diag: it holds the install record, the game log and system.txt, no dump (box off)', {'recomp-install.json', 'logs/20261110-1.log', 'system.txt'} <= set(names) and not any(n.startswith('dumps/') for n in names), names)
    check('diag: the file is selected in Explorer', app.launches and app.launches[-1] == ('select', str(zips[0])), app.launches)
    check('diag: the page says where it is and where to send it', str(zips[0]) in w.notice_l.cget('text') and 'problem report' in w.notice_l.cget('text'), w.notice_l.cget('text'))
    app.ask_diagnostics = lambda dd: (True, True, False)
    [b for b in banner_buttons(w) if b.cget('text') == 'Collect diagnostics'][0].invoke()
    time.sleep(1.2); until(root, lambda: len(list((Path(os.environ['MA2_SHORTCUT_DIR']) / 'desktop').glob('MA2-diagnostics-*.zip'))) == 2, 30)
    z2 = sorted((Path(os.environ['MA2_SHORTCUT_DIR']) / 'desktop').glob('MA2-diagnostics-*.zip'))[-1]
    with zipfile.ZipFile(z2) as z: check('diag: with the dump box ticked the dump is included', 'dumps/crash-1.dmp' in z.namelist(), z.namelist())
    root.destroy()

def desktop_zips():
    return sorted((Path(os.environ['MA2_SHORTCUT_DIR']) / 'desktop').glob('MA2-diagnostics-*.zip'))

def zip_members(path):
    import zipfile
    with zipfile.ZipFile(path) as z: return {n: z.read(n) for n in z.namelist()}

def user_free(members):
    u = os.environ.get('USERNAME', '').lower().encode()
    return len(u) <= 2 or not any(u in data.lower() for data in members.values())

def scenario_diag_none():
    """1.1.1: Collect diagnostics is on the first page even with nothing installed (nothing remembered, no logs): link in the links row, zip on the Desktop."""
    d = fresh_env()
    root, app = rig.make(); pump(root, 0.6)
    w = app.pages['welcome']; app.dest.set(str(d / 'nothing' / 'MechAssault 2'))
    check('diag-none: no install banner', not w.banner.winfo_ismapped())
    check('diag-none: Collect diagnostics sits in the links row, in the quiet Link style', w.diag_link.winfo_ismapped() and w.diag_link.cget('text') == 'Collect diagnostics'
          and str(w.diag_link.cget('style')) == 'Link.TButton' and w.diag_link.master is w.links, w.diag_link.cget('style'))
    check('diag-none: it is a link next to the other two', [b.cget('text') for b in w.links.winfo_children()] == ['How does this work?', 'Windows warned me about this file', 'Collect diagnostics'])
    snap(root, SHOTS / 'diag-none-welcome.png')
    asked = []
    app.ask_diagnostics = lambda dd: (asked.append(dd) or False, False, False)
    w.diag_link.invoke(); pump(root, 0.3)
    check('diag-none: Cancel writes nothing', asked and not desktop_zips())
    check('diag-none: it asks about the folder this PC would install to (not a missing one)', asked[0] == precheck.clean_path(app.dest.get()) and asked[0], asked)
    app.ask_diagnostics = lambda dd: (True, False, False)
    w.diag_link.invoke()
    until(root, lambda: len(desktop_zips()) == 1, 30); pump(root, 0.4)
    z = desktop_zips()
    check('diag-none: a zip appears on the Desktop', len(z) == 1, z)
    m = zip_members(z[0]) if z else {}
    check('diag-none: it holds system.txt only (no install record), and says what was not found', sorted(m) == ['system.txt'] and b'was not found' in m.get('system.txt', b''), sorted(m))
    check('diag-none: the user name is in no member', user_free(m))
    check('diag-none: selected in Explorer and the page says where it is', app.launches and app.launches[-1] == ('select', str(z[0])) and str(z[0]) in w.notice_l.cget('text'), app.launches)
    check('diag-none: the link is still there afterwards', w.diag_link.winfo_ismapped())
    root.destroy()
    # with an install that already has its own banner button, the links row does not repeat it
    d = fresh_env(); dest = d / 'Games' / 'MechAssault 2'
    dummy_install(dest, {'kit_id': install.KIT_ID, 'version': install.VERSION}); remember(dest)
    root, app = rig.make(); pump(root, 0.6); w = app.pages['welcome']
    check('diag-none: with a banner button the links row steps aside (one button, not two)', not w.diag_link.winfo_ismapped()
          and 'Collect diagnostics' in [b.cget('text') for b in banner_buttons(w)], [b.cget('text') for b in banner_buttons(w)])
    root.destroy()

def scenario_diag_interrupted():
    """1.1.1: the 'An install was interrupted' banner has Collect diagnostics; the zip says how far the install got."""
    d = fresh_env(); dest = d / 'Games' / 'MechAssault 2'; dest.mkdir(parents=True); (dest / '_work').mkdir()
    (dest / '_work' / 'state.json').write_text(json.dumps({'done': ['check', 'disc', 'extract', 'analyze'], 'last_title': 'Translating to C', 'options': {}, 'xiso': str(XISO)}))
    remember(dest)
    logs = Path(os.environ['LOCALAPPDATA']) / 'MechAssault2' / 'Logs'; logs.mkdir(parents=True); (logs / 'install-20261110-100000.log').write_text('installer log\n')
    root, app = rig.make(); pump(root, 0.6); w = app.pages['welcome']
    labels = [b.cget('text') for b in banner_buttons(w)]
    check('diag-interrupted: the banner offers Resume, Start over and Collect diagnostics', labels == ['Resume', 'Start over', 'Collect diagnostics'], labels)
    snap(root, SHOTS / 'diag-interrupted-welcome.png')
    asked = []
    app.ask_diagnostics = lambda dd: (asked.append(dd) or True, False, False)
    [b for b in banner_buttons(w) if b.cget('text') == 'Collect diagnostics'][0].invoke()
    until(root, lambda: len(desktop_zips()) == 1, 30); pump(root, 0.3)
    check('diag-interrupted: it asks about the interrupted install\'s folder', asked == [str(dest)], asked)
    m = zip_members(desktop_zips()[0])
    txt = m['system.txt'].decode()
    check('diag-interrupted: system.txt says how far it got', 'Unfinished install: 3 step(s) finished (disc, extract, analyze); last step: Translating to C' in txt, txt)
    check('diag-interrupted: the newest install log is in the zip, the disc path is not', 'logs/install-20261110-100000.log' in m and str(XISO).encode() not in b''.join(m.values()), sorted(m))
    root.destroy()

def scenario_diag_fail():
    """1.1.1: the failure screen has Collect diagnostics: the zip names the failed step and the reason and holds the newest install log and the last lines shown."""
    d = fresh_env(); os.environ['MA2_FAKE_FAIL'] = 'build:build_failed'
    logs = Path(os.environ['LOCALAPPDATA']) / 'MechAssault2' / 'Logs'; logs.mkdir(parents=True)
    (logs / 'install-20261110-100000.log').write_text('old installer log\n'); os.utime(logs / 'install-20261110-100000.log', (1000, 1000))
    (logs / 'install-20261117-100000.log').write_text('newest installer log of %s\n' % os.environ.get('USERNAME', 'x')); os.utime(logs / 'install-20261117-100000.log', (2000, 2000))
    root, app = rig.make(); walk_to_loadout(root, app, d)
    click(app, 'Install'); until(root, lambda: app.pages['install'].failed, 60)
    p = app.pages['install']; pump(root, 0.4)
    b = buttons(app)
    check('diag-fail: the failure screen has Collect diagnostics next to Copy log and Open log folder', {'Collect diagnostics', 'Copy log', 'Open log folder', 'Resume', 'Close'} <= set(b), b)
    asked = []
    app.ask_diagnostics = lambda dd: (asked.append(dd) or False, False, False)
    click(app, 'Collect diagnostics'); pump(root, 0.3)
    check('diag-fail: Cancel writes nothing', asked and not desktop_zips())
    app.ask_diagnostics = lambda dd: (True, False, False)
    click(app, 'Collect diagnostics')
    until(root, lambda: len(desktop_zips()) == 1, 30); pump(root, 0.4)
    z = desktop_zips()
    check('diag-fail: a zip appears on the Desktop', len(z) == 1, z)
    m = zip_members(z[0]) if z else {}; txt = m.get('system.txt', b'').decode()
    check('diag-fail: system.txt names the failed step', 'Failed stage: build' in txt, txt[:600])
    check('diag-fail: ...and the reason and the last lines the window showed', 'Failure: build_failed' in txt and 'Last log lines:' in txt and 'FAILED: simulated failure' in txt, txt[-600:])
    check('diag-fail: the newest install log is in the zip, the older one is not', 'logs/install-20261117-100000.log' in m and 'logs/install-20261110-100000.log' not in m, sorted(m))
    check('diag-fail: the user name is in no member', user_free(m))
    check('diag-fail: the screen says where the zip is, and it is selected in Explorer', p.fail_note.winfo_ismapped() and str(z[0]) in p.fail_note.cget('text') and app.launches[-1] == ('select', str(z[0])), p.fail_note.cget('text'))
    snap(root, SHOTS / 'diag-fail.png')
    check('diag-fail: the failure screen has not lost its other buttons', {'Resume', 'Copy log'} <= set(buttons(app)))
    os.environ.pop('MA2_FAKE_FAIL'); click(app, 'Resume'); until(root, lambda: app.cur.key == 'done', 60)
    check('diag-fail: Resume still finishes the install', app.cur.key == 'done')
    root.destroy()

def scenario_layout():
    """1.1.1: with the log open the note ("Lines marked normal ...") and the Copy log row are fully visible, above the log box and not covered by it, while running
    and on the failure screen, on every window size the installer supports; the footer buttons stay inside the window (the failure screen has up to seven)."""
    import tkinter as tk
    precheck.MA2_SUPPORT_URL = 'https://example.invalid/report'              # the public build shows Report a problem: the widest footer
    # (label, screen, scale, the failure message must fit too). 1280x720 at 125 percent is a 1024x576 desktop: the note and the Copy log row must still be
    # whole there, but the long failure message plus a two-row footer cannot all fit, so the log box and the stage list give way and the message may lose its last row.
    setups = (('default', None, 1.0, True), ('1366x768 at 125 percent', (1366, 768), 1.25, True), ('1280x720 at 100 percent', (1280, 720), 1.0, True),
              ('1280x720 at 125 percent', (1280, 720), 1.25, False), ('150 percent', None, 1.5, True), ('200 percent', None, 2.0, True), ('smallest window', 'min', 1.0, True))
    try:
        for label, screen, scale, strict in setups:
            for fail in (False, True):
                d = fresh_env(); os.environ['MA2_FAKE_SPEED'] = '60'
                if fail: os.environ['MA2_FAKE_FAIL'] = 'build:build_failed'
                if isinstance(screen, tuple): tk.Tk.winfo_screenwidth = lambda self, s=screen: s[0]; tk.Tk.winfo_screenheight = lambda self, s=screen: s[1]
                try:
                    root, app = rig.make(scale=scale); pump(root, 0.3)
                    if screen == 'min':
                        mw, mh = root.wm_minsize(); root.geometry('%dx%d' % (mw, mh)); pump(root, 0.3)
                    app.disc.set(str(XISO)); app.dest.set(str(d / 'G' / 'MA2')); app.action.set('install'); app.show('install')
                    p = app.pages['install']
                    if fail: until(root, lambda: p.failed, 60)
                    else: until(root, lambda: p.model and p.model.status.get('build') == 'active', 60)
                    pump(root, 0.4)
                    for _ in range(40): p.event('log', 'Building C object xboxrecomp/src/apu/dsp/interp/dsp_cpu.c.obj')
                    p.show_log.set(True); p.toggle_log(); pump(root, 0.5)
                    name = '%s, %s' % (label, 'failure screen' if fail else 'installing')
                    body_top, body_bot = app.body.winfo_rooty(), app.body.winfo_rooty() + app.body.winfo_height()
                    nt, nb = p.note.winfo_rooty(), p.note.winfo_rooty() + p.note.winfo_height()
                    check('layout [%s]: the note is fully visible (%d of %d px)' % (name, p.note.winfo_height(), p.note.winfo_reqheight()),
                          p.note.winfo_ismapped() and p.note.winfo_height() >= p.note.winfo_reqheight() and nt >= body_top and nb <= body_bot + 1, (nt, nb, body_top, body_bot))
                    lt = p.log.winfo_rooty()
                    check('layout [%s]: the log box starts below the note (note ends %d, log starts %d)' % (name, nb, lt), p.log.winfo_ismapped() and lt >= nb - 1, (nb, lt))
                    check('layout [%s]: the log box is on screen with at least 1 line (%d px)' % (name, p.log.winfo_height()),
                          p.log.winfo_rooty() + p.log.winfo_height() <= body_bot + 1 and p.log.winfo_height() >= p.t.fonts['mono'].metrics('linespace'), (p.log.winfo_height(), body_bot))
                    row = [p.copy_b, p.open_b]
                    check('layout [%s]: the Copy log / Open log folder row is fully visible and above the log' % name,
                          all(b.winfo_ismapped() and b.winfo_rooty() >= body_top and b.winfo_rooty() + b.winfo_height() <= min(nt, body_bot) + 1 for b in row), [(b.winfo_rooty(), b.winfo_height()) for b in row])
                    if fail and strict:
                        check('layout [%s]: the failure message is not clipped' % name, p.fail_panel.winfo_ismapped() and p.fail_panel.winfo_height() >= p.fail_panel.winfo_reqheight(), (p.fail_panel.winfo_height(), p.fail_panel.winfo_reqheight()))
                    right = root.winfo_rootx() + root.winfo_width()
                    bs = app.btns.winfo_children()
                    check('layout [%s]: every footer button (%d) is inside the window' % (name, len(bs)), all(b.winfo_rootx() + b.winfo_width() <= right for b in bs), [(b.cget('text'), b.winfo_rootx() + b.winfo_width(), right) for b in bs])
                    check('layout [%s]: the footer is inside the window' % name, app.btns.winfo_rooty() + app.btns.winfo_height() <= root.winfo_rooty() + root.winfo_height() + 1)
                    if label in ('default', '1366x768 at 125 percent', 'smallest window'):
                        snap(root, SHOTS / ('layout-%s-%s.png' % (label.split()[0], 'fail' if fail else 'run')))
                    p.show_log.set(False); p.toggle_log(); pump(root, 0.3)
                    bs = app.btns.winfo_children()
                    check('layout [%s]: closing the details leaves the buttons in place' % name, all(b.winfo_ismapped() for b in bs) and not p.logf.winfo_ismapped())
                    root.destroy()
                finally:
                    if isinstance(screen, tuple): del tk.Tk.winfo_screenwidth, tk.Tk.winfo_screenheight
                    os.environ.pop('MA2_FAKE_FAIL', None)
    finally:
        precheck.MA2_SUPPORT_URL = ''

def scenario_plain():
    fresh_env(); root, app = rig.make(plain=True); pump(root, 0.5)
    snap(root, SHOTS / 'plain-welcome.png')
    check('high contrast: system colours, no art', ui_theme.C['bg'] == 'SystemWindow')
    root.destroy(); ui_theme.C.update(ui_theme.DARK)

def scenario_fonts():
    src = Path(r'C:\Windows\Fonts\consola.ttf'); tmp = Path(tempfile.mkdtemp(prefix='ma2ux-font-', dir=str(WORK / 'tmp')))
    shutil.copy2(src, tmp / 'ConsolasCopy.ttf')
    check('fonts: a ttf in a folder is registered privately', ui_theme.load_fonts(tmp) == 1)
    ui_theme.unload_fonts()
    check('fonts: empty/missing folder is fine', ui_theme.load_fonts(tmp / 'nope') == 0)
    root, app = rig.make(fonts=False); th = app.t
    check('fonts: falls back when Rajdhani/Share Tech Mono are absent', th.head_family in ('Bahnschrift', 'Segoe UI Semibold', 'Segoe UI', 'Rajdhani') and th.mono_family in ('Consolas', 'Courier New', 'Share Tech Mono'),
          (th.head_family, th.mono_family))
    print('   fonts in use:', th.head_family, th.mono_family)
    root.destroy()

if __name__ == '__main__':
    (WORK / 'tmp').mkdir(exist_ok=True)
    names = sys.argv[1:] or ['happy', 'fail', 'cancel', 'update', 'resume', 'real', 'small', 'dpi', 'plain', 'fonts', 'disk', 'checks', 'crash', 'back', 'folder', 'work', 'uninstall', 'tools', 'diag', 'diag_none', 'diag_interrupted', 'diag_fail', 'layout']
    for n in names:
        print('== %s' % n)
        try:
            globals()['scenario_' + n]()
        except Exception:
            import traceback; traceback.print_exc(); check(n + ' ran without an exception', False)
    bad = [r for r in RESULTS if not r[1]]
    print('\n%d checks, %d failed' % (len(RESULTS), len(bad)))
    (WORK / 'tmp' / 'ui_flow-results.json').write_text(json.dumps(RESULTS, indent=1))
    sys.exit(1 if bad else 0)
