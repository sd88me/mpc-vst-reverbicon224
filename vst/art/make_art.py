#!/usr/bin/env python3
"""Reverbicon 224 skin art, rendered to look like the original Lexicon 224 remote with real lighting: an ivory panel in a black
frame (light from the top left: frame shadow on the plate, recessed display with glass reflection), black keys and fader caps
graded from cineolX's own photographic sprites (keysprite.png, fadercap.png) so their bevel shading is kept, soft cast shadows,
recessed slots, glowing LED lenses. Fixed text is baked into bg.png; live text (algorithm, status, fader values) and the meter
are MPC's, laid on top by layout.conf.
Outputs (run from vst/art): bg.png, key_off/on.png, small_off/on.png, tog_off/on.png, cap.png, track.png, meter.png."""
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageChops
import os, random
here = os.path.dirname(os.path.abspath(__file__))
FONTS = os.path.join(here, "fonts")
W, H = 1280, 628
IVORY = (232, 224, 203)
BLACK = (22, 21, 20)
INK = (28, 26, 24)
LIGHT = (-1, -1)    # light comes from the top left: shadows fall down and to the right


def font(name, size):
    return ImageFont.truetype(os.path.join(FONTS, name), size)


TIT_B, TIT_SB, TIT_R = "TitilliumWeb-Bold.ttf", "TitilliumWeb-SemiBold.ttf", "TitilliumWeb-Regular.ttf"


def text(d, xy, s, f, fill=INK, anchor="la", spacing=0):
    if spacing:
        x, y = xy
        total = sum(d.textlength(c, font=f) + spacing for c in s) - spacing
        x -= {"l": 0, "m": total / 2, "r": total}[anchor[0]]
        for c in s:
            d.text((x, y), c, font=f, fill=fill, anchor="l" + anchor[1])
            x += d.textlength(c, font=f) + spacing
    else:
        d.text(xy, s, font=f, fill=fill, anchor=anchor)


def cast_shadow(sprite, dx, dy, blur, opacity, size):
    """A soft shadow of a sprite's silhouette on a transparent canvas of `size`."""
    a = Image.new("L", size, 0)
    a.paste(sprite.split()[-1].point(lambda v: int(v * opacity)), (dx, dy))
    a = a.filter(ImageFilter.GaussianBlur(blur))
    sh = Image.new("RGBA", size, (0, 0, 0, 0))
    sh.putalpha(a)
    return sh


def grade_black(sprite, hi=84, gamma=3.0, lo=6):
    """Re-colour a cream photographic sprite as black plastic, keeping its lighting: luminance -> charcoal range."""
    rgba = sprite.convert("RGBA")
    lum = rgba.convert("L")
    out = lum.point(lambda v: int(lo + hi * (max(0, v - 50) / 205.0) ** gamma))
    img = Image.merge("RGBA", (out.point(lambda v: min(255, v + 1)), out, out.point(lambda v: max(0, v - 1)), rgba.split()[-1]))
    return img


# ------------------------------------------------------------------ LED lens
def led_layer(size, cx, cy, r, on):
    """A recessed round LED lens with a specular highlight; lit ones glow (the bloom is a separate soft layer)."""
    S = 4
    L = Image.new("RGBA", (size[0] * S, size[1] * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(L)
    X, Y, R = cx * S, cy * S, r * S
    d.ellipse([X - R - 2 * S, Y - R - 2 * S, X + R + 2 * S, Y + R + 2 * S], fill=(8, 8, 8, 255), outline=(70, 68, 64, 255), width=S // 2)   # bezel
    for i in range(R, 0, -1):   # radial gradient lens
        t = i / R
        if on:
            col = (255, int(70 + 150 * (1 - t) ** 2.2), int(40 + 150 * (1 - t) ** 3), 255) if t < 0.85 else (170, 18, 14, 255)
        else:
            col = (int(70 + 30 * (1 - t)), 12, 10, 255) if t < 0.85 else (36, 6, 6, 255)
        d.ellipse([X - i, Y - i, X + i, Y + i], fill=col)
    d.ellipse([X - R // 2, Y - R // 2 - S, X - R // 8, Y - R // 8 - S], fill=(255, 235, 225, 190 if on else 90))   # specular dot
    L = L.resize(size, Image.LANCZOS)
    if on:
        bloom = Image.new("RGBA", size, (0, 0, 0, 0))
        ImageDraw.Draw(bloom).ellipse([cx - r - 4, cy - r - 4, cx + r + 4, cy + r + 4], fill=(255, 40, 20, 210))
        bloom = bloom.filter(ImageFilter.GaussianBlur(r * 0.9))
        out = Image.new("RGBA", size, (0, 0, 0, 0))
        out.alpha_composite(bloom)
        out.alpha_composite(L)
        return out
    return L


# ------------------------------------------------------------------ keys (3D sprite, LED above, cast shadow)
KEY = Image.open(os.path.join(here, "keysprite.png")).convert("RGBA")
KEY = KEY.crop(KEY.split()[-1].getbbox())
KEY_BLACK = grade_black(KEY)


def slate(sprite, hi=100, gamma=2.3, lo=16):
    """The key sprite graded to the blue-grey plastic of the real keys (its bevel lighting is kept)."""
    g = grade_black(sprite, hi=hi, gamma=gamma, lo=lo)
    r, gr, bl, a = g.split()
    return Image.merge("RGBA", (r.point(lambda v: max(0, v - 4)), gr, bl.point(lambda v: min(255, v + 12)), a))


CAP_SLATE = slate(KEY)


def key_image(cw, hw, hh, hx, band, slope, led_r, on):
    """A key drawn after the 224's: one slate housing in three zones -- a flat dark band with the LED, a sloped lighter bevel
    below it that widens outwards, and a large, nearly full-width flat cap with a thin lit lip. No well and no rim of its own:
    the dark between neighbours is the keybed showing through the gap (canvas = housing + the gap)."""
    S = 3
    big = Image.new("RGBA", (cw * S, hh * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    X0, X1 = hx * S, (hx + hw) * S - 1
    pw = hw * S
    d.rectangle([X0, 0, X1, hh * S - 1], fill=(34, 35, 40, 255))                                  # housing base (the sides and foot)
    for yy in range(0, band * S):                                                                 # LED band: flat dark slate, a touch lighter on top
        t = yy / (band * S)
        d.line([(X0, yy), (X1, yy)], fill=(int(60 - 10 * t), int(62 - 10 * t), int(70 - 10 * t), 255))
    d.line([(X0, 0), (X1, 0)], fill=(96, 98, 110, 255), width=S)                                  # top edge catching the light
    m = int(0.09 * pw)                                                                            # bevel strip: narrow at the top, full width at the foot
    y0, y1 = band * S, (band + slope) * S
    for yy in range(y0, y1):
        t = (yy - y0) / (y1 - y0)
        inset = int(m * (1 - t))
        v = int(92 - 20 * t)
        d.line([(X0 + inset, yy), (X1 - inset, yy)], fill=(v, v + 2, v + 8, 255))
    cm = 4 * S                                                                                    # cap: nearly the full width
    cap_top, cap_bot = y1, hh * S - 6 * S
    for yy in range(cap_top, cap_bot):
        t = (yy - cap_top) / (cap_bot - cap_top)
        v = (int(78 - 14 * t) - (10 if on else 0))
        d.line([(X0 + cm, yy), (X1 - cm, yy)], fill=(v, v + 2, v + 7, 255))
    d.line([(X0 + cm, cap_top), (X1 - cm, cap_top)], fill=(30, 31, 36, 255), width=S)           # where the slope meets the cap
    d.line([(X0 + cm, cap_bot - S), (X1 - cm, cap_bot - S)], fill=(128, 130, 140, 255), width=S)  # lit lip along the bottom
    d.line([(X0 + cm, cap_top), (X0 + cm, cap_bot)], fill=(112, 114, 124, 255), width=S)         # and down the left
    d.line([(X1 - cm, cap_top), (X1 - cm, cap_bot)], fill=(44, 45, 52, 255), width=S)            # shaded right side
    img = big.resize((cw, hh), Image.LANCZOS)
    if on:   # the lit LED throws a faint red onto the bevel below it
        glow = Image.new("RGBA", (cw, hh), (0, 0, 0, 0))
        ImageDraw.Draw(glow).rectangle([hx + 6, band - 1, hx + hw - 6, band + 3], fill=(255, 40, 20, 150))
        img.alpha_composite(glow.filter(ImageFilter.GaussianBlur(2)))
    img.alpha_composite(led_layer((cw, hh), hx + hw // 2, band // 2 + 1, led_r, on))
    return img


for on in (False, True):
    n = "on" if on else "off"
    key_image(66, 63, 84, 1, 29, 9, 5, on).save(os.path.join(here, "key_%s.png" % n))
    key_image(66, 63, 84, 1, 29, 9, 5, on).save(os.path.join(here, "tog_%s.png" % n))
    key_image(46, 44, 40, 1, 13, 5, 3, on).save(os.path.join(here, "small_%s.png" % n))

# ------------------------------------------------------------------ fader cap (photographic sprite, black plastic, pale index bar)
CAPSRC = Image.open(os.path.join(here, "fadercap.png")).convert("RGBA")
CAPSRC = CAPSRC.crop(CAPSRC.split()[-1].getbbox())
cl = CAPSRC.convert("L")
cw0, ch0 = CAPSRC.size
bar = Image.new("L", CAPSRC.size, 0)
bx0, bx1, by0, by1 = int(cw0 * 0.03), int(cw0 * 0.97), int(ch0 * 0.50), int(ch0 * 0.65)
bar.paste(cl.crop((bx0, by0, bx1, by1)).point(lambda v: 255 if v < 100 else 0), (bx0, by0))
body = grade_black(CAPSRC, hi=78)
pale = CAPSRC.convert("L").point(lambda v: int(176 + v * 0.45))
palergba = Image.merge("RGBA", (pale, pale, pale.point(lambda v: v - 6), CAPSRC.split()[-1]))
capimg = Image.composite(palergba, body, bar.filter(ImageFilter.GaussianBlur(1.2)))
CAPW, CAPH, CANW, CANH = 46, 42, 60, 64
spr = capimg.resize((CAPW, CAPH), Image.LANCZOS)
c = Image.new("RGBA", (CANW, CANH), (0, 0, 0, 0))
c.alpha_composite(cast_shadow(spr, (CANW - CAPW) // 2 + 4, (CANH - CAPH) // 2 + 6, 3.5, 0.65, (CANW, CANH)))
c.alpha_composite(spr, ((CANW - CAPW) // 2, (CANH - CAPH) // 2))
c.save(os.path.join(here, "cap.png"))

# recessed slot: dark groove with an inner shadow and a lit lower lip
TW, TH = 60, 300
tr = Image.new("RGBA", (TW, TH), (0, 0, 0, 0))
d = ImageDraw.Draw(tr)
g = TW // 2
d.rounded_rectangle([g - 7, 0, g + 7, TH - 1], 6, fill=(170, 162, 142, 255))                 # lip catching the light
d.rounded_rectangle([g - 6, 0, g + 5, TH - 3], 5, fill=(10, 10, 10, 255))
for i in range(5):                                                                            # inner shadow, darker at the top-left
    d.line([(g - 6 + i, 3), (g - 6 + i, TH - 5)], fill=(0, 0, 0, 200 - i * 35))
d.line([(g + 4, 3), (g + 4, TH - 5)], fill=(52, 50, 46, 255))
tr.save(os.path.join(here, "track.png"))

# ------------------------------------------------------------------ headroom meter strip (6 frames, vertical): LEDs lit cumulatively
MW, MH, N = 190, 40, 6
strip = Image.new("RGBA", (MW, MH * N), (0, 0, 0, 0))
for k in range(N):
    fr = Image.new("RGBA", (MW, MH), (0, 0, 0, 0))
    for i in range(k):
        fr.alpha_composite(led_layer((MW, MH), 19 + i * 38, 20, 5, True))
    strip.paste(fr, (0, k * MH))
strip.save(os.path.join(here, "meter.png"))

# ------------------------------------------------------------------ background
random.seed(224)
bg = Image.new("RGB", (W, H), BLACK)
pan = Image.new("RGB", (W - 28, H - 28), IVORY)
px = pan.load()
for _ in range(70000):   # paper grain
    x, y = random.randrange(pan.width), random.randrange(pan.height)
    k = random.randint(-9, 6)
    r, g_, b = px[x, y]
    px[x, y] = (r + k, g_ + k, b + k)
pan = pan.filter(ImageFilter.GaussianBlur(0.4))
# lighting: a soft gradient from the top left, a darker vignette in the corners
grad = Image.linear_gradient("L").rotate(45, expand=False).resize(pan.size)
pan = Image.composite(pan, Image.new("RGB", pan.size, (0, 0, 0)), Image.new("L", pan.size, 255))
shade = Image.new("L", pan.size, 255)
sd = ImageDraw.Draw(shade)
for i in range(60):   # corner vignette
    sd.rounded_rectangle([i * 3, i * 2, pan.width - i * 3, pan.height - i * 2], 40, outline=int(255 - 22 * (1 - i / 60) ** 2), width=4)
shade = shade.filter(ImageFilter.GaussianBlur(14))
pan = ImageChops.multiply(pan, Image.merge("RGB", (shade, shade, shade)))
# the black frame stands proud: its shadow falls on the plate along the top and left edges
fs = Image.new("L", pan.size, 0)
ImageDraw.Draw(fs).rectangle([0, 0, pan.width, 18], fill=170)
ImageDraw.Draw(fs).rectangle([0, 0, 14, pan.height], fill=140)
fs = fs.filter(ImageFilter.GaussianBlur(7))
pan = Image.composite(Image.new("RGB", pan.size, (60, 54, 44)), pan, fs)
m = Image.new("L", pan.size, 0)
ImageDraw.Draw(m).rounded_rectangle([0, 0, pan.width - 1, pan.height - 1], 12, fill=255)
bg.paste(pan, (14, 14), m)
d = ImageDraw.Draw(bg)
d.rounded_rectangle([14, 14, W - 15, H - 15], 12, outline=(40, 38, 35), width=2)
d.line([(24, H - 15), (W - 24, H - 15)], fill=(120, 114, 100), width=1)   # light catching the lower frame edge


def engraved_line(d, p0, p1):
    """A line cut into the plate: dark groove with a light edge under it."""
    d.line([p0, p1], fill=(120, 112, 96), width=1)
    d.line([(p0[0], p0[1] + 1), (p1[0], p1[1] + 1)], fill=(246, 240, 224), width=1)


# logo
lf, nf = font(TIT_B, 48), font(TIT_B, 56)
text(d, (46, 70), "reverbicon", lf, INK, "ls")
lw = d.textlength("reverbicon", font=lf)
text(d, (46 + lw + 8, 70), "224", nf, INK, "ls")
d.rectangle([46, 77, 46 + lw + 8 + d.textlength("224", font=nf), 81], fill=INK)
serif = Image.new("RGBA", (330, 60), (0, 0, 0, 0))
ImageDraw.Draw(serif).text((10, 8), "Digital Reverb", font=ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf", 34), fill=INK + (255,))
serif = serif.transform(serif.size, Image.AFFINE, (1, 0.28, -12, 0, 1, 0), Image.BICUBIC)
bg.paste(serif, (int(46 + lw + 8 + d.textlength("224", font=nf) + 24), 32), serif)

# display window: recessed bezel (shadow top/left, light bottom/right), dark red glass with a reflection
DX0, DY0, DX1, DY1 = 44, 96, 540, 186
d = ImageDraw.Draw(bg)
d.rounded_rectangle([DX0 - 6, DY0 - 6, DX1 + 6, DY1 + 6], 11, fill=(54, 50, 44))
d.rounded_rectangle([DX0 - 6, DY0 - 6, DX1 + 6, DY1 + 6], 11, outline=(246, 240, 224), width=1)
d.line([(DX0 - 4, DY0 - 6), (DX1 + 4, DY0 - 6)], fill=(20, 18, 16), width=2)
d.line([(DX0 - 6, DY0 - 4), (DX0 - 6, DY1 + 4)], fill=(20, 18, 16), width=2)
d.rounded_rectangle([DX0 - 2, DY0 - 2, DX1 + 2, DY1 + 2], 8, fill=(6, 6, 6))
glass = Image.new("RGB", (DX1 - DX0, DY1 - DY0), (24, 3, 3))
gd = ImageDraw.Draw(glass)
for yy in range(glass.height):
    k = 0.75 + 0.25 * yy / glass.height
    gd.line([(0, yy), (glass.width, yy)], fill=(int(26 * k), int(3 * k), int(3 * k)))
refl = Image.new("RGBA", glass.size, (0, 0, 0, 0))
rd = ImageDraw.Draw(refl)
rd.polygon([(0, 0), (int(glass.width * 0.62), 0), (int(glass.width * 0.38), glass.height), (0, glass.height)], fill=(255, 255, 255, 16))
refl = refl.filter(ImageFilter.GaussianBlur(3))
glass = Image.alpha_composite(glass.convert("RGBA"), refl).convert("RGB")
gm = Image.new("L", glass.size, 0)
ImageDraw.Draw(gm).rounded_rectangle([0, 0, glass.width - 1, glass.height - 1], 7, fill=255)
bg.paste(glass, (DX0, DY0), gm)
inner = Image.new("L", glass.size, 0)   # inner shadow along the top and left of the opening
ImageDraw.Draw(inner).rectangle([0, 0, glass.width, 7], fill=150)
ImageDraw.Draw(inner).rectangle([0, 0, 6, glass.height], fill=110)
inner = inner.filter(ImageFilter.GaussianBlur(4))
bg.paste(Image.new("RGB", glass.size, (0, 0, 0)), (DX0, DY0), ImageChops.multiply(inner, gm))
d = ImageDraw.Draw(bg)
# unlit headroom LEDs (the meter strip lights them) and their legend
for lbl, cy in (("L", 112), ("R", 150)):
    text(d, (62, cy), lbl, font(TIT_B, 20), (255, 90, 70), "lm")
    for i in range(5):
        bg.alpha_composite(led_layer((W, H), 104 + i * 38, cy, 5, False), (0, 0)) if bg.mode == "RGBA" else None
bgA = bg.convert("RGBA")
for cy in (112, 150):
    for i in range(5):
        lay = led_layer((40, 30), 20, 15, 5, False)
        bgA.alpha_composite(lay, (104 + i * 38 - 20, cy - 15))
bg = bgA.convert("RGB")
d = ImageDraw.Draw(bg)
for i, s in enumerate(("24", "18", "12", "6", "0dB")):
    text(d, (104 + i * 38, 131), s, font(TIT_SB, 14), (255, 96, 76), "mm")
text(d, (285, 169), "ovfl", font(TIT_R, 12), (255, 96, 76), "rm")
d.line([(296, DY0 + 8), (296, DY1 - 8)], fill=(80, 14, 12), width=1)
text(d, (DX0 + 4, DY1 + 17), "HEADROOM-dB", font(TIT_SB, 12), INK, "lm", spacing=1)

COLX = [108 + i * 68 for i in range(6)]
CAP = font(TIT_SB, 13)


def keybed(x0, y0, x1, y1):
    """The dark base the keys sit in: its shadow falls down and to the right; the gaps between keys show it."""
    global bg, d
    sh = Image.new("L", (W, H), 0)
    ImageDraw.Draw(sh).rounded_rectangle([x0 + 2, y0 + 6, x1 + 4, y1 + 9], 4, fill=150)
    sh = sh.filter(ImageFilter.GaussianBlur(4))
    bg.paste(Image.new("RGB", (W, H), (50, 44, 34)), (0, 0), sh)
    d = ImageDraw.Draw(bg)
    d.rounded_rectangle([x0, y0, x1, y1], 3, fill=(8, 8, 10))


keybed(COLX[0] - 36, 249, COLX[5] + 36, 339)
keybed(COLX[0] - 36, 437, COLX[1] + 36, 527)
keybed(288 - 26, 437, 432 + 26, 527)

text(d, (278, 216), "PROGRAM", font(TIT_B, 14), INK, "mm", spacing=3)
progs = (("SM HALL", "B"), ("VOC", "PLATE"), ("LG HALL", "B"), ("CHAMBER", ""), ("PERC", "PLATE"), ("SM HALL", "A"))
for i, (cx, (l1, l2)) in enumerate(zip(COLX, progs)):
    text(d, (cx, 238), str(i + 1), font(TIT_B, 16), INK, "mm")                 # the number above the key
    text(d, (cx, 358), l1, CAP, INK, "mm", spacing=1)                           # the name under it, as MODE ENH / DECAY OPT
    if l2:
        text(d, (cx, 373), l2, CAP, INK, "mm", spacing=1)
# function row labels (below the keys, as on the original)
for cx, (a, b) in zip(COLX[:2], (("MODE", "ENH"), ("DECAY", "OPT"))):
    text(d, (cx, 546), a, CAP, INK, "mm", spacing=1)
    text(d, (cx, 561), b, CAP, INK, "mm", spacing=1)
# output block: LEFT / RIGHT rows of four keys under one caption
OUTX = [288 + 48 * i for i in range(4)]
text(d, (254, 460), "L", font(TIT_B, 15), INK, "mm")
text(d, (254, 504), "R", font(TIT_B, 15), INK, "mm")
for cx, s in zip(OUTX, "ABCD"):
    text(d, (cx, 544), s, font(TIT_B, 14), INK, "mm")
engraved_line(d, (OUTX[0] - 28, 556), (OUTX[3] + 28, 556))
text(d, ((OUTX[0] + OUTX[3]) // 2, 570), "OUTPUTS", CAP, INK, "mm", spacing=2)

# faders: guide lines across each slot, baked names below the live value
FX = [598 + i * 64 for i in range(7)] + [1074, 1138, 1202]
FTOP, FBOT = 112, 492
names = [("BASS", ""), ("MID", ""), ("CROSS", "OVER"), ("TREBLE", "DECAY"), ("DEPTH", ""), ("PRE-", "DELAY"),
         ("DIFFU-", "SION"), ("DIRT", ""), ("INPUT", ""), ("MIX", "")]
for cx, (a, b) in zip(FX, names):
    for k in range(6):
        y = FTOP + 28 + (FBOT - FTOP - 56) * k / 5
        engraved_line(d, (cx - 28, y), (cx + 28, y))
    text(d, (cx, 544), a, CAP, INK, "mm")
    if b:
        text(d, (cx, 559), b, CAP, INK, "mm")
engraved_line(d, (1028, FTOP), (1028, 572))
engraved_line(d, (FX[0] - 24, 579), (FX[1] + 24, 579))
text(d, ((FX[0] + FX[1]) // 2, 594), "REVERB TIME", CAP, INK, "mm", spacing=1)
bg.save(os.path.join(here, "bg.png"))
