#!/usr/bin/env python3
"""
Generate the KiCad 7 schematic (../cassette-carrier.kicad_sch) and the project symbol library (../cassette.kicad_sym).

Like gen_board.py this is a bootstrap: it writes the files from scratch. Once you start editing the schematic in KiCad,
edit it there instead of regenerating.

Stock symbols (R, C, C_Polarized, SolderJumper, GND, MountingHole, Conn_01x01) are copied out of KiCad 7's own libraries;
the Thing Plus, the RV112FF pot and the Kycon jack are drawn here (pin numbers match the footprints in cassette.pretty).
Wires are drawn where a signal path reads better that way; everything else is joined by net labels.
"""
import math, pathlib, re, sys, uuid

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from gen_board import TP_LEFT, TP_RIGHT, OLED_NAME, JR_NAME                    # Thing Plus pad names, single source of truth

OUT = pathlib.Path(__file__).resolve().parent.parent
KICAD_SYM = pathlib.Path("/Applications/KiCad 7/KiCad.app/Contents/SharedSupport/symbols")
PROJECT = "cassette-carrier"

_n = 0


def uid():
    global _n
    _n += 1
    return str(uuid.uuid5(uuid.NAMESPACE_URL, f"cassette-carrier-sch/{_n}"))


ROOT = str(uuid.uuid5(uuid.NAMESPACE_URL, "cassette-carrier-sch/root"))


def f(x):
    return f"{x:.4f}".rstrip("0").rstrip(".")


# ------------------------------------------------------------------ symbol library handling
def balanced(text, start):
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    raise ValueError("unbalanced")


def stock_symbol(lib, name):
    """Copy one symbol out of KiCad's library; renamed to lib:name for embedding in a schematic."""
    text = (KICAD_SYM / f"{lib}.kicad_sym").read_text()
    m = re.search(r'\(symbol "%s"\s' % re.escape(name), text)
    body = balanced(text, m.start())
    return body.replace(f'(symbol "{name}"', f'(symbol "{lib}:{name}"', 1)


def parse_pins(sym_text):
    """{pin number: (x, y, angle)} in library coordinates (y up)."""
    pins = {}
    for m in re.finditer(r'\(pin\s+\w+\s+\w+\s*\(at\s+([-\d.]+)\s+([-\d.]+)\s+(\d+)\)\s*\(length\s+[-\d.]+\).*?\(number\s+"([^"]+)"', sym_text, re.S):
        pins[m.group(4)] = (float(m.group(1)), float(m.group(2)), int(m.group(3)))
    return pins


def eff(size=1.27, extra=""):
    return f"(effects (font (size {size} {size})){extra})"


def custom_symbol(name, ref, footprint, descr, rects, pins, pin_len=2.54):
    """A KiCad 7 symbol as text. pins: list of (number, name, x, y, angle)."""
    pin_txt = "".join(
        f'      (pin passive line (at {f(x)} {f(y)} {a}) (length {pin_len})\n'
        f'        (name "{pn}" {eff()})\n        (number "{num}" {eff()}))\n' for num, pn, x, y, a in pins)
    rect_txt = "".join(
        f'      (rectangle (start {f(x1)} {f(y1)}) (end {f(x2)} {f(y2)}) (stroke (width 0.254) (type default)) (fill (type background)))\n'
        for x1, y1, x2, y2 in rects)
    top = max(r[1] for r in rects)
    return (f'  (symbol "{name}" (pin_names (offset 1.016)) (in_bom yes) (on_board yes)\n'
            f'    (property "Reference" "{ref}" (at 0 {f(top + 2.54)} 0) {eff()})\n'
            f'    (property "Value" "{name}" (at 0 {f(-top - 2.54)} 0) {eff()})\n'
            f'    (property "Footprint" "{footprint}" (at 0 0 0) {eff(1.27, " hide")})\n'
            f'    (property "Datasheet" "" (at 0 0 0) {eff(1.27, " hide")})\n'
            f'    (property "ki_description" "{descr}" (at 0 0 0) {eff(1.27, " hide")})\n'
            f'    (symbol "{name}_0_1"\n{rect_txt}    )\n'
            f'    (symbol "{name}_1_1"\n{pin_txt}    )\n  )\n')


# Thing Plus: left header column (pads 1-12) on the left of the symbol, right column (13-28) on the right.
TP_PINS = []
for i, n in enumerate(TP_LEFT):
    TP_PINS.append((str(i + 1), n, -15.24, 19.05 - 2.54 * i, 0))
for i, n in enumerate(TP_RIGHT):
    TP_PINS.append((str(13 + i), n, 15.24, 19.05 - 2.54 * i, 180))
SYM_TP = custom_symbol("ThingPlus", "U", "cassette:ThingPlus", "SparkFun Thing Plus RA6M5 (long variant)",
                       [(-12.7, 21.59, 12.7, -21.59)], TP_PINS)

# RV112FF-40: wipers on the right, track ends on the left, the two case clips underneath.
RV_PINS = [("2", "PWR", -12.7, 2.54, 0), ("4", "GND", -12.7, -2.54, 0),
           ("3", "WIPER_A", 12.7, 2.54, 180), ("1", "WIPER_B", 12.7, -2.54, 180),
           ("5", "CASE", -2.54, -10.16, 90), ("6", "CASE", 2.54, -10.16, 90)]
SYM_RV = custom_symbol("RV112FF-40", "RV", "cassette:RV112FF-40", "Taiwan Alpha RV112FF-40 dual 360-degree potentiometer",
                       [(-10.16, 7.62, 10.16, -7.62)], RV_PINS)

# Kycon STX-3120-3B: pin numbers are the jack's own (1 sleeve, 2 = ring?, 5 = tip?; tip/ring unverified)
J_PINS = [("5", "TIP", -10.16, 2.54, 0), ("2", "RING", -10.16, 0, 0), ("1", "SLEEVE", -10.16, -2.54, 0)]
SYM_J = custom_symbol("STX-3120-3B", "J", "cassette:Kycon_STX-3120-3B", "Kycon STX-3120-3B 3.5 mm stereo jack",
                      [(-7.62, 5.08, 7.62, -5.08)], J_PINS)

# Not on the board in rev 1 (kept for the next rev; see branch feature/pcb-mic-oled).
# Adafruit I2S MEMS microphone breakout (SPH0645LM4H): 1x6 header, pin order as listed in Adafruit's pinout page
# (3V, GND, BCLK, DOUT, LRCLK, SEL) - UNVERIFIED against the real module; check its silkscreen when it arrives.
MIC_PINS = [("1", "3V", -2.54, 11.43, 270), ("2", "GND", -2.54, -11.43, 90), ("6", "SEL", 2.54, -11.43, 90),
            ("3", "BCLK", -10.16, 2.54, 0), ("4", "DOUT", -10.16, 0, 0), ("5", "LRCLK", -10.16, -2.54, 0)]
SYM_MIC = custom_symbol("I2S_Mic_Breakout", "MIC", "Connector_PinSocket_2.54mm:PinSocket_1x06_P2.54mm_Vertical",
                        "Adafruit I2S MEMS microphone breakout (SPH0645LM4H)", [(-7.62, 8.89, 7.62, -8.89)], MIC_PINS)

# Optional SSD1306 OLED module, 4-pin I2C, pin order GND, VCC, SCL, SDA at 0.1 in (the common layout).
OLED_PINS = [("2", "VCC", -2.54, 8.89, 270), ("1", "GND", -2.54, -8.89, 90),
             ("3", "SCL", -10.16, 1.27, 0), ("4", "SDA", -10.16, -1.27, 0)]
SYM_OLED = custom_symbol("SSD1306_I2C_OLED", "DS", f"cassette:{OLED_NAME}",
                         "SSD1306 OLED module, 4-pin I2C (GND, VCC, SCL, SDA)", [(-7.62, 6.35, 7.62, -6.35)], OLED_PINS)

CUSTOM = {"cassette:ThingPlus": SYM_TP, "cassette:RV112FF-40": SYM_RV, "cassette:STX-3120-3B": SYM_J,
          "cassette:I2S_Mic_Breakout": SYM_MIC, "cassette:SSD1306_I2C_OLED": SYM_OLED}
LIB = {
    "Device:R": stock_symbol("Device", "R"),
    "Device:C": stock_symbol("Device", "C"),
    "Device:C_Polarized": stock_symbol("Device", "C_Polarized"),
    "Jumper:SolderJumper_2_Bridged": stock_symbol("Jumper", "SolderJumper_2_Bridged"),
    "power:GND": stock_symbol("power", "GND"),
    "power:+3V3": stock_symbol("power", "+3V3"),
    "Switch:SW_SPDT": stock_symbol("Switch", "SW_SPDT"),
    "Mechanical:MountingHole": stock_symbol("Mechanical", "MountingHole"),
    "Connector_Generic:Conn_01x01": stock_symbol("Connector_Generic", "Conn_01x01"),
}
for k, v in CUSTOM.items():                                  # embed with the library prefix
    lib, nm = k.split(":")
    LIB[k] = v.replace(f'(symbol "{nm}"', f'(symbol "{k}"', 1)
PINS = {k: parse_pins(v) for k, v in LIB.items()}


# ------------------------------------------------------------------ schematic building blocks
items = []                                                   # s-expression fragments
refs = {}                                                    # reference -> {pin number: (x, y)}


def rot_pt(x, y, rot):
    r = math.radians(rot)
    return x * math.cos(r) - y * math.sin(r), x * math.sin(r) + y * math.cos(r)


def sch_pt(at, rot, lib_xy):
    rx, ry = rot_pt(lib_xy[0], lib_xy[1], rot)
    return round(at[0] + rx, 4), round(at[1] - ry, 4)


def out_dir(rot, pin_angle):
    """Direction (schematic coords) pointing away from the symbol at this pin."""
    dx, dy = rot_pt(-math.cos(math.radians(pin_angle)), -math.sin(math.radians(pin_angle)), rot)
    return round(dx), round(-dy)


def yn(flag):
    return "yes" if flag else "no"


def symbol(lib_id, ref, value, footprint, at, rot=0, ref_at=None, val_at=None, hide_ref=False, in_bom=True, on_board=True, props=None):
    pins = {n: sch_pt(at, rot, (x, y)) for n, (x, y, a) in PINS[lib_id].items()}
    refs[ref] = pins
    horiz = rot in (90, 270)
    if ref_at is None:
        ref_at = (at[0], at[1] - 2.54) if horiz else (at[0] + 2.54, at[1] - 1.27)
    if val_at is None:
        val_at = (at[0], at[1] + 2.54) if horiz else (at[0] + 2.54, at[1] + 1.27)
    just = "" if horiz else (" (justify right)" if rot == 180 else " (justify left)")   # 180 mirrors the justification
    pin_txt = "".join(f'    (pin "{n}" (uuid {uid()}))\n' for n in PINS[lib_id])
    items.append(
        f'  (symbol (lib_id "{lib_id}") (at {f(at[0])} {f(at[1])} {rot}) (unit 1)\n'
        f'    (in_bom {yn(in_bom)}) (on_board {yn(on_board)}) (dnp no) (fields_autoplaced)\n    (uuid {uid()})\n'
        f'    (property "Reference" "{ref}" (at {f(ref_at[0])} {f(ref_at[1])} 0) {eff(1.27, just + (" hide" if hide_ref else ""))})\n'
        f'    (property "Value" "{value}" (at {f(val_at[0])} {f(val_at[1])} 0) {eff(1.27, just)})\n'
        f'    (property "Footprint" "{footprint}" (at {f(at[0])} {f(at[1])} 0) {eff(1.27, " hide")})\n'
        f'    (property "Datasheet" "~" (at {f(at[0])} {f(at[1])} 0) {eff(1.27, " hide")})\n'
        + "".join(f'    (property "{k}" "{v}" (at {f(at[0])} {f(at[1])} 0) {eff(1.27, " hide")})\n' for k, v in (props or {}).items()) +
        f'{pin_txt}'
        f'    (instances (project "{PROJECT}" (path "/{ROOT}" (reference "{ref}") (unit 1))))\n  )\n')
    return pins


def pin_dir(lib_id, rot, num):
    return out_dir(rot, PINS[lib_id][num][2])


def wire(*pts):
    for a, b in zip(pts, pts[1:]):
        items.append(f'  (wire (pts (xy {f(a[0])} {f(a[1])}) (xy {f(b[0])} {f(b[1])}))\n'
                     f'    (stroke (width 0) (type default)) (uuid {uid()}))\n')


def junction(p):
    items.append(f'  (junction (at {f(p[0])} {f(p[1])}) (diameter 0) (color 0 0 0 0) (uuid {uid()}))\n')


def label(name, p, angle=0):
    just = {0: "left bottom", 90: "left bottom", 180: "right bottom", 270: "right bottom"}[angle]
    items.append(f'  (label "{name}" (at {f(p[0])} {f(p[1])} {angle}) (fields_autoplaced)\n'
                 f'    (effects (font (size 1.27 1.27)) (justify {just})) (uuid {uid()}))\n')


def no_connect(p):
    items.append(f'  (no_connect (at {f(p[0])} {f(p[1])}) (uuid {uid()}))\n')


_gnd = 0


def gnd(p, direction=(0, 1)):
    """Ground symbol whose pin sits exactly at p, body extending in `direction` (schematic coords)."""
    global _gnd
    _gnd += 1
    rot = {(0, 1): 0, (1, 0): 90, (0, -1): 180, (-1, 0): 270}[direction]
    val_at = {(0, 1): (p[0] + 1.5, p[1] + 3), (0, -1): (p[0] + 1.5, p[1] - 3),
              (1, 0): (p[0] + 4, p[1] + 1.5), (-1, 0): (p[0] - 4, p[1] + 1.5)}[direction]
    pins = symbol("power:GND", f"#PWR0{_gnd:02d}", "GND", "", p, rot, ref_at=(p[0] + 1.5, p[1] - 3), val_at=val_at, hide_ref=True)
    return pins


def gnd_from_pin(p, outward, direction=(0, 1), n=None):
    """Short wire out of a pin, then a ground symbol at the wire's end."""
    n = n or (5.08 if outward[0] else 2.54)
    q = (round(p[0] + outward[0] * n, 4), round(p[1] + outward[1] * n, 4))
    wire(p, q)
    gnd(q, direction)


def p3v3(p, outward=None, n=5.08):
    """+3V3 power symbol at p (body up). With `outward`, a short wire out of the pin first."""
    global _gnd
    q = p
    if outward:
        q = (round(p[0] + outward[0] * n, 4), round(p[1] + outward[1] * n, 4))
        wire(p, q)
    _gnd += 1
    symbol("power:+3V3", f"#PWR0{_gnd:02d}", "+3V3", "", q, 0, ref_at=(q[0] + 1.5, q[1] + 3), val_at=(q[0] + 1.5, q[1] - 3.5), hide_ref=True)


def stub_label(lib_id, rot, pins, num, name, length=5.08):
    """Wire from a pin out to a net label."""
    p = pins[num]
    d = pin_dir(lib_id, rot, num)
    q = (round(p[0] + d[0] * length, 4), round(p[1] + d[1] * length, 4))
    wire(p, q)
    label(name, q, {(1, 0): 0, (-1, 0): 180, (0, -1): 90, (0, 1): 270}[d])


def box(x1, y1, x2, y2, title):
    items.append(f'  (rectangle (start {f(x1)} {f(y1)}) (end {f(x2)} {f(y2)})\n'
                 f'    (stroke (width 0.254) (type dash)) (fill (type none)) (uuid {uid()}))\n')
    text(title, (x1 + 2.54, y1 + 3.81), 2.0, True)


def text(s, p, size=1.27, bold=False):
    items.append(f'  (text "{s}" (at {f(p[0])} {f(p[1])} 0)\n'
                 f'    (effects (font (size {size} {size}){" bold" if bold else ""}) (justify left top)) (uuid {uid()}))\n')


R_FP = "Resistor_THT:R_Axial_DIN0204_L3.6mm_D1.6mm_P7.62mm_Horizontal"
C_FP = "Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm"
CP_FP = "Capacitor_THT:CP_Radial_D5.0mm_P2.00mm"
JP_FP = "Jumper:SolderJumper-2_P1.3mm_Bridged_RoundedPad1.0x1.5mm"

# ------------------------------------------------------------------ Thing Plus
U1 = (63.5, 166.37)
box(20.32, 122.0, 110.0, 210.0, "Thing Plus RA6M5: used pins wired")
u = symbol("cassette:ThingPlus", "U1", "Thing Plus RA6M5", "cassette:ThingPlus", U1, 0,
           ref_at=(U1[0], U1[1] - 26.0), val_at=(U1[0], U1[1] + 26.0))
USED_RIGHT = {"TX": "D20", "RX": "D21", "A5": "POT2_B", "A4": "POT2_A", "A3": "POT1_B", "A2": "POT1_A",
              "A1": "DAC_R", "A0": "DAC_L", "VREF": "VREF"}
USED_LEFT = {"D6": "D6", "EN": "EN"}
names = {str(i + 1): n for i, n in enumerate(TP_LEFT)}
names.update({str(13 + i): n for i, n in enumerate(TP_RIGHT)})
for num, nm in names.items():
    p = u[num]
    d = pin_dir("cassette:ThingPlus", 0, num)
    net = (USED_LEFT if int(num) <= 12 else USED_RIGHT).get(nm)
    if nm == "GND":
        gnd_from_pin(p, d, (1, 0), 15.24)             # sideways and far enough out to clear the VREF label
    elif net:
        stub_label("cassette:ThingPlus", 0, u, num, net)
    else:
        no_connect(p)

# ------------------------------------------------------------------ Audio output
box(118.0, 20.0, 262.0, 118.0, "Audio out: one-pole RC low-pass, then the DC blocker")
for ch, y, net_in, net_out, ci, cb, ri in (("L", 45.72, "DAC_L", "AUDIO_L", 1, 3, 1), ("R", 78.74, "DAC_R", "AUDIO_R", 2, 4, 2)):
    x_r, x_node, x_b = 139.7, 154.94, 170.18
    label(net_in, (127.0, y), 180)
    r = symbol("Device:R", f"R{ri}", "1k", R_FP, (x_r, y), 90)
    wire((127.0, y), r["1"])
    node = (x_node, y)
    wire(r["2"], node)
    junction(node)
    cf = symbol("Device:C", f"C{ci}", "6.8n", C_FP, (x_node, y + 10.16), 0, ref_at=(x_node + 2.54, y + 8.89), val_at=(x_node + 2.54, y + 11.43))
    wire(node, cf["1"])
    gnd(cf["2"])
    cd = symbol("Device:C_Polarized", f"C{cb}", "2.2u", "cassette:C_DCBlock_Dual", (x_b, y), 90, ref_at=(x_b, y - 5.5), val_at=(x_b, y + 5.5))
    wire(node, cd["1"])
    wire(cd["2"], (181.61, y))
    label(net_out, (181.61, y), 0)
text("R1/R2 + C1/C2: one-pole low-pass, fc = 1/(2 pi R C) = 23 kHz. R sits at the DAC pin so the DAC's output",
     (122.0, 101.0))
text("amplifier never sees the capacitor directly (datasheet: 5 kohm min load, 50 pF max). C1/C2 should be C0G or film.",
     (122.0, 104.0))
text("C3/C4 (2.2 uF, + toward the DAC) block the DC bias; electrolytic or film fits the same footprint.",
     (122.0, 107.0))
J1 = (236.22, 62.23)
j = symbol("cassette:STX-3120-3B", "J1", "STX-3120-3B", "cassette:Kycon_STX-3120-3B", J1, 0,
           ref_at=(J1[0] - 7.62, J1[1] - 10.0), val_at=(J1[0] - 7.62, J1[1] + 10.0))
stub_label("cassette:STX-3120-3B", 0, j, "5", "AUDIO_L")
stub_label("cassette:STX-3120-3B", 0, j, "2", "AUDIO_R")
gnd_from_pin(j["1"], pin_dir("cassette:STX-3120-3B", 0, "1"))

# ------------------------------------------------------------------ Pots
box(118.0, 126.0, 262.0, 240.0, "Pots: RV112FF-40, wipers filtered by 100 nF to ground")
for k, (cy, na, nb, ca, cb) in enumerate(((162.56, "POT1_A", "POT1_B", 5, 6), (208.28, "POT2_A", "POT2_B", 7, 8)), 1):
    at = (149.86, round(cy / 1.27) * 1.27)
    rv = symbol("cassette:RV112FF-40", f"RV{k}", "RV112FF-40-25A-0B10K", "cassette:RV112FF-40", at, 0,
                ref_at=(at[0] - 12.7, at[1] - 11.5), val_at=(at[0] - 12.7, at[1] + 11.5))
    stub_label("cassette:RV112FF-40", 0, rv, "2", "POT_V")
    gnd_from_pin(rv["4"], pin_dir("cassette:RV112FF-40", 0, "4"))
    (x5, y5), (x6, y6) = rv["5"], rv["6"]                    # both case clips to one ground
    a, b, m = (x5, y5 + 5.08), (x6, y6 + 5.08), ((x5 + x6) / 2, y5 + 5.08)
    wire((x5, y5), a, m)
    wire((x6, y6), b, m)
    junction(m)
    gnd(m)
    xj = rv["3"][0] + 7.62
    # wiper A: cap hangs up (ground above), wiper B: cap hangs down
    ja, jb = (xj, rv["3"][1]), (xj, rv["1"][1])
    wire(rv["3"], ja, (xj + 7.62, ja[1]))
    wire(rv["1"], jb, (xj + 7.62, jb[1]))
    junction(ja)
    junction(jb)
    label(na, (xj + 7.62, ja[1]), 0)
    label(nb, (xj + 7.62, jb[1]), 0)
    capa = symbol("Device:C", f"C{ca}", "100n", C_FP, (xj, ja[1] - 10.16), 180, ref_at=(xj + 2.54, ja[1] - 11.43), val_at=(xj + 2.54, ja[1] - 8.89))
    wire(ja, capa["1"])
    gnd(capa["2"], (0, -1))
    capb = symbol("Device:C", f"C{cb}", "100n", C_FP, (xj, jb[1] + 10.16), 0, ref_at=(xj + 2.54, jb[1] + 8.89), val_at=(xj + 2.54, jb[1] + 11.43))
    wire(jb, capb["1"])
    gnd(capb["2"])

# ------------------------------------------------------------------ Pot supply
box(274.0, 126.0, 400.0, 186.0, "Pot supply: VREF through an optional filter resistor")
xr, y = 304.8, 149.86
label("VREF", (289.56, y), 180)
r3 = symbol("Device:R", "R3", "22", f"cassette:{JR_NAME}", (xr, y), 90)
wire((289.56, y), r3["1"])
jp = symbol("Jumper:SolderJumper_2_Bridged", "JP1", "Bridged", JP_FP, (xr, y + 12.7), 0, ref_at=(xr, y + 17.78), val_at=(xr, y + 20.32),
            in_bom=False, on_board=False)          # the jumper is part of R3's footprint; JP1 only documents it here
p1, p2 = jp["1"], jp["2"]
wire(r3["1"], (r3["1"][0], p1[1]), p1)
wire(r3["2"], (r3["2"][0], p2[1]), p2)
junction(r3["1"])
junction(r3["2"])
xn = 322.58
wire(r3["2"], (xn, y), (xn + 7.62, y))
junction((xn, y))
label("POT_V", (xn + 7.62, y), 0)
c9 = symbol("Device:C_Polarized", "C9", "10u", CP_FP, (xn, y + 10.16), 0, ref_at=(xn + 2.54, y + 8.89), val_at=(xn + 2.54, y + 11.43))
wire((xn, y), c9["1"])
gnd(c9["2"])
text("R3 is optional: its footprint has the jumper (JP1) built in, closed by default. Cut the bridge, then fit a", (276.0, 178.0))
text("resistor (22 ohm suggested) to filter the pot supply. JP1 is not a separate part. C9 stabilises the supply.", (276.0, 181.0))

# ------------------------------------------------------------------ Touch
box(274.0, 192.0, 400.0, 246.0, "Touch pads (CTSU): 560 ohm damping resistors, shared TSCAP capacitor")
for k, (y, net, ri) in enumerate(((203.2, "D20", 4), (213.36, "D21", 5)), 1):
    label(net, (289.56, y), 180)
    rr = symbol("Device:R", f"R{ri}", "560", R_FP, (304.8, y), 90)
    wire((289.56, y), rr["1"])
    tc = symbol("Connector_Generic:Conn_01x01", f"TCH{k}", f"Touch{k}", "cassette:TouchPad_40x20", (330.2, y), 0,
                ref_at=(330.2 - 2.54, y - 3.81), val_at=(330.2 - 2.54, y + 3.81))
    wire(rr["2"], tc["1"])
label("D6", (289.56, 231.14), 180)
wire((289.56, 231.14), (304.8, 231.14))
c10 = symbol("Device:C", "C10", "10n", C_FP, (304.8, 236.22), 0, ref_at=(307.34, 234.95), val_at=(307.34, 237.49))
wire((304.8, 231.14), c10["1"])
gnd(c10["2"])
text("TSCAP", (309.0, 229.0))

# ------------------------------------------------------------------ Power switch
box(20.32, 14.0, 110.0, 48.0, "Power switch: EN to GND")
SW = (66.04, 29.21)
sw = symbol("Switch:SW_SPDT", "SW1", "EG1224", "Button_Switch_THT:SW_E-Switch_EG1224_SPDT_Angled", SW, 0,
            ref_at=(SW[0] - 3.0, SW[1] - 7.0), val_at=(SW[0] - 3.0, SW[1] + 7.0))
stub_label("Switch:SW_SPDT", 0, sw, "2", "EN")
gnd_from_pin(sw["1"], pin_dir("Switch:SW_SPDT", 0, "1"), (0, 1), None)
no_connect(sw["3"])
text("Closed to GND, the switch holds the Thing Plus's EN pin low, which turns its", (22.0, 39.0), 1.0)
text("3.3 V regulator off. Use it as the power switch when running on battery.", (22.0, 42.0), 1.0)

# ------------------------------------------------------------------ Sockets (BOM only)
box(20.32, 256.5, 110.0, 283.5, "Sockets: BOM lines only (pads are in the footprints)")
SOCKETS = (("XS1", "1x12 socket", "U1 left column", "HCTL", "PM254-1-12-Z-3.0-C", "C5156451"),
           ("XS2", "1x16 socket", "U1 right column", "HCTL", "PM254-1-16-Z-3.0-C", "C5159943"))
for k, (ref, val, use, mfr, mpn, lcsc) in enumerate(SOCKETS):
    x = 30.48 + 20.32 * k
    symbol("Mechanical:MountingHole", ref, val, "", (x, 269.5), 0, ref_at=(x - 3.0, 265.5), val_at=(x - 6.0, 274.0), on_board=False,
           props={"Manufacturer": mfr, "MPN": mpn, "LCSC": lcsc, "Used for": use, "Description": "2.54 mm pitch, 3 mm insulation height, round-hole turned pin"})
text("Low-profile (3 mm) sockets.", (22.0, 277.5), 1.0)
text("Standoff between the carrier and the Thing Plus: about 5.5 mm (3 mm socket + 2.5 mm header body).", (22.0, 280.5), 1.0)

# ------------------------------------------------------------------ Mechanical
box(20.32, 216.0, 110.0, 254.0, "Board-only parts")
for k in range(4):
    symbol("Mechanical:MountingHole", f"TH{k + 1}", "ToolingHole", "cassette:ToolingHole_1.152mm", (30.48 + 20.32 * k, 232.41), 0,
           ref_at=(27.0 + 20.32 * k, 226.5), val_at=(21.0 + 20.32 * k, 237.5))
text("TH1-TH4: 1.152 mm NPTH tooling holes on the corner-arc centres", (22.0, 247.0))


def build():
    lib_syms = "".join("    " + LIB[k].replace("\n", "\n    ").rstrip() + "\n" for k in LIB)
    return (f'(kicad_sch (version 20230121) (generator eeschema)\n\n  (uuid {ROOT})\n\n  (paper "A3")\n\n'
            f'  (title_block\n    (title "Cassette carrier")\n'
            f'    (rev "A")\n    (comment 1 "Thing Plus RA6M5, knobs, touch and audio out")\n  )\n\n'
            f'  (lib_symbols\n{lib_syms}  )\n\n' + "".join(items) +
            f'\n  (sheet_instances\n    (path "/" (page "1"))\n  )\n)\n')


if __name__ == "__main__":
    (OUT / "cassette-carrier.kicad_sch").write_text(build())
    lib = "".join(v for v in CUSTOM.values())
    (OUT / "cassette.kicad_sym").write_text(f'(kicad_symbol_lib (version 20220914) (generator kicad_symbol_editor)\n{lib})\n')
    (OUT / "sym-lib-table").write_text(
        '(sym_lib_table\n  (version 7)\n'
        '  (lib (name "cassette")(type "KiCad")(uri "${KIPRJMOD}/cassette.kicad_sym")(options "")(descr "Project symbols"))\n)\n')
    print("wrote schematic:", len(items), "items")
