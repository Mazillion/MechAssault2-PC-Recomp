"""Named stages, a weighted progress bar and an ETA for the install (pure; the wizard feeds it the engine's events).

Expected seconds per engine step come from measured runs (exe-test-1022/1029/1030: analyze 85 s, recompile 225 s,
generate 52-110 s, build 540 s at 8-12 jobs, everything else seconds). Edit STEP_SECONDS in one place when the code grows.
"""
import re, time

STEP_SECONDS = {'check': 2, 'disc': 3, 'extract': 20, 'verify': 3, 'analyze': 85, 'recompile': 225, 'generate': 80, 'patch': 20,
                'build': 540, 'assets': 5, 'install': 4, 'finish': 3}
QUICK_BUILD_SECONDS = 90         # 1.1.7: a quick rebuild (a few changed files and the link), measured in the release gate
QUICK_MINUTES = 3                 # 1.1.7: a whole update that reuses the kept build (checks, quick rebuild, menus, install)
# (id, title shown, engine steps)
CORE_STAGES = [('read', 'Reading your disc', ('check', 'disc', 'extract')),
               ('analyze', 'Analysing the Xbox program', ('analyze',)),
               ('translate', 'Translating to C', ('recompile', 'generate')),
               ('compile', 'Compiling the game', ('patch', 'build')),
               ('menus', 'Building PC menus', ('assets',)),
               ('install', 'Installing', ('install', 'finish'))]
ENHANCED = {'textures-2x': 'Enhanced textures 2x', 'textures-4x': 'Enhanced textures 4x', 'videos': 'Enhanced cutscenes'}

def fmt_eta(seconds):
    if seconds is None: return 'Estimating...'
    if seconds < 60: return 'Less than a minute left'
    if seconds < 3600: return 'About %d min left' % max(1, round(seconds / 60.0))
    h = seconds / 3600.0
    return 'About %.1f h left' % h if h < 10 else 'About %d h left' % round(h)

def fmt_elapsed(seconds):
    seconds = int(seconds)
    return '%d:%02d:%02d' % (seconds // 3600, seconds % 3600 // 60, seconds % 60) if seconds >= 3600 else '%d:%02d' % (seconds // 60, seconds % 60)

class ProgressModel:
    def __init__(self, steps, clock=time.monotonic):
        """steps: engine step names in order (as Installer.steps() lists them)."""
        self.clock = clock
        self.seconds = dict(STEP_SECONDS)      # 1.1.7: set_quick() shortens the build step for a quick rebuild
        self.steps = list(steps)
        self.status = {s: 'pending' for s in self.steps}
        self.sub = {}                  # step -> measured fraction inside it (0..1)
        self.detail = {}
        self.t0 = clock()
        self.start_at, self.dur = {}, {}      # step -> start time, measured seconds
        self.current = None
        self.core = [s for s in self.steps if s in self.seconds]
        self.stages = [(i, t, [s for s in ss if s in self.steps]) for i, t, ss in CORE_STAGES]
        self.stages = [x for x in self.stages if x[2]]
        self.stages += [(s, ENHANCED[s], [s]) for s in self.steps if s in ENHANCED]
        self.ninja = None

    def set_quick(self, quick):
        """1.1.7: the installer reuses its kept build (only changed files are compiled): the translation steps are kept and the
        build is short. False: back to the full build's numbers (the quick rebuild did not work)."""
        self.seconds['build'] = QUICK_BUILD_SECONDS if quick else STEP_SECONDS['build']

    # -- events from the engine
    def step(self, name, skipped=False):
        if self.current and self.status.get(self.current) == 'active': self.finish(self.current)
        if name not in self.status: self.status[name] = 'pending'; self.steps.append(name)
        if skipped:
            self.status[name] = 'kept'; return
        self.status[name] = 'active'; self.current = name; self.start_at[name] = self.clock()

    def finish(self, name):
        if self.status.get(name) == 'active':
            self.dur[name] = self.clock() - self.start_at.get(name, self.clock()); self.status[name] = 'done'
            self.sub[name] = 1.0

    def fail(self, name=None):
        name = name or self.current
        if name in self.status: self.status[name] = 'failed'

    def complete(self):
        for s in self.steps:
            if self.status[s] in ('active', 'pending'):
                self.status[s] = 'done'; self.sub[s] = 1.0

    def feed(self, line):
        """Read progress out of one engine log line."""
        cur = self.current
        if not cur: return
        m = re.search(r'\[(\d+)/(\d+)\]\s+(Building|Linking)', line)
        if m and cur == 'build':
            n, total = int(m.group(1)), max(1, int(m.group(2)))
            self.sub['build'] = min(0.99, n / float(total))
            self.detail['build'] = 'Compiling %d of %d files' % (n, total)
            return
        m = re.search(r'Translating ([\d,]+) functions', line)
        if m and cur == 'recompile':
            self.detail['recompile'] = 'Translating %s functions' % m.group(1); return
        if cur in ENHANCED:
            m = re.search(r'(\d+)\s*/\s*(\d+)', line)
            if m and int(m.group(2)) >= 10 and int(m.group(1)) <= int(m.group(2)):
                self.sub[cur] = min(0.99, int(m.group(1)) / float(m.group(2)))
                self.detail[cur] = 'Enhanced %s of %s' % (m.group(1), m.group(2))

    # -- derived numbers
    def _in_step(self, s):
        st = self.status.get(s)
        if st in ('done', 'kept'): return 1.0
        if st != 'active': return 0.0
        if s in self.sub: return self.sub[s]
        exp = self.seconds.get(s)
        if not exp: return 0.0
        return min(0.95, (self.clock() - self.start_at[s]) / float(exp))

    def stage_fraction(self, sid):
        for i, t, ss in self.stages:
            if i == sid:
                w = [self.seconds.get(s, 1) for s in ss]
                return sum(wi * self._in_step(s) for wi, s in zip(w, ss)) / float(sum(w))
        return 0.0

    def stage_status(self, sid):
        for i, t, ss in self.stages:
            if i == sid:
                sts = [self.status.get(s) for s in ss]
                if 'failed' in sts: return 'failed'
                if 'active' in sts: return 'active'
                if all(x == 'kept' for x in sts): return 'kept'
                if all(x in ('done', 'kept') for x in sts): return 'done'
                if any(x in ('done', 'kept') for x in sts): return 'active'
                return 'pending'
        return 'pending'

    def stage_detail(self, sid):
        for i, t, ss in self.stages:
            if i == sid:
                for s in ss:
                    if self.status.get(s) == 'active': return self.detail.get(s, '')
        return ''

    def core_fraction(self):
        total = sum(self.seconds[s] for s in self.core) or 1
        return sum(self.seconds[s] * self._in_step(s) for s in self.core) / float(total)

    def fraction(self):
        """The bar: the core install's weighted progress; during an enhancer stage, that stage's own progress."""
        cur = self.current
        if cur in ENHANCED and self.status.get(cur) == 'active': return self._in_step(cur)
        return self.core_fraction()

    def pace(self):
        """Actual / expected seconds over the finished steps (None until a heavy step has finished)."""
        done = [s for s in self.core if s in self.dur and self.seconds[s] >= 20]
        if not done: return None
        exp = sum(self.seconds[s] for s in done)
        return min(4.0, max(0.3, sum(self.dur[s] for s in done) / float(exp)))

    def eta(self):
        cur = self.current
        if cur in ENHANCED and self.status.get(cur) == 'active':
            f = self._in_step(cur); el = self.clock() - self.start_at[cur]
            return el * (1 - f) / f if f > 0.02 and el > 30 else None
        p = self.pace()
        if cur == 'build' and self.status.get('build') == 'active':
            f = self.sub.get('build', 0); el = self.clock() - self.start_at['build']
            if f > 0.05 and el > 20:
                rest = sum(self.seconds[s] * (1 - self._in_step(s)) for s in self.core if s != 'build')
                return el * (1 - f) / f + rest * (p or 1.0)
        if p is None: return None
        rest = sum(self.seconds[s] * (1 - self._in_step(s)) for s in self.core)
        return rest * p

    def elapsed(self):
        return self.clock() - self.t0
