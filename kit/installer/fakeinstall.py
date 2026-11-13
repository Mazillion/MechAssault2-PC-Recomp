"""Maintainer test hook: replays a recorded install log through the wizard without compiling anything.

  set MA2_INSTALLER_FAKE=<a recorded out.txt>   (optional: MA2_FAKE_SPEED=40, MA2_FAKE_FAIL=build:build_failed,
                                                 MA2_FAKE_EXTRA=textures-2x)

gui.py then uses FakeInstaller in place of install.Installer: same callbacks (log lines, step events), same exceptions.
"""
import re, threading, time
from pathlib import Path
import install

NAMES = ['check', 'disc', 'extract', 'analyze', 'recompile', 'generate', 'patch', 'build', 'assets', 'install', 'finish']

def parse(path):
    """[(title, [lines], seconds)] from a recorded install log."""
    steps, cur = [], None
    for raw in Path(path).read_text(encoding='utf-8', errors='replace').splitlines():
        m = re.match(r'^\[(\d+)/(\d+)\] (.+)$', raw)
        if m and not raw.startswith('  '):
            cur = [m.group(3), [], 0.0]; steps.append(cur); continue
        if cur is None: continue
        d = re.match(r'^\s+done in (?:(\d+)h )?(?:(\d+)m )?(\d+)s$', raw)
        if d:
            cur[2] = int(d.group(1) or 0) * 3600 + int(d.group(2) or 0) * 60 + int(d.group(3)); continue
        if raw.strip(): cur[1].append(raw)
    return steps

class FakeInstaller:
    def __init__(self, log_path, speed=40.0, fail=None, extra=(), dest=None, log=print, step=lambda *a: None, **kw):
        self.steps_data = parse(log_path)
        self.speed, self.log, self.on_step = float(speed), log, step
        self.fail = fail                       # ('build', 'build_failed')
        self.extra = list(extra)
        self.cancelled = False; self.notes = []; self.session_log = Path(dest or '.') / 'fake-install.log'
        self.dest = Path(dest) if dest else Path('.')
        self.state = {}; self.stage = ''; self.opts = kw.get('opts', {})
        self.on_plan = lambda names: None; self.until = None; self.done_steps = set(kw.get('already_done', ()))

    def step_names(self):
        n = [NAMES[i] if i < len(NAMES) else 'step%d' % i for i in range(len(self.steps_data))]
        return n[:-1] + self.extra + n[-1:] if self.extra else n

    def cancel(self):
        self.cancelled = True

    def run(self):
        names = self.step_names(); total = len(names)
        self.on_plan(names)
        data = {n: d for n, d in zip([x for x in names if x not in self.extra], self.steps_data)}
        for index, name in enumerate(names, 1):
            if self.cancelled: raise install.Stop('Cancelled.', 'cancelled')
            self.stage = name
            title, lines, secs = data.get(name, ['Generating %s' % name, ['  %d/%d textures' % (i, 600) for i in range(0, 601, 50)], 120.0])
            skipped = name in self.done_steps and name not in ('check', 'finish')
            self.on_step(index, total, title, name, skipped)
            if skipped:
                self.log('[%d/%d] %s: already done' % (index, total, title)); continue
            self.log('[%d/%d] %s' % (index, total, title))
            pause = (secs / self.speed) / max(1, len(lines))
            for i, line in enumerate(lines):
                if self.cancelled: raise install.Stop('Cancelled.', 'cancelled')
                if self.fail and self.fail[0] == name and i >= len(lines) // 2:
                    self.log('  FAILED: simulated failure')
                    raise install.Stop('%s failed (exit code 1). The last lines are above; the full log is %s.' % (title, self.session_log), self.fail[1])
                self.log(line); time.sleep(min(pause, 0.25))
            self.log('    done in %ds' % secs)
