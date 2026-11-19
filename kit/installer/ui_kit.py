"""The installer window's widget kit: one look ("gunmetal + HUD amber"), drawn on Tk canvases, with a full keyboard model.

Everything here is original (shapes drawn with Tk, the OFL fonts in data/fonts/ or Windows' own); no game art.
Every size is a logical (96 dpi) pixel passed through Kit.dp(); fonts use pixel sizes, so one scale factor sizes everything.

Widgets (each a focusable tk.Canvas; Tab / Shift+Tab move, a 2 px ring shows focus):
  HexButton  chamfered button (primary, ghost, big); Return / Space / click
  Link       text action (amber), quiet (muted) or danger; underline + ring on hover / focus
  Toggle     track, knob and an On / Off word; Space / click
  Segment    one choice of a few; Left / Right move, Space / click choose
  Zone       the disc drop target; Return / Space open Browse
"""
import math
import tkinter as tk
from tkinter import font as tkfont, ttk

DARK = dict(bg='#0B0E11', panel='#141A1F', panel2='#1B232A', line='#2B3640', hexline='#222D35', hexlit='#4A3A14', steel='#5E6E7B',
            text='#DCE3E8', muted='#8E9DA8', amber='#FFB21A', amber_dim='#7A5410', ok='#5BD68A', danger='#FF5A4E',
            on_amber='#14100A', hover='#26323B', amber_hover='#FFC24D', amber_press='#E09A10', dis_fill='#2E2A20', dis_text='#7D7563',
            ring='#DCE3E8')
HC = dict(bg='SystemWindow', panel='SystemWindow', panel2='SystemButtonFace', line='SystemWindowText', hexline='SystemWindow',
          hexlit='SystemWindow', steel='SystemGrayText', text='SystemWindowText', muted='SystemWindowText', amber='SystemHighlight',
          amber_dim='SystemHighlight', ok='SystemWindowText', danger='SystemWindowText', on_amber='SystemHighlightText',
          hover='SystemButtonFace', amber_hover='SystemHighlight', amber_press='SystemHighlight', dis_fill='SystemButtonFace',
          dis_text='SystemGrayText', ring='SystemWindowText')
P = dict(DARK)

def chamfer(x, y, w, h, c):
    return [x + c, y, x + w, y, x + w, y + h - c, x + w - c, y + h, x, y + h, x, y + c]

def hexagon(cx, cy, r, flat=False):
    rot = 0 if flat else 30
    return [v for i in range(6) for v in (cx + r * math.cos(math.radians(60 * i + rot)), cy + r * math.sin(math.radians(60 * i + rot)))]

class Kit:
    """Scale, colours and fonts for one window."""
    def __init__(self, root, scale, plain=False):
        self.root, self.scale, self.plain = root, scale, plain
        P.clear(); P.update(HC if plain else DARK)
        fams = {f.lower(): f for f in tkfont.families(root)}
        pick = lambda *names: next((fams[n.lower()] for n in names if n.lower() in fams), 'Segoe UI')
        self.head = pick('Rajdhani', 'Bahnschrift', 'Segoe UI Semibold', 'Segoe UI')
        self.body = pick('Segoe UI Variable Text', 'Segoe UI', 'Tahoma')
        self.mono = pick('Share Tech Mono', 'Consolas', 'Courier New')
        self.fonts = {}
        self.make_fonts()

    def dp(self, n):
        return int(round(n * self.scale))

    def line(self):
        return max(1, self.dp(1))

    def make_fonts(self):
        spec = {'h1': (self.head, 34, 'bold'), 'h1s': (self.head, 28, 'bold'), 'h1l': (self.head, 40, 'bold'), 'h1m': (self.head, 30, 'bold'),
                'pct': (self.head, 72, 'bold'), 'kick': (self.head, 15, 'bold'), 'zone': (self.head, 20, 'bold'), 'btn': (self.head, 19, 'bold'),
                'btn_big': (self.head, 26, 'bold'), 'word': (self.head, 62, 'bold'), 'word2': (self.head, 22, 'bold'),
                'body': (self.body, 14, 'normal'), 'body_u': (self.body, 14, 'normal'), 'small': (self.body, 13, 'normal'),
                'small_u': (self.body, 13, 'normal'), 'row': (self.body, 15, 'normal'), 'bold': (self.body, 14, 'bold'),
                'mono': (self.mono, 15, 'normal'), 'mono_s': (self.mono, 13, 'normal')}
        for k, (fam, px, w) in spec.items():
            kw = dict(family=fam, size=-self.dp(px), weight=w, underline=k.endswith('_u'))
            if k in self.fonts: self.fonts[k].configure(**kw)
            else: self.fonts[k] = tkfont.Font(self.root, **kw)

    def rescale(self, scale):
        self.scale = scale; self.make_fonts()

    def style_ttk(self):
        """Dark ttk pieces used on the Troubleshooting page (Combobox) and text pages (Scrollbar)."""
        st = ttk.Style(self.root)
        try: st.theme_use('clam')
        except tk.TclError: pass
        f = self.fonts['mono_s']
        st.configure('Dark.TCombobox', fieldbackground=P['panel2'], background=P['panel2'], foreground=P['text'], arrowcolor=P['muted'],
                     bordercolor=P['steel'], lightcolor=P['panel2'], darkcolor=P['panel2'], selectbackground=P['panel2'],
                     selectforeground=P['text'], padding=self.dp(3))
        st.map('Dark.TCombobox', fieldbackground=[('readonly', P['panel2'])], foreground=[('readonly', P['text'])],
               bordercolor=[('focus', P['ring'])], selectbackground=[('readonly', P['panel2'])], selectforeground=[('readonly', P['text'])])
        self.root.option_add('*TCombobox*Listbox.background', P['panel2'])
        self.root.option_add('*TCombobox*Listbox.foreground', P['text'])
        self.root.option_add('*TCombobox*Listbox.selectBackground', P['amber'])
        self.root.option_add('*TCombobox*Listbox.selectForeground', P['on_amber'])
        self.root.option_add('*TCombobox*Listbox.font', f)
        st.configure('Dark.Vertical.TScrollbar', background=P['panel2'], troughcolor=P['panel'], arrowcolor=P['muted'], bordercolor=P['panel'])
        st.map('Dark.Vertical.TScrollbar', background=[('active', P['hover'])])

# ---------------------------------------------------------------------------- widgets

class Widget(tk.Canvas):
    """A focusable canvas widget. Subclasses draw(); activate() runs the action."""
    activate_keys = ('<Return>', '<KP_Enter>', '<space>')

    def __init__(self, parent, kit, w, h, bg, enabled=True, key=None):
        super().__init__(parent, width=w, height=h, bg=bg, highlightthickness=0, bd=0, takefocus=1 if enabled else 0,
                         cursor='hand2' if enabled else '')
        self.kit, self.bgc, self.enabled, self.key = kit, bg, enabled, key
        self.hover = self.pressed = self.focused = False
        self.bind('<Enter>', lambda e: self._set(hover=True)); self.bind('<Leave>', lambda e: self._set(hover=False, pressed=False))
        cmd = self.register(self._focus)                      # with Tk's %d detail (tkinter's Event has none)
        self.tk.call('bind', self._w, '<FocusIn>', cmd + ' in %d'); self.tk.call('bind', self._w, '<FocusOut>', cmd + ' out %d')
        self.bind('<ButtonPress-1>', self._press); self.bind('<ButtonRelease-1>', self._release)
        for k in self.activate_keys: self.bind(k, self._key)

    def _set(self, **kw):
        for k, v in kw.items(): setattr(self, k, v)
        try: self.draw()
        except tk.TclError: pass

    def _focus(self, kind, detail):
        """Focus on this widget itself; Tk also reports focus moving into a link inside it (Virtual details), which is not ours."""
        if detail in ('NotifyVirtual', 'NotifyNonlinearVirtual', 'NotifyPointer'): return
        has = kind == 'in'
        if has != self.focused: self._set(focused=has)

    def _press(self, e):
        if not self.enabled: return
        self._set(pressed=True)
        try: self.focus_set()
        except tk.TclError: pass

    def _release(self, e):
        if not self.enabled or not self.pressed: return
        self._set(pressed=False)
        if 0 <= e.x <= int(self['width']) and 0 <= e.y <= int(self['height']): self.on_click(e)

    def _key(self, e):
        if self.enabled: self.invoke()
        return 'break'

    def on_click(self, e):
        self.invoke()

    def invoke(self):
        pass

    def set_enabled(self, on):
        self.enabled = on; self.configure(takefocus=1 if on else 0, cursor='hand2' if on else ''); self.draw()

    def draw(self):
        pass

class HexButton(Widget):
    def __init__(self, parent, kit, text, command, bg, big=False, ghost=False, enabled=True, key=None):
        dp = kit.dp
        self.text, self.command, self.big, self.ghost, self.danger = text, command, big, ghost, False
        self.font = kit.fonts['btn_big' if big else 'btn']
        self.bh, self.c, self.pad = dp(50 if big else 40), dp(12 if big else 9), dp(5)
        self.bw = max(dp(220 if big else 140), self.font.measure(text) + dp(64 if big else 52))
        super().__init__(parent, kit, self.bw + 2 * self.pad, self.bh + 2 * self.pad, bg, enabled, key)
        self.draw()

    def draw(self):
        self.delete('all')
        p, kit = self.pad, self.kit
        if not self.enabled: fill, tc = P['dis_fill'], P['dis_text']
        elif self.danger: fill, tc = (P['danger'] if self.hover or self.pressed else P['panel2']), (P['on_amber'] if self.hover or self.pressed else P['danger'])
        elif self.ghost: fill, tc = (P['hover'] if self.hover else P['panel2']), P['text']
        else: fill, tc = (P['amber_press'] if self.pressed else P['amber_hover'] if self.hover else P['amber']), P['on_amber']
        if self.focused and self.enabled:
            lw = max(2, kit.dp(2))
            self.create_polygon(chamfer(lw / 2, lw / 2, self.bw + 2 * p - lw, self.bh + 2 * p - lw, self.c + kit.dp(2)),
                                fill='', outline=P['ring'], width=lw)
        self.create_polygon(chamfer(p, p, self.bw, self.bh, self.c), fill=fill, outline=P['danger'] if self.danger else P['steel'] if self.ghost else '',
                            width=max(2, kit.dp(2)) if self.danger else kit.line())
        self.create_text(p + self.bw / 2, p + self.bh / 2, text=self.text, font=self.font, fill=tc)

    def invoke(self):
        if self.enabled and self.command: self.command()

class Link(Widget):
    """amber action link; quiet=True muted; danger=True red. icon: 'gear' draws the small hex gear before the text."""
    def __init__(self, parent, kit, text, command, bg, quiet=False, danger=False, icon=None, enabled=True, key=None, small=False):
        dp = kit.dp
        self.text, self.command, self.quiet, self.danger, self.icon = text, command, quiet, danger, icon
        self.f, self.fu = kit.fonts['small' if small else 'body'], kit.fonts['small_u' if small else 'body_u']
        self.iw = dp(20) if icon else 0
        self.pad = dp(4)
        w = self.f.measure(text) + self.iw + 2 * self.pad
        h = max(dp(24), self.f.metrics('linespace') + dp(4)) + 2 * dp(2)
        super().__init__(parent, kit, w, h, bg, enabled, key)
        self.draw()

    def colour(self):
        if not self.enabled: return P['dis_text']
        if self.danger: return P['danger']
        if self.quiet: return P['text'] if (self.hover or self.focused) else P['muted']
        return P['amber_hover'] if self.hover else P['amber']

    def draw(self):
        self.delete('all')
        w, h = int(self['width']), int(self['height'])
        col = self.colour(); lw = max(2, self.kit.dp(2))
        if self.focused and self.enabled:
            self.create_rectangle(lw / 2, lw / 2, w - lw / 2, h - lw / 2, outline=P['ring'], width=lw)
        x = self.pad
        if self.icon == 'gear':
            r = self.kit.dp(7); cx, cy = x + r, h / 2
            self.create_polygon(hexagon(cx, cy, r), fill='', outline=col, width=max(1, self.kit.dp(1.6)))
            self.create_oval(cx - self.kit.dp(2.2), cy - self.kit.dp(2.2), cx + self.kit.dp(2.2), cy + self.kit.dp(2.2), fill=col, outline='')
            x += self.iw
        self.create_text(x, h / 2, text=self.text, anchor='w', fill=col,
                         font=self.fu if (self.enabled and (self.hover or self.focused)) else self.f)

    def invoke(self):
        if self.enabled and self.command: self.command()

class Toggle(Widget):
    activate_keys = ('<space>',)                  # Return stays the window's primary action

    def __init__(self, parent, kit, value, command, bg, enabled=True, key=None, label=''):
        dp = kit.dp
        self.value, self.command, self.label = bool(value), command, label
        self.f = kit.fonts['small']
        self.tw, self.th = dp(40), dp(22)
        w = self.tw + dp(8) + max(self.f.measure('On'), self.f.measure('Off')) + dp(6)
        super().__init__(parent, kit, w, self.th + dp(6), bg, enabled, key)
        self.draw()

    def draw(self):
        self.delete('all')
        dp, lw = self.kit.dp, max(2, self.kit.dp(2))
        y0 = dp(3); on = self.value
        if self.focused and self.enabled:
            self.create_rectangle(lw / 2, lw / 2, int(self['width']) - lw / 2, int(self['height']) - lw / 2, outline=P['ring'], width=lw)
        tr_fill = P['amber'] if on and self.enabled else P['dis_fill'] if on else P['panel2']
        self.create_rectangle(dp(3), y0, dp(3) + self.tw, y0 + self.th, fill=tr_fill, outline=tr_fill if on else P['steel'], width=self.kit.line())
        k = dp(14); kx = dp(3) + (self.tw - k - dp(4) if on else dp(4)); ky = y0 + (self.th - k) / 2
        self.create_rectangle(kx, ky, kx + k, ky + k, fill=P['on_amber'] if on else (P['muted'] if self.enabled else P['dis_text']), outline='')
        self.create_text(dp(3) + self.tw + dp(8), y0 + self.th / 2, text='On' if on else 'Off', anchor='w', font=self.f,
                         fill=(P['text'] if on else P['muted']) if self.enabled else P['dis_text'])

    def invoke(self):
        if not self.enabled: return
        self.value = not self.value; self.draw()
        if self.command: self.command(self.value)

class Segment(Widget):
    """options: [(value, label, enabled)]."""
    activate_keys = ('<space>',)

    def __init__(self, parent, kit, options, value, command, bg, key=None):
        dp = kit.dp
        self.options, self.value, self.command = options, value, command
        self.f = kit.fonts['bold']
        self.ws = [max(dp(84), self.f.measure(lbl) + dp(32)) for _, lbl, _ in options]
        self.h = dp(34); self.m = dp(4)
        enabled = any(o[2] for o in options)
        super().__init__(parent, kit, sum(self.ws) + 2 * self.m, self.h + 2 * self.m, bg, enabled, key)
        vals = [o[0] for o in options]
        self.cursor = vals.index(value) if value in vals else next((i for i, o in enumerate(options) if o[2]), 0)
        self.bind('<Left>', lambda e: self.move(-1)); self.bind('<Right>', lambda e: self.move(1))
        self.draw()

    def move(self, d):
        i = self.cursor
        for _ in range(len(self.options)):
            i = (i + d) % len(self.options)
            if self.options[i][2]: self.cursor = i; break
        self.draw(); return 'break'

    def at(self, x):
        pos = self.m
        for i, w in enumerate(self.ws):
            if pos <= x < pos + w: return i
            pos += w
        return None

    def on_click(self, e):
        i = self.at(e.x)
        if i is not None and self.options[i][2]: self.cursor = i; self.choose(i)

    def invoke(self):
        self.choose(self.cursor)

    def choose(self, i):
        if not self.options[i][2]: return
        self.value = self.options[i][0]; self.draw()
        if self.command: self.command(self.value)

    def draw(self):
        self.delete('all')
        m, h, lw = self.m, self.h, max(2, self.kit.dp(2))
        x = m
        self.create_rectangle(m, m, m + sum(self.ws), m + h, fill=P['panel2'], outline=P['steel'], width=self.kit.line())
        for i, (val, lbl, en) in enumerate(self.options):
            w = self.ws[i]; sel = val == self.value
            fill = P['amber'] if sel and en else P['panel'] if not en else P['hover'] if (self.hover and i == self.cursor) else P['panel2']
            self.create_rectangle(x + 1, m + 1, x + w - 1, m + h - 1, fill=fill, outline='')
            if i: self.create_line(x, m, x, m + h, fill=P['steel'], width=self.kit.line())
            self.create_text(x + w / 2, m + h / 2, text=lbl, font=self.f, fill=P['on_amber'] if sel and en else P['text'] if en else P['dis_text'])
            if self.focused and i == self.cursor:
                self.create_rectangle(x - m + lw / 2 + 1, lw / 2, x + w + m - lw / 2 - 1, h + 2 * m - lw / 2, outline=P['ring'], width=lw)
            x += w

def dashed_rect(cv, x0, y0, x1, y1, colour, width, dash, gap, tags=()):
    """A dashed rectangle drawn as segments (Tk on Windows draws wide dashed lines solid)."""
    for (ax, ay, bx, by) in ((x0, y0, x1, y0), (x1, y0, x1, y1), (x1, y1, x0, y1), (x0, y1, x0, y0)):
        length = math.hypot(bx - ax, by - ay); n = 0.0
        while n < length:
            e = min(length, n + dash)
            cv.create_line(ax + (bx - ax) * n / length, ay + (by - ay) * n / length, ax + (bx - ax) * e / length, ay + (by - ay) * e / length,
                           fill=colour, width=width, tags=tags)
            n += dash + gap

class Zone(Widget):
    """The disc drop target: state 'empty' | 'checking' | 'ok' | 'bad'. The canvas holds the icon and the text; links are placed by the owner."""
    def __init__(self, parent, kit, command, bg, w, h, key='zone'):
        self.command = command; self.state = 'empty'; self.title = ''; self.sub = ''
        super().__init__(parent, kit, w, h, bg, True, key)

    def setup(self, state, title, sub):
        self.state, self.title, self.sub = state, title, sub; self.draw()

    def draw(self):
        self.delete('art')
        dp, w, h = self.kit.dp, int(self['width']), int(self['height'])
        bw = max(2, dp(2))
        self.create_rectangle(0, 0, w, h, fill=P['panel'], outline='', tags='art')
        if self.state == 'empty':
            dashed_rect(self, bw / 2, bw / 2, w - bw / 2, h - bw / 2, P['amber'] if (self.hover or self.focused) else P['steel'], bw, dp(6), dp(4), 'art')
        else:
            self.create_rectangle(bw / 2, bw / 2, w - bw / 2, h - bw / 2, outline=P['ok'] if self.state == 'ok' else P['danger'] if self.state == 'bad' else P['steel'],
                                  width=bw, tags='art')
        if self.focused:
            self.create_rectangle(bw * 2.5, bw * 2.5, w - bw * 2.5, h - bw * 2.5, outline=P['ring'], width=bw, tags='art')
        f = self.kit.fonts
        if self.state == 'empty':
            icon(self, 'disc', w / 2, dp(30), dp(22), 'art')
            self.create_text(w / 2, dp(64), text=self.title, font=f['zone'], fill=P['text'], tags='art')
        else:
            icon(self, {'ok': 'ok', 'bad': 'bad'}.get(self.state, 'wait'), dp(42), h / 2, dp(22), 'art')
            self.create_text(dp(78), h / 2 - dp(11), text=self.title, anchor='w', font=f['zone'], fill=P['text'], tags='art')
            self.create_text(dp(78), h / 2 + dp(13), text=self.sub, anchor='w', font=f['small'], fill=P['muted'], tags='art')
        self.tag_lower('art')

    def invoke(self):
        if self.command: self.command()

# ---------------------------------------------------------------------------- drawn icons and art

def icon(cv, kind, cx, cy, r, tags=()):
    """Hex-framed status icons: ok, bad, disc, wait, restart, blocked, info, needs."""
    s = r / 22.0
    w = lambda n: max(1, round(n * s))
    col = {'ok': P['ok'], 'bad': P['danger'], 'blocked': P['danger'], 'restart': P['amber'], 'needs': P['amber']}.get(kind, P['muted'])
    cv.create_polygon(hexagon(cx, cy, r), fill='', outline=col, width=w(3 if kind not in ('disc', 'wait', 'info') else 2.5), tags=tags)
    if kind == 'ok':
        cv.create_line(cx - 10 * s, cy, cx - 3 * s, cy + 7 * s, cx + 10 * s, cy - 7 * s, fill=col, width=w(4), tags=tags)
    elif kind == 'bad':
        cv.create_line(cx - 8 * s, cy - 8 * s, cx + 8 * s, cy + 8 * s, fill=col, width=w(4), tags=tags)
        cv.create_line(cx + 8 * s, cy - 8 * s, cx - 8 * s, cy + 8 * s, fill=col, width=w(4), tags=tags)
    elif kind == 'disc':
        cv.create_line(cx, cy - 11 * s, cx, cy + 5 * s, fill=P['amber'], width=w(3.5), tags=tags)
        cv.create_line(cx - 7 * s, cy - 2 * s, cx, cy + 5 * s, cx + 7 * s, cy - 2 * s, fill=P['amber'], width=w(3.5), tags=tags)
        cv.create_line(cx - 9 * s, cy + 11 * s, cx + 9 * s, cy + 11 * s, fill=P['muted'], width=w(3), tags=tags)
    elif kind == 'wait':
        for i in range(3): cv.create_oval(cx - 9 * s + i * 9 * s - 2 * s, cy - 2 * s, cx - 9 * s + i * 9 * s + 2 * s, cy + 2 * s, fill=P['muted'], outline='', tags=tags)
    elif kind == 'restart':
        cv.create_arc(cx - 12 * s, cy - 12 * s, cx + 12 * s, cy + 12 * s, start=110, extent=-300, style='arc', outline=col, width=w(3.5), tags=tags)
        cv.create_polygon(cx - 12 * s, cy - 10 * s, cx - 2 * s, cy - 9 * s, cx - 8 * s, cy - 1 * s, fill=col, outline='', tags=tags)
    elif kind == 'blocked':
        cv.create_oval(cx - 12 * s, cy - 12 * s, cx + 12 * s, cy + 12 * s, outline=col, width=w(3.5), tags=tags)
        cv.create_line(cx - 8.5 * s, cy + 8.5 * s, cx + 8.5 * s, cy - 8.5 * s, fill=col, width=w(3.5), tags=tags)
    elif kind == 'needs':
        cv.create_line(cx, cy - 11 * s, cx, cy + 3 * s, fill=col, width=w(4), tags=tags)
        cv.create_oval(cx - 2.5 * s, cy + 7 * s, cx + 2.5 * s, cy + 12 * s, fill=col, outline='', tags=tags)
    elif kind == 'info':
        cv.create_line(cx, cy - 1 * s, cx, cy + 9 * s, fill=P['text'], width=w(3), tags=tags)
        cv.create_line(cx, cy - 8 * s, cx, cy - 5 * s, fill=P['text'], width=w(3), tags=tags)

def small_hex(cv, x, cy, size, colour, tags=()):
    cv.create_polygon(hexagon(x + size / 2, cy, size / 2), fill=colour, outline=colour, tags=tags)

def emblem(cv, kit, x, y, tags=()):
    """The original fallback emblem for the Done screen (800 x 200 logical slot): a hex badge with a chevron, the name in Rajdhani."""
    dp = kit.dp
    cx, cy = x + dp(100), y + dp(104)
    cv.create_polygon(hexagon(cx, cy, dp(94)), fill='', outline=P['amber'], width=max(2, dp(4)), tags=tags)
    cv.create_polygon(hexagon(cx, cy, dp(64)), fill=P['panel'], outline=P['steel'], width=max(1, dp(2)), tags=tags)
    cv.create_line(cx - dp(30), cy + dp(22), cx, cy - dp(30), cx + dp(30), cy + dp(22), fill=P['amber'], width=max(3, dp(8)), tags=tags)
    cv.create_line(cx - dp(16), cy + dp(6), cx + dp(16), cy + dp(6), fill=P['amber'], width=max(3, dp(8)), tags=tags)
    for sx in (-1, 1):
        for sy in (-1, 1):
            bx, by = cx + sx * dp(80), cy + sy * dp(80)
            cv.create_line(bx, by - sy * dp(14), bx, by, bx - sx * dp(14), by, fill=P['steel'], width=max(1, dp(2)), tags=tags)
    cv.create_text(x + dp(226), y + dp(118), text='MECHASSAULT 2', anchor='sw', font=kit.fonts['word'], fill=P['text'], tags=tags)
    cv.create_text(x + dp(229), y + dp(124), text='PC RECOMP', anchor='nw', font=kit.fonts['word2'], fill=P['amber'], tags=tags)
    cv.create_line(x + dp(229), y + dp(160), x + dp(560), y + dp(160), fill=P['line'], width=kit.line(), tags=tags)
    small_hex(cv, x + dp(560), y + dp(160), dp(9), P['amber'], tags)

def hex_field(cv, kit, w, h, x_from):
    """The faint hex grid on the right of the window (drawn once, tag 'hex'). Returns [(item, centre x, centre y)]."""
    dp = kit.dp
    r = dp(20); hw = math.sqrt(3) * r; vh = 1.5 * r
    out = []
    row = -1
    while row * vh < h + r:
        col = -1
        while True:
            cx = col * hw + (hw / 2 if row % 2 else 0); cy = row * vh
            if cx > w + r: break
            if cx >= x_from:
                it = cv.create_polygon(hexagon(cx, cy, r), fill='', outline=P['hexline'], width=kit.line(), tags='hex')
                out.append((it, cx, cy))
            col += 1
        row += 1
    # one amber chamfered corner mark, top right
    c = dp(34)
    cv.create_polygon(w - c, 0, w, 0, w, c, fill=P['amber_dim'], outline='', tags='hex')
    cv.create_line(w - dp(56), dp(6), w - dp(6), dp(6), w - dp(6), dp(56), fill=P['amber'], width=max(1, dp(2)), tags='hex')
    return out
