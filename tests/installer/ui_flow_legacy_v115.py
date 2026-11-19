"""1.1.5 in the classic window (gui_legacy.py): the build waiting for free memory (a line on the Install page, not a failure) and the
out-of-memory stop's advice. The 1.1.3 flow itself stays in ui_flow_legacy.py, unchanged.
  python -B ui_flow_legacy_v115.py
"""
import json, os, sys
import ui_flow_legacy as F                                   # sets MA2_UI_MODULE=gui_legacy before the rig loads the window
from ui_flow_legacy import rig, pump, until, snap, check, SHOTS, WORK


def scenario_memwait():
    d = F.fresh_env(); os.environ['MA2_FAKE_SPEED'] = '20'; root, app = rig.make()
    F.walk_to_loadout(root, app, d)
    F.click(app, 'Install')
    p = app.pages['install']
    until(root, lambda: p.model and p.model.status.get('build') == 'active', 60)
    p.event('wait', {'free_gb': 1.4}); pump(root, 0.3)
    t = p.detail.cget('text')
    check('legacy memwait: the Install page says it is waiting for memory', t.startswith('Waiting for memory') and 'Discord' in t
          and 'by itself' in t and 'page file' in t and 'C:' in t, t)
    check('legacy memwait: the line wraps inside the page', int(p.detail.cget('wraplength') or 0) > 0 and p.detail.winfo_width() <= p.upper.winfo_width() + 2)
    check('legacy memwait: not a failure; Cancel still offered', not p.failed and 'Cancel' in F.buttons(app), F.buttons(app))
    snap(root, SHOTS / 'v115-memwait.png')
    p.event('wait', None); pump(root, 0.2)
    check('legacy memwait: carrying on: the step line again', not p.detail.cget('text').startswith('Waiting') and int(p.detail.cget('wraplength') or 0) == 0,
          p.detail.cget('text'))
    until(root, lambda: app.cur.key == 'done', 120)
    check('legacy memwait: the install still finishes', app.cur.key == 'done')
    root.destroy()
    d = F.fresh_env(); os.environ['MA2_FAKE_FAIL'] = 'build:out_of_memory'; root, app = rig.make()
    try:
        F.walk_to_loadout(root, app, d); F.click(app, 'Install'); until(root, lambda: app.pages['install'].failed, 60)
        p = app.pages['install']; pump(root, 0.4)
        msg = p.fail_msg.cget('text')
        check('legacy oom stop: headline and advice', 'ran out of memory' in p.fail_head.cget('text') and 'Discord' in msg and 'page file' in msg
              and 'Resume' in msg, (p.fail_head.cget('text'), msg))
        snap(root, SHOTS / 'v115-oom-stop.png')
    finally:
        os.environ.pop('MA2_FAKE_FAIL', None)
    root.destroy()


if __name__ == '__main__':
    (WORK / 'tmp').mkdir(exist_ok=True)
    try:
        scenario_memwait()
    except Exception:
        import traceback; traceback.print_exc(); check('memwait ran without an exception', False)
    bad = [r for r in F.RESULTS if not r[1]]
    print('\n%d checks, %d failed' % (len(F.RESULTS), len(bad)))
    sys.exit(1 if bad else 0)
