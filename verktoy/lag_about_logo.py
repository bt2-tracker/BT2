# Lager Bernttracker II-logoen som en kopi av den klassiske 3D-logoen fra Fasttracker II (About-skjermen),
# der det staar «Bernttracker» i stedet for «Fasttracker».
# Kjor fra prosjektmappa:  python verktoy/lag_about_logo.py      (krever: pip install numpy scipy pillow)
# Skriver:
#   src/gfxdata/ft2_bernt_about.h  - logoen til About-skjermen (485x78, 4-bit RLE-BMP som originalen,
#                                    fargene er FT2-palettens, saa den foelger fargetemaet slik originalen gjorde)
#   web/bt2-logo.png               - samme logo i fargene til temaet «Blues» (startskjermen paa nett og i Windows)
#
# Slik blir den laget (tar ca. et halvt minutt):
#   - «ttracker II» er originalen, piksel for piksel.
#   - «e» og «r» i «Bern» er originalens egne bokstaver, flyttet paa plass i riktig perspektiv (forsidene sees
#     nedenfra: loddrette streker moetes i et punkt langt over logoen, og 3D-dybden gaar mot et punkt under den).
#   - «B» og «n» finnes ikke i originalen. De er tegnet med skriften TeX Gyre Termes Bold, malt opp til samme
#     tykkelse og bredde som originalens bokstaver.
#   - Alle flatene til de nye bokstavene (forside, sider, skygger) fylles med biter av originalens egen
#     pikselgrafikk: hver piksel kopieres fra et sted i originalen som har samme slags plass (forside / side /
#     kant, samme retning og dybde), og naboer kopierer helst fra nabopiksler, saa hele biter blir med.
#
# Originalgrafikken (verktoy/ft2_original_logo.bmp, fra ft2-clone sin About-skjerm foer 2024) er laget av
# Magnus «Vogue» Hoegdahl og har lisens CC BY-NC-SA 4.0 (se src/gfxdata/bmp/LICENSE.txt). Den nye logoen er
# en bearbeidelse av den, og har samme lisens.
# Skrift: TeX Gyre Termes (GUST Font License).
import os, sys, struct
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy import ndimage as ndi

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# roles (the original BMP's palette order): 0 k (FT2 button text) 1 W (foreground) 2 background (transparent)
# 3 w (button light) 4 m (buttons) 5 d (button dark)
BG = 2
BMP_PAL = [0xABCDEF, 0xFFFFFF, 0x00FF00, 0xD3D3D3, 0x7F7F7F, 0x434343]  # = FT2 palette numbers 7, 5, -, 11, 6, 10
BLUES = [(0, 0, 0), (255, 255, 255), None, (255, 255, 255), (150, 150, 182), (53, 53, 65)]  # theme «Blues»

# the new word: letters, where each one comes from, and the gaps between the fronts (in flat letter pixels)
LETTERS = [('B', 'font', 'B'), ('e', 'orig', 'e'), ('r', 'orig', 'r1'), ('n', 'font', 'n')]
GAPS = [6.5, 7.0, 5.0, 7.0]  # B-e, e-r, r-n, n-t
SEED = 3

# ------------------------------------------------------------------------------------------------
# the original logo


def decode_bmp(data):
    """Decodes FT2's RLE BMPs the same way as ft2_bmp.c. Returns palette indices (top row first)."""
    off, = struct.unpack_from('<I', data, 10)
    w, h = struct.unpack_from('<ii', data, 18)
    comp, = struct.unpack_from('<I', data, 30)
    idx = np.zeros((h, w), np.int32)
    p, x, y = off, 0, h - 1
    while p < len(data):
        b = data[p]; p += 1
        if b == 0:
            b = data[p]; p += 1
            if b == 0:
                x = 0; y -= 1
            elif b == 1:
                break
            elif b == 2:
                x += data[p]; y -= data[p + 1]; p += 2
            elif comp == 1:
                for i in range(b):
                    if x + i < w: idx[y, x + i] = data[p]
                    p += 1
                if b & 1: p += 1
                x += b
            else:
                for i in range(b >> 1):
                    v = data[p]; p += 1
                    if x < w: idx[y, x] = v >> 4
                    x += 1
                    if x < w: idx[y, x] = v & 15
                    x += 1
                if ((b + 1) >> 1) & 1: p += 1
        else:
            v = data[p]; p += 1
            for i in range(b):
                if x < w:
                    idx[y, x] = v if comp == 1 else ((v >> 4) if (i & 1) == 0 else (v & 15))
                x += 1
    return idx


def find_fronts(c):
    """the letters' front faces: the light pixels plus the mid pixels between them"""
    ink = c != BG
    wl = c == 3
    f = wl.copy()
    hz = np.zeros_like(f); hz[:, 1:-1] = wl[:, :-2] & wl[:, 2:]
    vt = np.zeros_like(f); vt[1:-1] = wl[:-2] & wl[2:]
    dg = np.zeros_like(f); dg[1:-1, 1:-1] = (wl[:-2, :-2] & wl[2:, 2:]) | (wl[:-2, 2:] & wl[2:, :-2])
    f |= (c == 4) & (hz | vt | dg)
    f = ndi.binary_closing(f, structure=np.ones((1, 3))) & ink
    f = ndi.binary_closing(f, structure=np.ones((3, 1))) & ink
    holes = ndi.binary_fill_holes(f) & ~f
    hl, hn = ndi.label(holes)
    for i, s in enumerate(ndi.sum(holes, hl, range(1, hn + 1))):
        if s <= 3:
            f[hl == i + 1] = True
    lab, n = ndi.label(f)
    for i, s in enumerate(ndi.sum(f, lab, range(1, n + 1))):
        if s < 6:
            f[lab == i + 1] = False
    return f


# where each letter of "Fasttracker II" starts in the original
STARTS = {'F': 2, 'a': 52, 's': 89, 't1': 124, 't2': 147, 'r1': 167, 'a2': 198, 'c': 234, 'k': 262, 'e': 304,
          'r2': 332, 'I1': 371, 'I2': 397}
NAMES = list(STARTS)
BLOCK_NAMES = ('t1', 't2', 'r1', 'a2', 'c', 'k', 'e', 'r2', 'I1', 'I2')  # «ttracker II» is kept as it is

# 3D: the depth goes towards a point under the logo, the fronts are seen from below (vertical lines meet far above)
VP0 = 236.0          # x of the depth's vanishing point (y = VPY)
VPY = 600.0
DEPTH = 19.0
BASE_Y = 57.0
XV = 226.3           # x of the vertical lines' vanishing point
DV = 398.0           # its distance above the baseline (row 56)
BASE = 56.0
YV = BASE - DV
UP = 8               # letter shapes are worked on at 8x for smooth edges


def side_geometry(front, vpx, vpy=VPY, depth=DEPTH, steps=300):
    H, W = front.shape
    tmax = depth / (vpy - BASE_Y)
    yy, xx = np.mgrid[0:H, 0:W].astype(float)
    trel = np.full((H, W), np.nan)
    sx = np.zeros((H, W), int); sy = np.zeros((H, W), int)
    todo = ~front
    for i in range(1, steps + 1):
        t = tmax * i / steps
        px = np.round((xx - t * vpx) / (1 - t)).astype(int)
        py = np.round((yy - t * vpy) / (1 - t)).astype(int)
        ok = todo & (px >= 0) & (px < W) & (py >= 0) & (py < H)
        hit = np.zeros((H, W), bool)
        hit[ok] = front[py[ok], px[ok]]
        trel[hit] = i / steps
        sx[hit] = px[hit]; sy[hit] = py[hit]
        todo &= ~hit
    return ~np.isnan(trel), trel, sx, sy


class Original:
    def __init__(self):
        idx = decode_bmp(open(os.path.join(HERE, 'ft2_original_logo.bmp'), 'rb').read())
        self.C = idx[:78].copy()  # (the copyright text under the logo is not used)
        self.F = find_fronts(self.C)
        H, W = self.C.shape
        lab0, n0 = ndi.label(self.F)
        objs = ndi.find_objects(lab0)
        lab = np.zeros_like(lab0)
        self.L = {}
        for i in range(n0):
            x0 = objs[i][1].start
            nm = min(STARTS, key=lambda k: abs(STARTS[k] - x0))
            if abs(STARTS[nm] - x0) > 3:  # a piece inside a letter
                nm = max([k for k in STARTS if STARTS[k] <= x0], key=lambda k: STARTS[k])
            lid = NAMES.index(nm) + 1
            lab[lab0 == i + 1] = lid
            self.L[nm] = lid
        self.lab = lab
        side, trel, sx, sy = side_geometry(self.F, VP0)
        own = np.zeros((H, W), int)
        own[self.F] = lab[self.F]
        own[side] = lab[sy[side], sx[side]]
        dist, (iy, ix) = ndi.distance_transform_edt(own == 0, return_indices=True)
        ring = (self.C != BG) & (own == 0) & (dist <= 2.5)
        own[ring] = own[iy[ring], ix[ring]]
        self.own = own


# ------------------------------------------------------------------------------------------------
# letter shapes


def to_hi(mask):
    m = np.kron(mask.astype(float), np.ones((UP, UP)))
    return ndi.gaussian_filter(m, UP * 0.45) > 0.5


def to_lo(hi):
    H, W = hi.shape
    h, w = H // UP, W // UP
    return hi[:h * UP, :w * UP].reshape(h, UP, w, UP).mean(axis=(1, 3)) > 0.5


def hi_crop(hi):
    ys, xs = np.nonzero(hi)
    return hi[:, xs.min():xs.max() + 1], xs.min()


def s_of(y):
    return (y - YV) / (BASE - YV)


def unproject(mask, xv=XV):
    """image mask -> flat (upright) letter at 8x; returns (flat, u0) where column i is u = u0 + i / UP"""
    hi = to_hi(mask)
    Hh, Wh = hi.shape
    ys, xs = np.nonzero(hi)
    u = xv + ((xs + 0.5) / UP - 0.5 - xv) / s_of((ys + 0.5) / UP - 0.5)
    u0 = np.floor(u.min()) - 2
    w = int(np.ceil(u.max() - u0)) + 3
    out = np.zeros((Hh, w * UP), bool)
    yy, uu = np.mgrid[0:Hh, 0:w * UP]
    ycl = (yy + 0.5) / UP - 0.5
    xi = xv + ((uu + 0.5) / UP - 0.5 + u0 - xv) * s_of(ycl)
    xh = np.round((xi + 0.5) * UP - 0.5).astype(int)
    ok = (xh >= 0) & (xh < Wh)
    out[ok] = hi[yy[ok], xh[ok]]
    return out, u0


def project(flat_hi, u0, W, xv=XV):
    """flat letter at 8x (column i is u = u0 + i / UP) -> image mask, W pixels wide"""
    Hh, Wf = flat_hi.shape
    out = np.zeros((Hh, W * UP), bool)
    yy, xx = np.mgrid[0:Hh, 0:W * UP]
    u = xv + ((xx + 0.5) / UP - 0.5 - xv) / s_of((yy + 0.5) / UP - 0.5)
    uh = np.round((u - u0 + 0.5) * UP - 0.5).astype(int)
    ok = (uh >= 0) & (uh < Wf)
    out[ok] = flat_hi[yy[ok], uh[ok]]
    return to_lo(out)


def find_font(name):
    for d in (os.path.join(HERE, 'fonts'), '/usr/share/texmf/fonts/opentype/public/tex-gyre/',
              '/usr/share/fonts/opentype/tex-gyre/', os.path.expanduser('~/AppData/Local/Microsoft/Windows/Fonts'),
              'C:/Windows/Fonts'):
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    sys.exit('Fant ikke skriften %s (TeX Gyre Termes, gratis fra https://www.gust.org.pl/projects/e-foundry/tex-gyre). '
             'Legg den i verktoy/fonts/.' % name)


def font_letter(ch, erode=0.5, width=0.9, cap=55.0):
    """a letter from TeX Gyre Termes Bold, made as thin and narrow as the original's letters"""
    path = find_font('texgyretermes-bold.otf')
    f0 = ImageFont.truetype(path, 1000)
    l, t, r, b = f0.getbbox('H')
    f = ImageFont.truetype(path, int(round(1000 * cap * UP / (b - t))))
    asc = f.getmetrics()[0]
    l, t, r, b = f.getbbox(ch)
    im = Image.new('L', (r - l + 8 * UP, 78 * UP), 0)
    ImageDraw.Draw(im).text((4 * UP - l, int((BASE + 1) * UP) - asc), ch, font=f, fill=255)
    g = np.array(im) > 127
    g = ndi.binary_erosion(g, structure=np.ones((1, int(round(2 * erode * UP)) + 1)))  # thinner stems
    im = Image.fromarray((g * 255).astype(np.uint8)).resize((max(1, int(round(g.shape[1] * width))), g.shape[0]),
                                                             Image.BILINEAR)
    g2 = np.array(im) > 127
    out = np.zeros_like(g)
    out[:, :g2.shape[1]] = g2
    return hi_crop(out)[0]


def orig_letter(o, nm):
    """an original letter, made upright; returns (flat at 8x, u of its left edge)"""
    flat, u0 = unproject(((o.lab == o.L[nm]) & o.F)[:78])
    ys, xs = np.nonzero(flat)
    return flat[:, xs.min():xs.max() + 1], u0 + xs.min() / UP


def gap_flat(a, ua, b, ub):
    g = 1e9
    for y in range(0, a.shape[0], 2):
        xa = np.nonzero(a[y])[0]; xb = np.nonzero(b[y])[0]
        if len(xa) and len(xb):
            g = min(g, (ub + xb.min() / UP) - (ua + (xa.max() + 1) / UP))
    return g


def layout(letters, gaps, right_flat, right_u):
    """places the letters right to left (from the first 't') so the gaps between them match"""
    placed = []
    nb, nbu = right_flat, right_u
    for (nm, fl), g in zip(reversed(letters), reversed(gaps)):
        u = nbu - g - fl.shape[1] / UP
        for _ in range(40):
            cur = gap_flat(fl, u, nb, nbu)
            if abs(cur - g) < 0.13:
                break
            u += cur - g
        placed.append((nm, fl, u))
        nb, nbu = fl, u
    return placed[::-1]


def project_letter(fl, u, W, ext):
    full = np.zeros((fl.shape[0], int((W + 8) * UP)), bool)
    x0 = int(round((u + ext) * UP))
    full[:, x0:x0 + fl.shape[1]] = fl
    return project(full, 0.0, W, xv=XV + ext)


# ------------------------------------------------------------------------------------------------
# texture: every new pixel is copied from a place in the original that has the same geometry

R = 4  # neighbourhood radius (9x9)
OFF = [(dy, dx) for dy in range(-R, R + 1) for dx in range(-R, R + 1) if (dy, dx) != (0, 0)]
OY = np.array([o[0] for o in OFF]); OX = np.array([o[1] for o in OFF])
WN = np.exp(-(OY ** 2 + OX ** 2) / (2 * 2.2 ** 2))


def features(front, vpx):
    """class of every pixel (0 background, 1 front, 2 side, 3/4 outline) and where in its face it sits"""
    H, W = front.shape
    side, trel, sx, sy = side_geometry(front, vpx)
    f = ndi.gaussian_filter(front.astype(float), 1.2)
    gy, gx = np.gradient(f)
    ang = np.arctan2(-gy, -gx)
    body = front | side
    dist_out = ndi.distance_transform_edt(~body)
    cls = np.zeros((H, W), np.int8)
    cls[(dist_out > 0) & (dist_out <= 1.0)] = 3
    cls[(dist_out > 1.0) & (dist_out <= 2.0)] = 4
    cls[side] = 2
    cls[front] = 1
    F = {'cls': cls, 'trel': np.nan_to_num(trel), 'sx': sx, 'sy': sy}

    def run(m, axis, rev):
        mm = np.flip(m, axis) if rev else m
        d = np.zeros(mm.shape, np.int16)
        if axis == 1:
            for i in range(mm.shape[1]):
                d[:, i] = np.where(mm[:, i], (d[:, i - 1] if i else 0) + 1, 0)
        else:
            for i in range(mm.shape[0]):
                d[i] = np.where(mm[i], (d[i - 1] if i else 0) + 1, 0)
        return np.flip(d, axis) if rev else d
    for k, (ax, rv) in {'dl': (1, False), 'dr': (1, True), 'du': (0, False), 'dd': (0, True)}.items():
        F[k] = np.minimum(run(front, ax, rv), 5).astype(float)
    a = np.zeros((H, W)); a[side] = ang[sy[side], sx[side]]
    F['ca'] = np.cos(a) * side; F['sa'] = np.sin(a) * side
    F['dfront'] = np.minimum(ndi.distance_transform_edt(~front), 8.0)
    F['dout'] = np.minimum(ndi.distance_transform_edt(body), 8.0)
    r = np.zeros((H, W), np.int16)
    for bit, (dy, dx) in enumerate(((0, 1), (0, -1), (1, 0), (-1, 0), (1, 1), (1, -1), (-1, 1), (-1, -1))):
        sh = np.zeros_like(body)
        ys = slice(max(dy, 0), H + min(dy, 0)); yd = slice(max(-dy, 0), H + min(-dy, 0))
        xs = slice(max(dx, 0), W + min(dx, 0)); xd = slice(max(-dx, 0), W + min(-dx, 0))
        sh[yd, xd] = body[ys, xs]
        r |= sh.astype(np.int16) << bit
    F['ring'] = r
    _, (iy, ix) = ndi.distance_transform_edt(~body, return_indices=True)
    F['ncls'] = cls[iy, ix].astype(float)
    F['nca'] = F['ca'][iy, ix]; F['nsa'] = F['sa'][iy, ix]
    return F


def geo_cost(FT, y, x, FS, cy, cx):
    c = FT['cls'][y, x]
    d = (FS['cls'][cy, cx] != c) * 100.0
    if c == 1:
        for k in ('dl', 'dr', 'du', 'dd'):
            d = d + np.abs(FS[k][cy, cx] - FT[k][y, x]) * 0.8
    elif c == 2:
        dot = FS['ca'][cy, cx] * FT['ca'][y, x] + FS['sa'][cy, cx] * FT['sa'][y, x]
        d = d + (1 - np.clip(dot, -1, 1)) * 6.0
        d = d + np.abs(FS['trel'][cy, cx] - FT['trel'][y, x]) * 5.0
        d = d + np.abs(FS['dfront'][cy, cx] - FT['dfront'][y, x]) * 0.5
        d = d + np.abs(FS['dout'][cy, cx] - FT['dout'][y, x]) * 0.7
    elif c in (3, 4):
        x_or = np.bitwise_xor(FS['ring'][cy, cx], FT['ring'][y, x])
        bits = np.zeros(len(cy))
        for b in range(8):
            bits += (x_or >> b) & 1
        d = d + bits * 0.6
        d = d + (FS['ncls'][cy, cx] != FT['ncls'][y, x]) * 2.0
        dot = FS['nca'][cy, cx] * FT['nca'][y, x] + FS['nsa'][cy, cx] * FT['nsa'][y, x]
        d = d + (1 - np.clip(dot, -1, 1)) * 2.0
    return d


def synthesize(T, known, src, FT, img, FS, todo, rng, passes=3, kapp=40, kappa=0.35):
    H, W = T.shape
    SH, SW = img.shape
    ipad = np.pad(img, R, constant_values=BG)
    cpad = np.pad(FS['cls'], R, constant_values=0)
    byclass = {c: np.nonzero(FS['cls'] == c) for c in (1, 2, 3, 4)}
    valid = FS['cls'] != 0
    clsT = FT['cls']
    Tpad = np.pad(T, R, constant_values=BG)
    Kpad = np.pad(known, R, constant_values=True)
    Cpad = np.pad(clsT, R, constant_values=0)
    srcp = np.pad(src, ((R, R), (R, R), (0, 0)), constant_values=-1)
    pts = list(zip(*np.nonzero(todo)))
    for it in range(passes):
        for (y, x) in pts:
            c = clsT[y, x]
            if c == 0:
                T[y, x] = BG; Tpad[y + R, x + R] = BG; Kpad[y + R, x + R] = True
                continue
            tv = Tpad[y + R + OY, x + R + OX]
            tk = Kpad[y + R + OY, x + R + OX]
            tc = Cpad[y + R + OY, x + R + OX]
            # continue copying from where the neighbours were copied from ...
            sy_ = srcp[y + R + OY, x + R + OX, 0]; sx_ = srcp[y + R + OY, x + R + OX, 1]
            ok = sy_ >= 0
            cy1 = sy_[ok] - OY[ok]; cx1 = sx_[ok] - OX[ok]
            inb = (cy1 >= 0) & (cy1 < SH) & (cx1 >= 0) & (cx1 < SW)
            cy1 = cy1[inb]; cx1 = cx1[inb]
            if len(cy1):
                v = valid[cy1, cx1]
                cy1 = cy1[v]; cx1 = cx1[v]
            # ... or from the places with the most similar geometry
            ys, xs = byclass[c]
            g = geo_cost(FT, y, x, FS, ys, xs)
            k = min(kapp, len(g))
            idx = np.argpartition(g + rng.rand(len(g)) * 0.05, k - 1)[:k]
            cy = np.concatenate([cy1, ys[idx]]); cx = np.concatenate([cx1, xs[idx]])
            coh = np.concatenate([np.ones(len(cy1), bool), np.zeros(k, bool)])
            yy = cy[:, None] + OY[None, :] + R; xx = cx[:, None] + OX[None, :] + R
            sv, sc = ipad[yy, xx], cpad[yy, xx]
            tex = ((sv != tv[None, :]) * tk[None, :] * WN[None, :]).sum(1) / max(1e-6, (tk * WN).sum())
            struct_ = ((sc != tc[None, :]) * WN[None, :]).sum(1) / WN.sum()
            cost = tex * 10 + struct_ * 6 + geo_cost(FT, y, x, FS, cy, cx)
            cost = np.where(coh, cost, cost * (1 + kappa) + 0.05)
            j = int(np.argmin(cost + rng.rand(len(cost)) * 1e-3))
            T[y, x] = img[cy[j], cx[j]]
            Tpad[y + R, x + R] = T[y, x]; Kpad[y + R, x + R] = True
            srcp[y + R, x + R] = (cy[j], cx[j])
    return T


# ------------------------------------------------------------------------------------------------


def build():
    o = Original()
    H, W0 = o.C.shape
    t1_flat, t1_u = orig_letter(o, 't1')
    letters = []
    for nm, kind, what in LETTERS:
        letters.append((nm, font_letter(what) if kind == 'font' else orig_letter(o, what)[0]))
    placed = layout(letters, GAPS, t1_flat, t1_u)
    minx = min(np.nonzero(project_letter(fl, u, W0 + 120, 100))[1].min() - 100 for nm, fl, u in placed)
    EXT = int(max(0, 3 - minx))  # the logo grows to the left
    Wn = W0 + EXT
    newf = np.zeros((H, Wn), bool)
    for nm, fl, u in placed:
        newf |= project_letter(fl, u, Wn, EXT)[:H]
    # «ttracker II»: copied (stray ink the 3D model does not explain goes with the nearest letter)
    own = o.own.copy()
    dist, (iy0, ix0) = ndi.distance_transform_edt(o.own == 0, return_indices=True)
    stray = (o.C != BG) & (o.own == 0) & (dist <= 4)
    own[stray] = o.own[iy0[stray], ix0[stray]]
    blockown = np.isin(own, [o.L[n] for n in BLOCK_NAMES])
    T = np.full((H, Wn), BG, int)
    copied = np.zeros((H, Wn), bool)
    src = np.full((H, Wn, 2), -1, int)
    ys, xs = np.nonzero(blockown)
    T[ys, xs + EXT] = o.C[ys, xs]; copied[ys, xs + EXT] = True
    src[ys, xs + EXT, 0] = ys; src[ys, xs + EXT, 1] = xs
    fronts = newf.copy()
    ys, xs = np.nonzero(o.F & blockown)
    fronts[ys, xs + EXT] = True
    FT = features(fronts, VP0 + EXT)
    cls = FT['cls']
    side = cls == 2
    sidenew = side & newf[FT['sy'], FT['sx']]
    body = (cls == 1) | (cls == 2)
    _, (iy, ix) = ndi.distance_transform_edt(~body, return_indices=True)
    ringnew = np.isin(cls, (3, 4)) & (newf | sidenew)[iy, ix]
    todo = (newf | sidenew | ringnew) & (cls != 0)
    copied &= ~todo
    T[todo] = BG
    src[todo] = -1
    known = copied | (cls == 0)
    T[(cls == 0) & ~copied] = BG
    FS = features(o.F, VP0)
    T = synthesize(T, known, src, FT, o.C, FS, todo, np.random.RandomState(SEED))
    # the original never has a 2x2 block of the mid colour on a front
    fr = (cls == 1) & todo
    for _ in range(2):
        m = T == 4
        blk = m[:-1, :-1] & m[1:, :-1] & m[:-1, 1:] & m[1:, 1:] & fr[:-1, :-1] & fr[1:, :-1] & fr[:-1, 1:] & fr[1:, 1:]
        for y, x in zip(*np.nonzero(blk)):
            if T[y, x] == 4 and T[y, x + 1] == 4 and T[y + 1, x] == 4 and T[y + 1, x + 1] == 4:
                T[y, x + 1] = 3
                T[y + 1, x] = 3
    # check: everything right of the first 't' is the original, pixel for pixel
    assert np.array_equal(T[:, EXT + 140:], o.C[:, 140:])
    # an even width (one empty column at the right): then the web page can show it with every pixel exact
    if T.shape[1] & 1:
        T = np.concatenate([T, np.full((H, 1), BG, T.dtype)], axis=1)
    return T.astype(np.uint8)


def encode_rle4(idx, pal):
    """4-bit RLE BMP the way ft2_bmp.c reads it (even run lengths only)"""
    H, W = idx.shape
    data = bytearray()
    for y in range(H - 1, -1, -1):  # BMP rows go bottom-up
        row = list(idx[y]) + [0] * (W & 1)
        pairs = [(row[i] << 4) | row[i + 1] for i in range(0, len(row), 2)]
        i = 0
        lit = []

        def flush():
            j = 0
            while j < len(lit):
                n = min(len(lit) - j, 127)
                if n >= 2:
                    data.extend((0, 2 * n)); data.extend(lit[j:j + n])
                    if n & 1: data.append(0)
                else:
                    data.extend((2, lit[j]))
                j += n
            lit.clear()
        while i < len(pairs):
            k = 1
            while i + k < len(pairs) and pairs[i + k] == pairs[i] and k < 127:
                k += 1
            if k >= 2:
                flush()
                data.extend((2 * k, pairs[i]))
            else:
                lit.append(pairs[i])
            i += k
        flush()
        data.extend((0, 0))
    data[-2:] = b'\x00\x01'
    palb = bytearray()
    for c in pal:
        palb += bytes((c & 255, (c >> 8) & 255, (c >> 16) & 255, 0))
    off = 14 + 40 + len(palb)
    hdr = struct.pack('<2sIHHI', b'BM', off + len(data), 0, 0, off)
    info = struct.pack('<IiiHHIIiiII', 40, W, H, 1, 4, 2, len(data), 2834, 2834, len(pal), len(pal))
    return bytes(hdr + info + palb + data)


def to_c_array(name, data, per_line=24):
    lines = ['\t' + ','.join('0x%02X' % b for b in data[i:i + per_line]) + ',' for i in range(0, len(data), per_line)]
    lines[-1] = lines[-1].rstrip(',')
    return 'static const uint8_t %s[%d] =\n{\n%s\n};\n' % (name, len(data), '\n'.join(lines))


def main():
    T = build()
    H, W = T.shape
    data = encode_rle4(T, BMP_PAL)
    assert np.array_equal(decode_bmp(data), T)
    hdr = ('// Bernttracker II-logo for About-skjermen (%dx%d piksler, 4-bit RLE-BMP som originalen, 0x00FF00 = gjennomsiktig).\n'
           '// Fargene er FT2-palettens (som i originalens gamle About-logo), saa logoen foelger fargetemaet.\n'
           '// Bearbeidet fra den klassiske Fasttracker II-logoen av Magnus "Vogue" Hoegdahl, lisens CC BY-NC-SA 4.0\n'
           '// (laget med verktoy/lag_about_logo.py). Ikke rediger for haand.\n\n#pragma once\n\n#include <stdint.h>\n\n'
           '#define BT2_ABOUT_LOGO_W %d\n#define BT2_ABOUT_LOGO_H %d\n\n'
           '#ifndef BT2_ABOUT_LOGO_SIZE_ONLY // (ft2_about.c only needs the size)\n\n') % (W, H, W, H)
    open(os.path.join(ROOT, 'src', 'gfxdata', 'ft2_bernt_about.h'), 'w', newline='\n').write(
        hdr + to_c_array('bt2AboutLogoBMP', data) + '\n#endif\n')
    # the start screen: 1 pixel per logo pixel (the page scales it up with sharp pixels), transparent background
    im = Image.fromarray(T, 'P')
    pal = []
    for c in BLUES:
        pal += list(c) if c is not None else [0, 0, 0]
    im.putpalette(pal + [0] * (768 - len(pal)))
    im.info['transparency'] = BG
    im.save(os.path.join(ROOT, 'web', 'bt2-logo.png'), optimize=True, transparency=BG)
    print('Ferdig: src/gfxdata/ft2_bernt_about.h (%d byte) og web/bt2-logo.png (%dx%d)' % (len(data), W, H))


if __name__ == '__main__':
    main()
