#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Mykle Hansen
# Specified by Mykle Hansen, implemented by Claude (Anthropic).

"""
Generate the 3D (STEP) models for the parts that have no stock KiCad model, into pcb/3dmodels/ (git-ignored).

Needs CadQuery, which is not part of this repo's requirements:  python3 -m venv v && v/bin/pip install cadquery
Run:   v/bin/python tools/gen_3d.py [--only NAME ...]

Model coordinates follow KiCad's: x right, y toward the top of the board (the footprint's -y), z up, origin at the
footprint's origin, millimetres. Every model is drawn as it sits on the finished board (pluggable modules float above
their sockets).

Sources (for provenance, not redistributed here):
  Thing Plus  - component positions/footprints from SparkFun's Eagle board file (CC BY-SA 4.0), heights are estimates
  Mic breakout - outline/header from Adafruit's Eagle board file (Adafruit-I2S-Microphone-Breakout-PCB)
  Pot / knob   - Taiwan Alpha RV112FF datasheet, Same Sky SPK-013A datasheet
  Jack         - Kycon's own STX-3120-3B STEP (downloaded to 3dmodels/dl/, re-oriented to our footprint origin)
"""
import math, pathlib, re, sys, urllib.request
import xml.etree.ElementTree as ET
import cadquery as cq

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "3dmodels"
DL = OUT / "dl"
OUT.mkdir(exist_ok=True)
DL.mkdir(exist_ok=True)

SOCKET_H = 8.5          # female header height
SPACER_H = 2.5          # male header plastic under a module
MODULE_Z = SOCKET_H + SPACER_H   # underside of a plugged-in module
TP_SOCKET_H = 3.0                # every plug-in module (Thing Plus, OLED, mic) sits on LOW-PROFILE (3 mm) sockets
TP_MODULE_Z = TP_SOCKET_H + SPACER_H


# ------------------------------------------------------------------ helpers
def col(r, g, b, a=1.0):
    return cq.Color(r, g, b, a)


BLACK, DARK, GRAY = col(0.08, 0.08, 0.09), col(0.16, 0.16, 0.18), col(0.55, 0.55, 0.58)
SILVER, GOLD, WHITE = col(0.78, 0.78, 0.8), col(0.85, 0.68, 0.25), col(0.93, 0.93, 0.9)
RED, GREEN, BLUE = col(0.75, 0.08, 0.1), col(0.05, 0.4, 0.2), col(0.05, 0.12, 0.35)
TAN, GLASS = col(0.7, 0.6, 0.45), col(0.03, 0.03, 0.05)


def box(cx, cy, z0, dx, dy, dz, rot=0.0):
    b = cq.Workplane("XY").box(dx, dy, dz, centered=(True, True, False))
    if rot:
        b = b.rotate((0, 0, 0), (0, 0, 1), rot)
    return b.translate((cx, cy, z0)).val()


def cyl(cx, cy, z0, r, h):
    return cq.Workplane("XY").circle(r).extrude(h).translate((cx, cy, z0)).val()


def save(asm, name):
    path = OUT / f"{name}.step"
    asm.save(str(path))
    print("wrote", path.relative_to(ROOT), f"({path.stat().st_size // 1024} KB)")


def new(name):
    return cq.Assembly(name=name)


def header_row(asm, x, y_first, n, y_step, z_top, spacer=True, prefix="hdr", mz=None):
    """A male-header spacer (black plastic under a PCB) plus square gold pins, pin 1 at (x, y_first)."""
    mz = MODULE_Z if mz is None else mz
    bottom = max(mz - 6.0, 0.5)            # a 6 mm pin tail, but never through the carrier board
    for i in range(n):
        y = y_first + i * y_step
        if spacer:
            asm.add(box(x, y, mz - SPACER_H, 2.54, 2.54, SPACER_H), color=BLACK, name=f"{prefix}_sp{i}")
        asm.add(box(x, y, bottom, 0.64, 0.64, z_top - bottom), color=GOLD, name=f"{prefix}_pin{i}")


def socket_row(asm, x, y_first, n, y_step, prefix="sock", h=None):
    """A female header (black, h tall, default SOCKET_H) with pin holes on top; pin 1 at (x, y_first)."""
    h = SOCKET_H if h is None else h
    for i in range(n):
        y = y_first + i * y_step
        body = cq.Workplane("XY").box(2.54, 2.54, h, centered=(True, True, False)).faces(">Z").workplane() \
            .rect(1.0, 1.0).cutBlind(-3.0).translate((x, y, 0)).val()
        asm.add(body, color=BLACK, name=f"{prefix}{i}")


# ------------------------------------------------------------------ Thing Plus (long variant)
TP_BRD = DL / "SparkFun_Thing_Plus_RA6M5.brd"
TP_URL = "https://raw.githubusercontent.com/sparkfun/SparkFun_Thing_Plus_RA6M5/main/Hardware/SparkFun_Thing_Plus_RA6M5.brd"
PCB_W, PCB_L, PCB_T = 22.86, 68.58, 1.6


def parse_rot(s):
    mirror = bool(s) and s.startswith("M")
    ang = float(re.sub(r"[^0-9.]", "", s)) if s and re.search(r"\d", s) else 0.0
    return mirror, ang


def package_bboxes(root):
    out = {}
    for pk in root.iter("package"):
        xs, ys = [], []
        for e in pk:
            if e.tag == "smd":
                x, y, dx, dy = (float(e.get(k)) for k in ("x", "y", "dx", "dy"))
                xs += [x - dx / 2, x + dx / 2]
                ys += [y - dy / 2, y + dy / 2]
            elif e.tag == "pad":
                x, y, d = float(e.get("x")), float(e.get("y")), float(e.get("diameter") or 1.6)
                xs += [x - d / 2, x + d / 2]
                ys += [y - d / 2, y + d / 2]
            elif e.tag == "wire" and e.get("layer") in ("51", "21"):
                for a, b in (("x1", "y1"), ("x2", "y2")):
                    xs.append(float(e.get(a)))
                    ys.append(float(e.get(b)))
            elif e.tag == "rectangle" and e.get("layer") in ("51", "21", "39"):
                xs += [float(e.get("x1")), float(e.get("x2"))]
                ys += [float(e.get("y1")), float(e.get("y2"))]
        if xs:
            out[pk.get("name")] = (min(xs), max(xs), min(ys), max(ys))
    return out


# (substring of package name or element name) -> (height above the PCB, colour). First match wins.
HEIGHTS = [
    ("USB-C", 3.2, SILVER), ("LQFP", 1.5, BLACK), ("DA14531MOD", 0, None), ("MICROSD", 1.85, SILVER), ("QWIIC", 3.6, WHITE),
    ("JST04", 3.6, WHITE), ("JST-2", 4.4, WHITE), ("TACTILE", 1.6, DARK), ("CRYSTAL", 0.8, SILVER), ("4-SMD", 0.8, SILVER),
    ("WS2812", 0.8, WHITE), ("SOT8", 1.0, BLACK), ("SOT23", 1.1, BLACK), ("SOT886", 0.5, BLACK), ("DFN", 0.8, BLACK),
    ("WSON", 0.8, BLACK), ("LED", 0.5, col(0.9, 0.85, 0.3)), ("1210", 1.0, TAN), ("1206", 0.9, TAN), ("0805", 0.7, TAN),
    ("0603", 0.5, TAN), ("0402", 0.4, TAN), ("0201", 0.25, TAN),
]
SKIP = ("FRAME", "LOGO", "FD", "FID", "H", "STAND", "SUPPLYTIE", "U$", "SHLD", "I2C", "BAT", "TP", "J2", "J4", "J5")


def thing_plus():
    if not TP_BRD.exists():
        print("downloading", TP_URL)
        urllib.request.urlretrieve(TP_URL, TP_BRD)
    root = ET.parse(TP_BRD).getroot()
    bb = package_bboxes(root)
    ox, oy = PCB_W / 2, PCB_L / 2                       # eagle (0,0) is the PCB's bottom-left; model origin = PCB centre
    zt = TP_MODULE_Z + PCB_T                            # top surface of the Thing Plus PCB
    zb = TP_MODULE_Z                                    # underside
    asm = new("ThingPlus")

    # sockets and male headers: left column = 12 pins (pads 1-12), right = 16 (pads 13-28); 2.54 pitch
    xl, xr = -10.16, 10.16
    y_top = 34.29 - 5.08 - (-17.78) * -1                # placeholder, replaced below
    # pad y (footprint, y down) = -17.78 + 2.54 i  ->  model y = 17.78 - 2.54 i
    for x, n, prefix in ((xl, 12, "L"), (xr, 16, "R")):
        socket_row(asm, x, 17.78, n, -2.54, prefix=f"sock{prefix}", h=TP_SOCKET_H)
        header_row(asm, x, 17.78, n, -2.54, zt + 0.6, prefix=f"hdr{prefix}", mz=TP_MODULE_Z)

    # the PCB: rounded rectangle, 4 mounting holes, plated header holes
    pcb = (cq.Workplane("XY").rect(PCB_W, PCB_L).extrude(PCB_T).edges("|Z").fillet(2.54).translate((0, 0, zb)))
    holes = [(2.54 - ox, 2.54 - oy), (20.32 - ox, 2.54 - oy), (2.54 - ox, 55.88 - oy), (20.32 - ox, 55.88 - oy)]
    pcb = pcb.faces(">Z").workplane(origin=(0, 0, zt)).pushPoints(holes).hole(3.2)
    hole_pts = [(xl, 17.78 - 2.54 * i) for i in range(12)] + [(xr, 17.78 - 2.54 * i) for i in range(16)]
    pcb = pcb.faces(">Z").workplane(origin=(0, 0, zt)).pushPoints(hole_pts).hole(1.0)
    asm.add(pcb.val(), color=RED, name="pcb")

    n_parts = 0
    for e in root.iter("element"):
        name, pkg = e.get("name"), e.get("package") or ""
        if name.startswith(SKIP) or pkg not in bb:
            continue
        val = e.get("value") or ""
        key = f"{pkg} {name}"
        mirror, ang = parse_rot(e.get("rot"))
        x0, x1, y0, y1 = bb[pkg]
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        w, h = x1 - x0, y1 - y0
        if mirror:
            cx = -cx
        r = math.radians(ang)
        px = float(e.get("x")) + cx * math.cos(r) - cy * math.sin(r)
        py = float(e.get("y")) + cx * math.sin(r) + cy * math.cos(r)
        hh, colr = next(((hh, c) for k, hh, c in HEIGHTS if k in key.upper() or k in val.upper()), (0.5, TAN))
        if hh == 0:
            continue
        # resistor/capacitor arrays and pads without a real body are still drawn as one block
        if mirror:
            asm.add(box(px - ox, py - oy, zb - hh, w, h, hh, ang), color=colr, name=name)
        else:
            asm.add(box(px - ox, py - oy, zt, w, h, hh, ang), color=colr, name=name)
        n_parts += 1

    # USB-C shell: 8.94 wide, overhangs the PCB edge by ~1.1 mm (from SparkFun's dimension drawing)
    # BLE module DA14531MOD: 12.7 x 14.5 (x 5.08..17.78, y 58.42..72.9 in eagle coords), overhangs the PCB end
    mx, my = 11.43 - ox, (58.42 + 72.9) / 2 - oy
    asm.add(box(mx, my, zt, 12.7, 14.48, 0.8), color=GREEN, name="ble_substrate")
    asm.add(box(mx, 58.42 - oy + 4.6, zt + 0.8, 12.7, 9.2, 1.3), color=SILVER, name="ble_shield")
    print(f"thing plus: {n_parts} components")
    return asm


# ------------------------------------------------------------------ Kycon STX-3120-3B (manufacturer STEP, re-oriented)
KYCON_STEP = DL / "STX-3120-3B_rB2.STEP"
KYCON_URL = "https://www.kycon.com/website/Extranet/3D/STX-3120-3B_rB2.STEP"


def kycon_jack():
    """Kycon's model stands on its back (barrel toward +z, up = +y, housing bottom at y=-3.1). Our footprint origin is the
    centre of the barrel's front face, +x toward the back, z up from the board."""
    if not KYCON_STEP.exists():
        urllib.request.urlretrieve(KYCON_URL, KYCON_STEP)
    shp = cq.importers.importStep(str(KYCON_STEP)).val()
    # M = (3.5 - Sz, -Sx, Sy + 3.1): rotate +90 about x -> (Sx, -Sz, Sy), then -90 about z -> (-Sz, -Sx, Sy), then translate
    O = cq.Vector(0, 0, 0)
    shp = shp.rotate(O, cq.Vector(1, 0, 0), 90).rotate(O, cq.Vector(0, 0, 1), -90).translate(cq.Vector(3.5, 0, 3.1))
    asm = new("STX-3120-3B")
    asm.add(shp, color=DARK, name="jack")
    return asm


# ------------------------------------------------------------------ RV112FF-40-25A pot (shaft up, pins bent down) and its knob
def pot():
    """Origin = shaft axis; the pin row is toward -y in model coordinates (footprint +y). Datasheet: body 11.0 wide, 6.7 mm
    toward the shaft side of the axis and 8.7 mm toward the pins, 8.5 mm deep; shaft 6.0 dia, 25 mm above the mounting face,
    flatted (4.5 across) over the last 12 mm."""
    asm = new("RV112FF")
    asm.add(box(0, -1.0, 0.0, 11.0, 15.4, 8.5), color=SILVER, name="body")
    for sx in (-1, 1):                                                   # case clips: 2.0 mm wide, 0.5 thick blades on the body's sides
        asm.add(box(sx * 5.75, 0, -3.2, 0.5, 2.0, 3.2 + 1.0), color=SILVER, name=f"clip{sx}")
    asm.add(cyl(0, 0, 8.5, 3.4, 0.6), color=SILVER, name="boss")
    shaft = cq.Workplane("XY").circle(3.0).extrude(25.0 - 8.5).translate((0, 0, 8.5))
    cut = cq.Workplane("XY").box(8, 3, 12.0, centered=(True, False, False)).translate((0, 1.5, 13.0))   # flat toward +y, last 12 mm
    asm.add(shaft.cut(cut).val(), color=BLACK, name="shaft")
    for i, x in enumerate((-3.75, -1.25, 1.25, 3.75)):                   # pins leave the pin face, then bend down through the board
        asm.add(box(x, -9.35, 1.25, 0.5, 1.7, 0.5), color=GOLD, name=f"pinH{i}")
        asm.add(box(x, -10.0, -3.0, 0.5, 0.5, 4.75), color=GOLD, name=f"pinV{i}")
    return asm


def knob():
    """Same Sky SPK-013A: 24.1 dia x 10.27 high, black TPE over a white ABS core, D-cut 6.0 x 4.5 bore 7.2 deep, white
    indicator line. Sits on the shaft: bore bottom (=shaft tip) at z=25 => knob underside at 25 - 7.2 = 17.8."""
    z0 = 25.0 - 7.2
    body = cq.Workplane("XY").circle(12.05).extrude(10.27).faces(">Z").edges().fillet(1.6)
    body = body.cut(cq.Workplane("XY").circle(10.4).extrude(2.6))                     # shallow skirt cavity underneath
    d_bore = cq.Workplane("XY").circle(3.0).extrude(7.2).cut(cq.Workplane("XY").box(8, 3, 7.2, centered=(True, False, False)).translate((0, 1.5, 0)))
    body = body.cut(d_bore).translate((0, 0, z0))
    asm = new("SPK-013A")
    asm.add(body.val(), color=BLACK, name="knob")
    asm.add(box(0, 8.3, z0 + 10.27 - 0.15, 0.9, 5.2, 0.16), color=WHITE, name="indicator")   # toward +y (top of board)
    return asm


# ------------------------------------------------------------------ pluggable modules on female headers
def oled():
    """0.91 in 128x32 SSD1306 module (38 x 12 mm), 4 pins along one short edge, plugged into a 1x4 socket. Origin = pin 1."""
    asm = new("OLED")
    socket_row(asm, 0, 0, 4, -2.54, h=TP_SOCKET_H)
    header_row(asm, 0, 0, 4, -2.54, TP_MODULE_Z + 1.0 + 0.6, mz=TP_MODULE_Z)
    pcb = cq.Workplane("XY").box(38.0, 12.0, 1.0, centered=(True, True, False)).translate((-36.5 + 19.0, -3.81 + 0.0, TP_MODULE_Z))
    asm.add(pcb.val(), color=BLUE, name="pcb")
    asm.add(box(-18.25, -3.81, TP_MODULE_Z + 1.0, 29.5, 11.5, 1.2), color=GLASS, name="glass")
    asm.add(box(-18.25, -3.81, TP_MODULE_Z + 2.2, 22.4, 5.6, 0.05), color=col(0.0, 0.0, 0.0), name="active")
    return asm


def mic():
    """Adafruit I2S MEMS mic breakout (SPH0645LM4H), 16.51 x 12.7 mm, 6-pin header along one long edge; the mic package is
    on the same side as the header so its sound hole faces up when the module is plugged in. Origin = socket pin 1 (VDD);
    pins run toward -y; the module extends toward +x. Dimensions from Adafruit's Eagle board file."""
    asm = new("I2S_MIC")
    socket_row(asm, 0, 0, 6, -2.54, h=TP_SOCKET_H)
    header_row(asm, 0, 0, 6, -2.54, TP_MODULE_Z + 1.6 + 0.6, mz=TP_MODULE_Z)
    # Eagle board coordinates (x 1.016..17.526, y 0..12.7); pin 1 (VDD) at eagle x=15.621, y=2.54.
    ex = lambda xe: xe - 15.621            # eagle x -> model y (pins step toward -y)
    ey = lambda ye: ye - 2.54              # eagle y -> model x
    pcb = cq.Workplane("XY").box(12.7, 16.51, 1.6, centered=(True, True, False)).translate((ey(6.35), ex(9.271), TP_MODULE_Z))
    pcb = pcb.faces(">Z").workplane(origin=(0, 0, TP_MODULE_Z + 1.6)).pushPoints([(ey(10.16), ex(3.556)), (ey(10.16), ex(14.986))]).hole(2.5)
    pcb = pcb.faces(">Z").workplane(origin=(0, 0, TP_MODULE_Z + 1.6)).pushPoints([(ey(7.112), ex(9.271))]).hole(0.9)   # sound port
    asm.add(pcb.val(), color=col(0.04, 0.04, 0.06), name="pcb")
    asm.add(box(ey(7.112), ex(9.271), TP_MODULE_Z - 1.0, 3.5, 2.65, 1.0), color=SILVER, name="mic")
    return asm


# ------------------------------------------------------------------ main
MODELS = {"ThingPlus_RA6M5_long": thing_plus, "Kycon_STX-3120-3B": kycon_jack, "RV112FF-40-25A": pot,
          "SPK-013A_knob": knob, "SSD1306_OLED_0.91in_socketed": oled, "I2S_Mic_SPH0645_socketed": mic}

if __name__ == "__main__":
    only = sys.argv[sys.argv.index("--only") + 1:] if "--only" in sys.argv else list(MODELS)
    for n in only:
        save(MODELS[n](), n)
