"""Builds colrv1-test.ttf, a tiny COLRv1 font exercising every paint kind
msga draws (text_tests colrv1). Needs fontTools:

    python3 make_colrv1_test.py colrv1-test.ttf

U+E000 linear gradient, rotated (p2), reflect extend
U+E001 two-circle radial gradient, repeat extend
U+E002 sweep gradient, pad
U+E003 rotate + skew + translate of solid boxes
U+E004 composite: multiply circle over gradient, then dest-out hole
U+E005 PaintColrGlyph reuse of U+E000 scaled about the centre, plus a transform
U+E006 composite HSL hue + screen + soft light
U+E007 solid box (palette colour with alpha), foreground colour 0xFFFF
"""
import math
import sys

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib.tables import otTables as ot

UPEM = 1000


def rect(x0, y0, x1, y1):
    p = TTGlyphPen(None)
    p.moveTo((x0, y0))
    p.lineTo((x0, y1))
    p.lineTo((x1, y1))
    p.lineTo((x1, y0))
    p.closePath()
    return p.glyph()


def circle(cx, cy, r, n=48):
    p = TTGlyphPen(None)
    for i in range(n):
        a = -2 * math.pi * i / n  # clockwise: TrueType outer contours
        pt = (round(cx + r * math.cos(a)), round(cy + r * math.sin(a)))
        if i == 0:
            p.moveTo(pt)
        else:
            p.lineTo(pt)
    p.closePath()
    return p.glyph()


def empty():
    return TTGlyphPen(None).glyph()


names = [".notdef", "box", "small", "disc", "hole"] + ["c%d" % i for i in range(8)]
glyphs = {
    ".notdef": empty(),
    "box": rect(50, -150, 950, 750),
    "small": rect(350, 150, 650, 450),
    "disc": circle(500, 300, 330),
    "hole": circle(500, 300, 120),
}
for i in range(8):
    glyphs["c%d" % i] = empty()

palette = [
    (1, 0, 0, 1),  # 0 red
    (0, 0.6, 0, 1),  # 1 green
    (0, 0, 1, 1),  # 2 blue
    (1, 0.85, 0, 1),  # 3 yellow
    (0, 0, 0, 1),  # 4 black
    (1, 1, 1, 0.5),  # 5 half white
    (0.5, 0, 0.6, 1),  # 6 purple
]


def solid(i, a=1.0):
    return {"Format": ot.PaintFormat.PaintSolid, "PaletteIndex": i, "Alpha": a}


def stops(*s):
    return [{"StopOffset": o, "PaletteIndex": i, "Alpha": 1.0} for o, i in s]


def glyph(g, paint):
    return {"Format": ot.PaintFormat.PaintGlyph, "Glyph": g, "Paint": paint}


linear = glyph("box", {
    "Format": ot.PaintFormat.PaintLinearGradient,
    "ColorLine": {"Extend": "reflect", "ColorStop": stops((0, 0), (0.5, 3), (1, 2))},
    "x0": 200, "y0": 0, "x1": 500, "y1": 0, "x2": 300, "y2": 300,
})
radial = glyph("box", {
    "Format": ot.PaintFormat.PaintRadialGradient,
    "ColorLine": {"Extend": "repeat", "ColorStop": stops((0, 2), (0.5, 3), (1, 0))},
    "x0": 400, "y0": 250, "r0": 30, "x1": 550, "y1": 350, "r1": 220,
})
sweep = glyph("box", {
    "Format": ot.PaintFormat.PaintSweepGradient,
    "ColorLine": {"Extend": "pad", "ColorStop": stops((0, 0), (0.33, 1), (0.66, 2), (1, 6))},
    "centerX": 500, "centerY": 300, "startAngle": 45, "endAngle": 315,
})
transforms = [
    glyph("box", solid(3)),
    {"Format": ot.PaintFormat.PaintRotateAroundCenter, "angle": 30, "centerX": 500, "centerY": 300,
     "Paint": glyph("small", solid(0))},
    {"Format": ot.PaintFormat.PaintTranslate, "dx": -220, "dy": 220,
     "Paint": {"Format": ot.PaintFormat.PaintSkewAroundCenter, "xSkewAngle": 20, "ySkewAngle": 0,
               "centerX": 500, "centerY": 300, "Paint": glyph("small", solid(2))}},
    {"Format": ot.PaintFormat.PaintTranslate, "dx": 220, "dy": -220,
     "Paint": {"Format": ot.PaintFormat.PaintSkewAroundCenter, "xSkewAngle": 0, "ySkewAngle": 20,
               "centerX": 500, "centerY": 300, "Paint": glyph("small", solid(1))}},
]
composite = {
    "Format": ot.PaintFormat.PaintComposite,
    "CompositeMode": "dest_out",
    "SourcePaint": glyph("hole", solid(4)),
    "BackdropPaint": {
        "Format": ot.PaintFormat.PaintComposite,
        "CompositeMode": "multiply",
        "SourcePaint": glyph("disc", solid(6)),
        "BackdropPaint": linear,
    },
}
reuse = [
    {"Format": ot.PaintFormat.PaintScaleAroundCenter, "scaleX": 0.6, "scaleY": 0.8,
     "centerX": 500, "centerY": 300, "Paint": {"Format": ot.PaintFormat.PaintColrGlyph, "Glyph": "c0"}},
    {"Format": ot.PaintFormat.PaintTransform,
     "Transform": {"xx": 0.5, "yx": 0.2, "xy": -0.2, "yy": 0.5, "dx": 450, "dy": 350},
     "Paint": glyph("disc", solid(1, 0.7))},
]
def moved(dx, dy, paint):
    return {"Format": ot.PaintFormat.PaintTranslate, "dx": dx, "dy": dy, "Paint": paint}


# Each blend on a backdrop of its own: hue on the left, screen top right,
# soft light bottom right.
blend = [
    glyph("box", solid(3)),
    {"Format": ot.PaintFormat.PaintComposite, "CompositeMode": "hsl_hue",
     "SourcePaint": moved(-200, 0, glyph("hole", solid(2))),
     "BackdropPaint": moved(-200, 0, glyph("small", solid(3)))},
    {"Format": ot.PaintFormat.PaintComposite, "CompositeMode": "screen",
     "SourcePaint": moved(250, 200, glyph("hole", solid(0))),
     "BackdropPaint": moved(250, 200, glyph("small", solid(6)))},
    {"Format": ot.PaintFormat.PaintComposite, "CompositeMode": "soft_light",
     "SourcePaint": moved(250, -250, glyph("hole", solid(5))),
     "BackdropPaint": moved(250, -250, glyph("small", solid(6)))},
]
plain = [glyph("box", solid(0, 0.5)), glyph("small", {"Format": ot.PaintFormat.PaintSolid,
                                                     "PaletteIndex": 0xFFFF, "Alpha": 1.0})]

color = {
    "c0": linear, "c1": radial, "c2": sweep, "c3": (ot.PaintFormat.PaintColrLayers, transforms),
    "c4": composite, "c5": (ot.PaintFormat.PaintColrLayers, reuse),
    "c6": (ot.PaintFormat.PaintColrLayers, blend), "c7": (ot.PaintFormat.PaintColrLayers, plain),
}

fb = FontBuilder(UPEM, isTTF=True)
fb.setupGlyphOrder(names)
fb.setupCharacterMap({0xE000 + i: "c%d" % i for i in range(8)})
fb.setupGlyf(glyphs)
# Left side bearings = xMin, or the TrueType loader moves the outline there.
glyf = fb.font["glyf"]
fb.setupHorizontalMetrics({n: (1000, getattr(glyf[n], "xMin", 0)) for n in names})
fb.setupHorizontalHeader(ascent=800, descent=-200)
fb.setupNameTable({"familyName": "MSGA COLRv1 Test", "styleName": "Regular"})
fb.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200)
fb.setupPost()
fb.setupCOLR(color, version=1)
fb.setupCPAL([palette])
fb.save(sys.argv[1])
