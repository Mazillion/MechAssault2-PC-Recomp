"""Look of the installer: "gunmetal + HUD amber". Everything here is original: shapes drawn with Tk, system fonts,
and (when present) the OFL fonts in data/fonts/ (Rajdhani, Share Tech Mono; see data/fonts/FONTS.txt). No game assets.

Real ttk widgets are used for every control (keyboard focus, screen readers); Canvas is decoration only.
"""
import ctypes, math, sys
from pathlib import Path
import tkinter as tk
from tkinter import font as tkfont, ttk

HERE = Path(__file__).resolve().parent
FONT_DIR = HERE / 'data' / 'fonts'

DARK = dict(bg='#0B0E11', panel='#141A1F', panel2='#1B232A', line='#2B3640', grid='#161E24', steel='#5E6E7B', text='#DCE3E8',
            muted='#8E9DA8', amber='#FFB21A', amber_dim='#7A5410', orange='#FF6B1A', ok='#5BD68A', danger='#FF4B3E',
            on_amber='#14100A', hover='#26323B', amber_hover='#FFC24D', scan='#10161A', disabled_fill='#3A3220')
HC = dict(bg='SystemWindow', panel='SystemWindow', panel2='SystemButtonFace', line='SystemWindowText', grid='SystemWindow',
          steel='SystemGrayText', text='SystemWindowText', muted='SystemWindowText', amber='SystemHighlight',
          amber_dim='SystemHighlight', orange='SystemWindowText', ok='SystemWindowText', danger='SystemWindowText',
          on_amber='SystemHighlightText', hover='SystemButtonFace', amber_hover='SystemHighlight', scan='SystemWindow',
          disabled_fill='SystemButtonFace')
C = dict(DARK)

_loaded = []

def high_contrast():
    try:
        class HCS(ctypes.Structure):
            _fields_ = [('cb', ctypes.c_uint), ('flags', ctypes.c_uint), ('scheme', ctypes.c_wchar_p)]
        h = HCS(); h.cb = ctypes.sizeof(HCS)
        return bool(ctypes.windll.user32.SystemParametersInfoW(0x42, h.cb, ctypes.byref(h), 0) and h.flags & 1)
    except Exception:
        return False

def load_fonts(folder=None):
    """Make the shipped .ttf files available to this process only (FR_PRIVATE). Call before tk.Tk()."""
    folder = Path(folder or FONT_DIR)
    n = 0
    if folder.is_dir():
        for f in sorted(folder.glob('*.ttf')):
            try:
                if ctypes.windll.gdi32.AddFontResourceExW(str(f), 0x10, 0): _loaded.append(str(f)); n += 1
            except Exception:
                pass
    return n

def unload_fonts():
    for f in _loaded:
        try: ctypes.windll.gdi32.RemoveFontResourceExW(f, 0x10, 0)
        except Exception: pass
    _loaded.clear()

class Theme:
    def __init__(self, root, force_plain=False):
        global C
        self.root = root
        self.plain = force_plain or high_contrast()
        C.clear(); C.update(HC if self.plain else DARK)
        self.scale = max(1.0, root.winfo_fpixels('1i') / 96.0)
        fams = {f.lower(): f for f in tkfont.families(root)}
        pick = lambda *names: next((fams[n.lower()] for n in names if n.lower() in fams), 'Segoe UI')
        self.head_family = pick('Rajdhani', 'Bahnschrift', 'Segoe UI Semibold', 'Segoe UI')
        self.mono_family = pick('Share Tech Mono', 'Consolas', 'Courier New')
        self.body_family = pick('Segoe UI', 'Tahoma')
        self.fonts = {
            'h1': tkfont.Font(root, family=self.head_family, size=22, weight='bold'),
            'h2': tkfont.Font(root, family=self.head_family, size=14, weight='bold'),
            'kicker': tkfont.Font(root, family=self.head_family, size=10, weight='bold'),
            'body': tkfont.Font(root, family=self.body_family, size=10),
            'bold': tkfont.Font(root, family=self.body_family, size=10, weight='bold'),
            'small': tkfont.Font(root, family=self.body_family, size=9),
            'btn': tkfont.Font(root, family=self.head_family, size=12, weight='bold'),
            'chip': tkfont.Font(root, family=self.head_family, size=9, weight='bold'),
            'mono': tkfont.Font(root, family=self.mono_family, size=9),
            'mono_big': tkfont.Font(root, family=self.mono_family, size=26),
            'mono_mid': tkfont.Font(root, family=self.mono_family, size=13),
            'rail': tkfont.Font(root, family=self.head_family, size=12, weight='bold'),
            'wordmark': tkfont.Font(root, family=self.head_family, size=15, weight='bold'),
        }
        self.style()

    def S(self, px):
        return int(round(px * self.scale))

    def style(self):
        st = ttk.Style(self.root)
        try: st.theme_use('clam')
        except tk.TclError: pass
        F = self.fonts
        st.configure('.', background=C['bg'], foreground=C['text'], font=F['body'], bordercolor=C['line'], lightcolor=C['line'],
                     darkcolor=C['line'], troughcolor=C['panel'], focuscolor=C['amber'], insertcolor=C['text'], relief='flat')
        pad = (self.S(16), self.S(7))
        st.configure('TButton', background=C['panel2'], foreground=C['text'], font=F['btn'], padding=pad, borderwidth=1, anchor='center')
        st.map('TButton', background=[('disabled', C['panel']), ('pressed', C['line']), ('active', C['hover'])],
               foreground=[('disabled', C['steel'])], bordercolor=[('focus', C['amber']), ('active', C['amber_dim'])])
        st.configure('Primary.TButton', background=C['amber'], foreground=C['on_amber'], font=F['btn'], padding=pad, bordercolor=C['amber'])
        st.map('Primary.TButton', background=[('disabled', C['disabled_fill']), ('pressed', C['amber_dim']), ('active', C['amber_hover'])],
               foreground=[('disabled', C['steel'])], bordercolor=[('focus', C['text']), ('disabled', C['line'])])
        st.configure('Big.Primary.TButton', font=self.fonts['h2'], padding=(self.S(34), self.S(12)))
        st.configure('Link.TButton', background=C['bg'], foreground=C['amber'], font=F['body'], padding=(self.S(4), self.S(2)), borderwidth=0)
        st.map('Link.TButton', background=[('active', C['bg'])], foreground=[('active', C['amber_hover']), ('disabled', C['steel'])])
        for name, bg in (('TCheckbutton', C['bg']), ('Panel.TCheckbutton', C['panel']), ('TRadiobutton', C['bg']), ('Panel.TRadiobutton', C['panel']),
                         ('P2.TRadiobutton', C['panel2'])):
            st.configure(name, background=bg, foreground=C['text'], font=F['body'], indicatorbackground=C['panel2'],
                         indicatorforeground=C['amber'], padding=(self.S(2), self.S(3)))
            st.map(name, background=[('active', bg)], indicatorcolor=[('selected', C['amber']), ('!selected', C['panel2'])],
                   foreground=[('disabled', C['steel'])], bordercolor=[('focus', C['amber'])])
        st.configure('TEntry', fieldbackground=C['panel2'], foreground=C['text'], padding=self.S(6), bordercolor=C['line'], insertcolor=C['text'])
        st.map('TEntry', bordercolor=[('focus', C['amber'])], fieldbackground=[('disabled', C['panel'])])
        st.configure('Horizontal.TProgressbar', troughcolor=C['panel2'], background=C['amber'], bordercolor=C['line'],
                     lightcolor=C['amber'], darkcolor=C['amber'], thickness=self.S(8))
        st.configure('Vertical.TScrollbar', background=C['panel2'], troughcolor=C['panel'], arrowcolor=C['muted'], bordercolor=C['panel'])
        st.map('Vertical.TScrollbar', background=[('active', C['hover'])])

    # -- small constructors
    def label(self, parent, text='', font='body', fg='text', bg=None, **kw):
        return tk.Label(parent, text=text, font=self.fonts[font], fg=C[fg], bg=C[bg or self._bg(parent)], **kw)

    def _bg(self, parent):
        try:
            for k, v in C.items():
                if v == parent.cget('bg') and k in ('bg', 'panel', 'panel2'): return k
        except tk.TclError:
            pass
        return 'bg'

    def frame(self, parent, bg='bg', **kw):
        return tk.Frame(parent, bg=C[bg], **kw)

    def panel(self, parent, accent=False, cut=True, bg='panel', **kw):
        """A framed panel with a 1 px outline, chamfered top-left/bottom-right corners and amber corner ticks."""
        outline = C['amber_dim'] if accent else C['line']
        f = tk.Frame(parent, bg=C[bg], highlightbackground=outline, highlightcolor=outline, highlightthickness=1, **kw)
        if cut and not self.plain:
            c = self.S(10)
            for corner in ('nw', 'se'):
                cv = tk.Canvas(f, width=c, height=c, bg=C['panel'], highlightthickness=0, bd=0)
                if corner == 'nw':
                    cv.create_polygon(0, 0, c, 0, 0, c, fill=C[self._outer(parent)], outline=C[self._outer(parent)])
                    cv.create_line(c, 0, 0, c, fill=outline)
                    cv.place(x=-1, y=-1)
                else:
                    cv.create_polygon(c, c, 0, c, c, 0, fill=C[self._outer(parent)], outline=C[self._outer(parent)])
                    cv.create_line(c, 0, 0, c, fill=outline)
                    cv.place(relx=1.0, rely=1.0, x=1, y=1, anchor='se')
            if accent:
                t = self.S(14)
                for corner in ('ne', 'sw'):
                    cv = tk.Canvas(f, width=t, height=t, bg=C[bg], highlightthickness=0, bd=0)
                    if corner == 'ne': cv.create_line(0, 1, t, 1, fill=C['amber'], width=2); cv.create_line(t - 1, 0, t - 1, t, fill=C['amber'], width=2); cv.place(relx=1.0, x=0, y=0, anchor='ne')
                    else: cv.create_line(0, t - 1, t, t - 1, fill=C['amber'], width=2); cv.create_line(1, 0, 1, t, fill=C['amber'], width=2); cv.place(rely=1.0, x=0, y=0, anchor='sw')
        return f

    def _outer(self, parent):
        return self._bg(parent)

    def chip(self, parent, kind, text=None):
        """Status chip: glyph + word + colour. kind: ready, action, blocked, info, wait, error, warn."""
        spec = {'ready': ('✓', 'READY', 'ok'), 'action': ('!', 'ACTION', 'orange'), 'blocked': ('✕', 'BLOCKED', 'danger'),
                'info': ('i', 'INFO', 'muted'), 'wait': ('…', 'CHECKING', 'steel'), 'error': ('✕', 'FIX', 'danger'),
                'warn': ('!', 'NOTE', 'orange'), 'ok': ('✓', 'OK', 'ok')}[kind]
        lab = tk.Label(parent, text='%s %s' % (spec[0], text or spec[1]), font=self.fonts['chip'], fg=C[spec[2]],
                       bg=C[self._bg(parent)], width=11, anchor='w')
        return lab

    def set_chip(self, lab, kind, text=None):
        spec = {'ready': ('✓', 'READY', 'ok'), 'action': ('!', 'ACTION', 'orange'), 'blocked': ('✕', 'BLOCKED', 'danger'),
                'info': ('i', 'INFO', 'muted'), 'wait': ('…', 'CHECKING', 'steel'), 'error': ('✕', 'FIX', 'danger'),
                'warn': ('!', 'NOTE', 'orange'), 'ok': ('✓', 'OK', 'ok')}[kind]
        lab.configure(text='%s %s' % (spec[0], text or spec[1]), fg=C[spec[2]])

# ---------------------------------------------------------------------------- canvas art

def hexagon(cx, cy, r, rot=0.0):
    return [v for i in range(6) for v in (cx + r * math.cos(math.radians(60 * i + rot)), cy + r * math.sin(math.radians(60 * i + rot)))]

def draw_hex_grid(cv, w, h, size, color):
    """Faint hexagon grid (flat-top) over the canvas."""
    dx, dy = size * 1.5, size * math.sqrt(3)
    col = 0; x = 0.0
    while x < w + size:
        y = (dy / 2 if col % 2 else 0.0)
        while y < h + size:
            cv.create_polygon(hexagon(x, y, size), fill='', outline=color, tags='art')
            y += dy
        x += dx; col += 1

def draw_emblem(cv, cx, cy, r, line=None, amber=None, fill=None):
    """The original emblem: a flat-top hexagon with a targeting reticle (four brackets and a centre chevron)."""
    line, amber, fill = line or C['amber'], amber or C['amber'], fill or C['panel2']
    cv.create_polygon(hexagon(cx, cy, r), fill=fill, outline=amber, width=2)
    b, g = r * 0.52, r * 0.22
    for sx in (-1, 1):
        for sy in (-1, 1):
            x0, y0 = cx + sx * b, cy + sy * b
            cv.create_line(x0, y0 - sy * g, x0, y0, x0 - sx * g, y0, fill=line, width=2)
    cv.create_line(cx - r * 0.2, cy + r * 0.12, cx, cy - r * 0.16, cx + r * 0.2, cy + r * 0.12, fill=amber, width=2)
    cv.create_line(cx, cy - r * 0.34, cx, cy - r * 0.26, fill=amber, width=2)

def draw_radar(cv, w, h, scale):
    """Header art: a radar arc with range ticks and angled struts (generic HUD motifs)."""
    S = lambda v: v * scale
    cx, cy = w - S(70), h + S(10)
    for i, rad in enumerate((S(46), S(78), S(110))):
        cv.create_arc(cx - rad, cy - rad, cx + rad, cy + rad, start=20, extent=140, style='arc', outline=C['amber_dim'] if i == 2 else C['line'], width=1)
    for a in range(30, 151, 15):
        x0, y0 = cx + S(104) * math.cos(math.radians(a)), cy - S(104) * math.sin(math.radians(a))
        x1, y1 = cx + S(116) * math.cos(math.radians(a)), cy - S(116) * math.sin(math.radians(a))
        cv.create_line(x0, y0, x1, y1, fill=C['amber_dim'])
    cv.create_line(cx, cy, cx + S(110) * math.cos(math.radians(112)), cy - S(110) * math.sin(math.radians(112)), fill=C['amber'], width=2)
    cv.create_line(w - S(6), 0, w - S(6), h, fill=C['line'])
    cv.create_polygon(w - S(34), 0, w, 0, w, S(34), fill=C['amber_dim'], outline='')
    for y in range(0, int(h), max(3, int(S(3)))):
        cv.create_line(0, y, w, y, fill=C['scan'])
    cv.tag_lower('scan')

class Reticle(tk.Canvas):
    """Progress ring with tick marks, corner brackets and a readout (percent) in the middle."""
    def __init__(self, parent, theme, size=150):
        self.t = theme; self.n = theme.S(size)
        super().__init__(parent, width=self.n, height=self.n, bg=C['bg'], highlightthickness=0, bd=0)
        self.frac = 0.0; self.text = '0%'
        self.draw()

    def draw(self):
        S, n = self.t.S, self.n
        self.delete('all')
        m = S(14); cx = cy = n / 2
        for i in range(60):                      # tick marks
            a = math.radians(i * 6); long = i % 5 == 0
            r0, r1 = n / 2 - S(2), n / 2 - S(8 if long else 5)
            self.create_line(cx + r0 * math.cos(a), cy + r0 * math.sin(a), cx + r1 * math.cos(a), cy + r1 * math.sin(a),
                             fill=C['steel'] if long else C['line'])
        r = n / 2 - S(18)
        self.create_oval(cx - r, cy - r, cx + r, cy + r, outline=C['panel2'], width=S(7))
        if self.frac > 0:
            self.create_arc(cx - r, cy - r, cx + r, cy + r, start=90, extent=-min(359.9, 360 * self.frac), style='arc',
                            outline=C['amber'], width=S(7))
        b, g = S(4), S(14)
        for sx, sy in ((0, 0), (1, 0), (0, 1), (1, 1)):
            x = b if not sx else n - b; y = b if not sy else n - b
            self.create_line(x, y + (g if not sy else -g), x, y, x + (g if not sx else -g), y, fill=C['amber_dim'], width=2)
        self.create_text(cx, cy, text=self.text, font=self.t.fonts['mono_big'], fill=C['text'])

    def set(self, frac, text=None):
        self.frac = max(0.0, min(1.0, frac)); self.text = text if text is not None else '%d%%' % round(self.frac * 100)
        self.draw()
