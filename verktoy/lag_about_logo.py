# Lager Bernttracker II-logoen i samme stil som 3D-logoen paa About-skjermen i Fasttracker II.
# Kjor fra prosjektmappa:  python verktoy/lag_about_logo.py      (krever: pip install numpy scipy pillow)
# Skriver:
#   src/gfxdata/ft2_bernt_about.h  - logoen til About-skjermen (541x78, 8-bit RLE-BMP som originalen)
#   web/bt2-logo.png               - stor utgave til startskjermen i nettversjonen
# Skrift: TeX Gyre Bonum (GUST Font License). Logoen er egen tegning, ikke en bearbeidelse av originalgrafikken.
import os, sys, struct
import numpy as np

from PIL import Image, ImageDraw, ImageFont
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))

def find_font(name):
    for d in (os.path.join(HERE, 'fonts'), '/usr/share/texmf/fonts/opentype/public/tex-gyre/',
              '/usr/share/fonts/opentype/tex-gyre/', os.path.expanduser('~/AppData/Local/Microsoft/Windows/Fonts'),
              'C:/Windows/Fonts'):
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    sys.exit('Fant ikke skriften %s (TeX Gyre Bonum, gratis fra https://www.gust.org.pl/projects/e-foundry/tex-gyre). '
             'Legg den i verktoy/fonts/.' % name)


FONT_WORD = find_font('texgyrebonum-bolditalic.otf')
FONT_II = find_font('texgyrebonum-bold.otf')
BASE = 400


def glyph(font, ch):
    l, t, r, b = font.getbbox(ch, anchor='ls')
    im = Image.new('L', (r - l + 8, b - t + 8), 0)
    ImageDraw.Draw(im).text((4 - l, 4 - t), ch, font=font, fill=255, anchor='ls')
    return im, (l - 4, t - 4), (l, t, r, b)


def layout(word='Bernttracker', track=0.84, k_word=0.10, k_ii=-0.34, ii_scale=1.05, gap=0.30, ii_adv=0.97):
    fw = ImageFont.truetype(FONT_WORD, BASE)
    fi = ImageFont.truetype(FONT_II, int(BASE * ii_scale))
    items = []  # (image, origin(x,y rel. to pen/baseline), pen_x, shear, ink bbox, group)
    pen = 0.0
    for ch in word:
        im, org, ink = glyph(fw, ch)
        items.append([im, org, pen, k_word, ink, 'word'])
        pen += fw.getlength(ch) * track
    pen += BASE * gap
    # the II: two separate strokes, back-slanted (moves the bottom to the right)
    for ch in 'II':
        im, org, ink = glyph(fi, ch)
        # shift so the slanted stroke starts at the pen position
        items.append([im, org, pen + (-k_ii) * (ink[3] - ink[1]) * 0.0, k_ii, ink, 'ii'])
        pen += fi.getlength(ch) * ii_adv
    return items


def sheared_bbox(items):
    xs, ys = [], []
    for im, org, px, k, ink, grp in items:
        l, t, r, b = ink
        for X, Y in ((l, t), (r, t), (l, b), (r, b)):
            xs.append(px + X - k * Y)
            ys.append(Y)
    return min(xs), min(ys), max(xs), max(ys)


def render(W, H, S, depth, face_lo=100, face_hi=190, ii_face=186, wordargs={}):
    dx, dy = depth
    items = layout(**wordargs)
    X0, Y0, X1, Y1 = sheared_bbox(items)
    CW, CH = W * S, H * S
    bx0, by0 = (dx + 1.5) * S, 1.0 * S
    bx1, by1 = (W - 1.0) * S, (H - dy - 1.5) * S
    sx = (bx1 - bx0) / (X1 - X0)
    sy = (by1 - by0) / (Y1 - Y0)
    if sx > sy:  # do not stretch wider than the font; centre instead
        extra = (bx1 - bx0) - (X1 - X0) * sy
        bx0 += extra / 2
        sx = sy

    rgb = np.zeros((CH, CW, 3), np.float32)
    alpha = np.zeros((CH, CW), np.float32)

    ex, ey = -dx * S, dy * S
    N = int(round(max(dx, dy) * S))
    el = np.hypot(ex, ey)
    L = np.array([-0.85, -0.35]); L /= np.hypot(*L)
    word_x0 = bx0
    word_x1 = max(bx0 + (px + ink[2] - k * ink[1] - X0) * sx for im, org, px, k, ink, grp in items if grp == 'word')

    for im, org, px, k, ink, grp in items:
        # canvas -> glyph image (inverse affine for PIL)
        a = 1.0 / sx
        b = k / sy
        c = X0 - bx0 / sx - px + k * Y0 - k * by0 / sy - org[0]
        d = 0.0
        e = 1.0 / sy
        f = Y0 - by0 / sy - org[1]
        m = np.asarray(im.transform((CW, CH), Image.AFFINE, (a, b, c, d, e, f), resample=Image.BICUBIC), np.float32) / 255.0
        face = m > 0.5
        if not face.any():
            continue
        ys_, xs_ = np.where(face)
        # work area: glyph bbox plus extrusion
        x0 = max(0, xs_.min() + int(ex) - 3 * S); x1 = min(CW, xs_.max() + 3 * S)
        y0 = max(0, ys_.min() - 3 * S); y1 = min(CH, ys_.max() + int(ey) + 3 * S)
        fa = face[y0:y1, x0:x1].astype(np.float32)
        bl = ndimage.gaussian_filter(fa, S * 0.9)
        gy, gx = np.gradient(bl)
        gl = np.hypot(gx, gy) + 1e-9
        nx, ny = -gx / gl, -gy / gl  # outward normal
        valid = gl > 1e-4

        R = rgb[y0:y1, x0:x1]; A = alpha[y0:y1, x0:x1]
        sweep = np.zeros_like(fa, bool)
        for j in range(N, 0, -1):
            t = j / N
            sxp, syp = int(round(ex * t)), int(round(ey * t))
            sh = ndimage.shift(fa, (syp, sxp), order=0) > 0.5
            snx = ndimage.shift(nx, (syp, sxp), order=0)
            sny = ndimage.shift(ny, (syp, sxp), order=0)
            sval = ndimage.shift(valid.astype(np.float32), (syp, sxp), order=0) > 0.5
            lam = np.clip(snx * L[0] + sny * L[1], 0, 1)
            lam = np.where(sval, lam, 0.15)
            near = np.clip(1.0 - t / 0.32, 0, 1)
            v = 27 + 18 * lam + 52 * lam * near ** 1.5
            # far back slightly darker
            v *= (1.0 - 0.10 * t)
            R[sh] = v[sh][:, None]
            A[sh] = 1
            sweep |= sh

        # dark separation line around the front face
        ring = ndimage.binary_dilation(face[y0:y1, x0:x1], iterations=max(1, int(S * 0.8))) & ~face[y0:y1, x0:x1]
        R[ring & sweep] = 14
        # dark outline around the whole letter (front + extrusion)
        full = sweep | face[y0:y1, x0:x1]
        outl = ndimage.binary_dilation(full, iterations=max(1, int(S * 0.9))) & ~full
        R[outl] = 9
        A[outl] = 1

        # front face: flat grey, lighter towards the right
        fy, fx = np.where(face[y0:y1, x0:x1])
        gxs = fx + x0
        if grp == 'word':
            base = face_lo + (face_hi - face_lo) * np.clip((gxs - word_x0) / (word_x1 - word_x0), 0, 1)
        else:
            base = np.full(fx.shape, float(ii_face))
        # bright rim along the inside of the front edges (strongest on the sides)
        inner = ndimage.distance_transform_edt(face[y0:y1, x0:x1])
        dist = inner[fy, fx] / S
        rim = np.clip(1.0 - (dist - 0.2) / 1.1, 0, 1)
        side = np.abs(nx[fy, fx]) * valid[fy, fx]
        topb = np.clip(-ny[fy, fx], 0, 1) * valid[fy, fx]
        val = base + rim * (16 + 22 * side + 8 * topb)
        val = np.minimum(val, 222)
        R[fy, fx] = val[:, None]
        A[fy, fx] = 1

    # neutral grey with a tiny bit of blue in the dark parts (like the original's 2B2A2A/1D1D1D tones)
    rgb[..., 2] *= 1.0
    img = np.dstack([np.clip(rgb, 0, 255), alpha * 255]).astype(np.uint8)
    im = Image.fromarray(img, 'RGBA')
    # premultiplied downscale
    pm = np.asarray(im, np.float32)
    pm[..., :3] *= pm[..., 3:4] / 255.0
    small = Image.fromarray(np.clip(pm, 0, 255).astype(np.uint8), 'RGBA').resize((W, H), Image.LANCZOS)
    s = np.asarray(small, np.float32)
    al = s[..., 3:4]
    col = np.where(al > 0, s[..., :3] * 255.0 / np.maximum(al, 1), 0)
    return np.dstack([np.clip(col, 0, 255), al]).astype(np.uint8)




# ---- 8-bit RLE BMP (leses av loadBMPTo32Bit i ft2_bmp.c)
def decode(src):
    (bfType, bfSize, r1, r2, bfOffBits, biSize, biWidth, biHeight, biPlanes, biBitCount,
     biCompression, biSizeImage, xp, yp, biClrUsed, biClrImportant) = struct.unpack_from('<HIHHIIiiHHIIiiII', src, 0)
    assert biCompression in (1, 2)
    pe = biClrUsed if biClrUsed else (1 << biBitCount)
    assert pe <= 256
    pal = []
    for i in range(pe):
        b, g, r, _ = src[0x36 + i * 4:0x3a + i * 4]
        pal.append((r << 16) | (g << 8) | b)
    out = np.full(biWidth * biHeight, pal[0], np.uint32)
    p = bfOffBits; x = 0; y = biHeight - 1
    while True:
        byte = src[p]; p += 1
        if byte == 0:
            byte = src[p]; p += 1
            if byte == 0: x = 0; y -= 1
            elif byte == 1: break
            elif byte == 2: x += src[p]; y -= src[p + 1]; p += 2
            else:
                assert biCompression == 1
                for i in range(byte):
                    out[y * biWidth + x + i] = pal[src[p]]; p += 1
                if byte & 1: p += 1
                x += byte
        else:
            idx = src[p]; p += 1
            assert biCompression == 1
            out[y * biWidth + x: y * biWidth + x + byte] = pal[idx]
            x += byte
    return out.reshape(biHeight, biWidth), pal, (biWidth, biHeight)

def encode(idx, pal, dpi=300):
    """idx: HxW array of palette indices (uint8); pal: list of 0xRRGGBB (<=256). Returns BMP bytes (RLE8)."""
    H, W = idx.shape
    data = bytearray()
    for y in range(H - 1, -1, -1):  # bottom-up
        row = [int(v) for v in idx[y]]
        x = 0
        while x < W:
            # run length at x
            run = 1
            while x + run < W and row[x + run] == row[x] and run < 255:
                run += 1
            if run >= 3 or W - x < 3:
                # encoded run(s) (also used for the last 1-2 pixels of a line)
                if run >= 3:
                    data += bytes((run, row[x])); x += run
                else:
                    data += bytes((1, row[x])); x += 1
                continue
            # absolute block: collect pixels until a run of >=3 starts (min 3 bytes, max 255)
            start = x
            n = 0
            while x + n < W and n < 255:
                r = 1
                while x + n + r < W and row[x + n + r] == row[x + n] and r < 3:
                    r += 1
                if r >= 3 and n >= 3:
                    break
                n += 1
            # absolute mode needs >= 3 pixels
            if n < 3:
                n = min(3, W - x)
                if n < 3:
                    for i in range(n):
                        data += bytes((1, row[x + i]))
                    x += n
                    continue
            data += bytes((0, n)) + bytes(row[x:x + n])
            if n & 1: data += b'\x00'
            x += n
        data += b'\x00\x00'  # end of line
    data[-2:] = b'\x00\x01'  # last end-of-line becomes end-of-bitmap
    npal = len(pal)
    palb = bytearray()
    for c in pal:
        palb += bytes((c & 255, (c >> 8) & 255, (c >> 16) & 255, 0))
    off = 14 + 40 + len(palb)
    ppm = int(round(dpi / 0.0254))
    hdr = struct.pack('<2sIHHI', b'BM', off + len(data), 0, 0, off)
    info = struct.pack('<IiiHHIIiiII', 40, W, H, 1, 8, 1, len(data), ppm, ppm, npal, npal)
    return bytes(hdr + info + palb + data)

def to_c_array(name, data, per_line=16):
    lines = []
    for i in range(0, len(data), per_line):
        lines.append('\t' + ','.join('0x%02X' % b for b in data[i:i + per_line]) + ',')
    lines[-1] = lines[-1].rstrip(',')
    return 'const uint8_t %s[%d] =\n{\n%s\n};\n' % (name, len(data), '\n'.join(lines))


def main():
    root = os.path.dirname(HERE)
    # About-skjermen: 541x78, 0x00FF00 = gjennomsiktig
    W, H = 541, 78
    sm = render(W, H, S=6, depth=(10.5, 11.5))
    a = sm[..., 3].astype(np.float32) / 255.0
    rgb = sm[..., :3].astype(np.float32) * a[..., None] + np.array([12, 12, 12], np.float32) * (1 - a[..., None])
    rgb = np.clip(np.round(rgb), 0, 255).astype(np.uint8)
    key = a < 0.5
    q = Image.fromarray(rgb[~key].reshape(-1, 1, 3), 'RGB').quantize(colors=255, method=Image.Quantize.MEDIANCUT,
                                                                       dither=Image.Dither.NONE)
    qpal = q.getpalette()
    qidx = np.asarray(q).reshape(-1)
    pal = [0x00FF00] + [(qpal[i * 3] << 16) | (qpal[i * 3 + 1] << 8) | qpal[i * 3 + 2] for i in range(qidx.max() + 1)]
    idx = np.zeros((H, W), np.uint8)
    idx[~key] = qidx + 1
    data = encode(idx, pal)
    dec, _, _ = decode(data)
    assert np.array_equal(dec, np.array(pal, np.uint32)[idx])
    hdr = ('// Bernttracker II-logo for About-skjermen (541x78 piksler, 8-bit RLE-BMP som originalen, 0x00FF00 = gjennomsiktig).\n'
           '// Egen tegning i samme stil som originalens 3D-logo (laget med verktoy/lag_about_logo.py, skrift: TeX Gyre Bonum).\n'
           '// Ikke rediger for haand.\n\n#pragma once\n\n#include <stdint.h>\n\n')
    body = to_c_array('bt2AboutLogoBMP', data, per_line=24).replace('const uint8_t', 'static const uint8_t', 1)
    open(os.path.join(root, 'src', 'gfxdata', 'ft2_bernt_about.h'), 'w', newline='\n').write(hdr + body)
    # startskjermen: stor utgave med gjennomsiktig bakgrunn
    BW = 1280
    f = BW / 541
    big = render(BW, round(BW * 78 / 541), S=3, depth=(10.5 * f, 11.5 * f))
    Image.fromarray(big, 'RGBA').save(os.path.join(root, 'web', 'bt2-logo.png'), optimize=True)
    print('Ferdig: src/gfxdata/ft2_bernt_about.h (%d byte) og web/bt2-logo.png' % len(data))


if __name__ == '__main__':
    main()
