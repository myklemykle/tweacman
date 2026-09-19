#!/usr/bin/env python3
"""
Generate the starting KiCad 7 board for the Compact-Cassette-shaped carrier PCB:
outline, hub cutouts, tooling holes, touch-pad footprints, hub silkscreen, and the
trapezoid (copper-free, mask-free window on the tape-access edge), Thing Plus and fifth-screw marker.

WARNING: this writes ../cassette-carrier.kicad_pcb from scratch. Run it once to
bootstrap; after you start placing parts in KiCad, edit the board there instead
of regenerating (or you will lose your work).

Design coordinates are millimetres, u to the right from the body's left edge and
v UP from the bottom edge; the KiCad sheet uses y-down, converted in xy() below.
The cassette proportions were traced from a 1:1 model of a real Compact Cassette
(body 100.0 x 64.0 mm, corner radius ~2.97 mm, 0.814 mm side bumps), then scaled
so the spool hubs (42 mm apart on a real cassette) land 60 mm apart.
"""
import math, uuid, pathlib

OUT = pathlib.Path(__file__).resolve().parent.parent

# ---- scale and cassette geometry (unscaled numbers are from the traced model) ----
S = 60.0 / 42.0                       # hub spacing 42 mm -> 60 mm
W, H = 100.0 * S, 64.0 * S            # body 142.857 x 91.429
R = 2.97 * S                          # all four body corners (4.243)
BUMP = 0.814 * S                      # side bump depth
BUMP_V = (5.942 * S, 7.103 * S, 21.955 * S, 23.117 * S)  # ramp/flat/ramp heights from bottom
HUB_SPACING = 60.0
HUB_D = (W - HUB_SPACING) / 2         # hub centre is this far from top AND sides
HUB_V = H - HUB_D                     # -> 50.000 above the bottom edge
HUB_X = (HUB_D, HUB_D + HUB_SPACING)
HUB_HOLE_D = 8.0
HUB_SILK = (6.7 * S, 7.2 * S)         # model hub bore / rim radii, scaled
M3_D = 3.2
PAD_W, PAD_H, PAD_R = 40.0, 20.0, 5.0   # touch pads: 40 x 20 mm, both bottom corners rounded r=5, top edge flush with the board edge
PAD_INSET = 15.0                      # distance from the side edges to the pad
ANT_M = R                             # plated M3 antenna holes sit this far in from the pad's top corners
TOOL_D, TOOL_MASK = 1.152, 0.148      # NPTH tooling holes: drill dia, solder-mask expansion
# tooling holes sit on the centres of the four board-corner arcs (R from each edge)
# hub sprocket outline (from the patent drawing): six inward teeth on the hub bore
TOOTH_ANGLES = (90, 30, -30, -90, -150, 150)
TOOTH_LEN, TOOTH_W, TOOTH_GAP = 1.73 * S, 1.5 * S, 0.3
# fifth screw: bottom centre, ~13.0 mm above the bottom edge on the (unscaled) patent drawing
SCREW5 = (W / 2, 13.0 * S)
# Thing Plus: rows 20.32 apart, USB facing left
TP_PAD_R = 1.8796 / 2
TP_ROW_V = 1.0 + TP_PAD_R              # lower row's copper 1 mm above the board edge
TP_CENTRE = (W / 2, TP_ROW_V + 20.32 / 2)
TP_TOP_V = TP_CENTRE[1] + 11.43       # top edge of the Thing Plus (its 22.86 mm width, rotated)

# Trapezoid on the tape-access edge. The model's bottom corners are at u = 16.0 / 85.6 (body-left = 0.814)
# and its sides lean inward 4.4 mm over 15.2 mm; we keep that slope but run the sides up to the
# Thing Plus's top edge instead of stopping at the model's 15.2 mm.
_TRAP_BL, _TRAP_BR = (16.0 - 0.814) * S, (85.6 - 0.814) * S
_TRAP_SLOPE = (20.4 - 16.0) / 15.2    # inward lean per mm of height (same in scaled units)
TRAP = [(_TRAP_BL, 0.0), (_TRAP_BR, 0.0),
        (_TRAP_BR - _TRAP_SLOPE * TP_TOP_V, TP_TOP_V), (_TRAP_BL + _TRAP_SLOPE * TP_TOP_V, TP_TOP_V)]

# ---- sheet placement ----
X0, Y0 = 140.0, 190.0                 # KiCad position of design (0,0); A3 sheet


def xy(u, v):
    return X0 + u, Y0 - v


_uid_n = 0


def uid():
    """Deterministic ids, so re-running the generator only changes files whose contents really changed."""
    global _uid_n
    _uid_n += 1
    return str(uuid.uuid5(uuid.NAMESPACE_URL, f"cassette-carrier/{_uid_n}"))


def f(x):
    return f"{x:.6f}".rstrip("0").rstrip(".")


def pt(u, v):
    x, y = xy(u, v)
    return f"{f(x)} {f(y)}"


def line(a, b, layer="Edge.Cuts", w=0.1):
    return (f'  (gr_line (start {pt(*a)}) (end {pt(*b)})\n'
            f'    (stroke (width {w}) (type default)) (layer "{layer}") (tstamp {uid()}))\n')


def arc(a, mid, b, layer="Edge.Cuts", w=0.1):
    return (f'  (gr_arc (start {pt(*a)}) (mid {pt(*mid)}) (end {pt(*b)})\n'
            f'    (stroke (width {w}) (type default)) (layer "{layer}") (tstamp {uid()}))\n')


def circle(c, r, layer, w=0.1):
    x, y = xy(*c)
    return (f'  (gr_circle (center {f(x)} {f(y)}) (end {f(x + r)} {f(y)})\n'
            f'    (stroke (width {w}) (type default)) (fill none) (layer "{layer}") (tstamp {uid()}))\n')


def poly(pts, layer):
    body = " ".join(f"(xy {pt(*p)})" for p in pts)
    return (f'  (gr_poly (pts {body})\n'
            f'    (stroke (width 0) (type default)) (fill solid) (layer "{layer}") (tstamp {uid()}))\n')


def text(u, v, s, layer="F.SilkS", size=1.2):
    return (f'  (gr_text "{s}" (at {pt(u, v)}) (layer "{layer}") (tstamp {uid()})\n'
            f'    (effects (font (size {size} {size}) (thickness 0.15))))\n')


# ---------------------------------------------------------------- board outline
def outline():
    s2 = math.sqrt(0.5)
    o = ""
    o += line((R, 0), (W - R, 0))
    o += arc((W - R, 0), (W - R + R * s2, R - R * s2), (W, R))                      # bottom right
    o += line((W, R), (W, BUMP_V[0]))
    o += line((W, BUMP_V[0]), (W + BUMP, BUMP_V[1]))
    o += line((W + BUMP, BUMP_V[1]), (W + BUMP, BUMP_V[2]))
    o += line((W + BUMP, BUMP_V[2]), (W, BUMP_V[3]))
    o += line((W, BUMP_V[3]), (W, H - R))
    o += arc((W, H - R), (W - R + R * s2, H - R + R * s2), (W - R, H))              # top right
    o += line((W - R, H), (R, H))
    o += arc((R, H), (R - R * s2, H - R + R * s2), (0, H - R))                      # top left
    o += line((0, H - R), (0, BUMP_V[3]))
    o += line((0, BUMP_V[3]), (-BUMP, BUMP_V[2]))
    o += line((-BUMP, BUMP_V[2]), (-BUMP, BUMP_V[1]))
    o += line((-BUMP, BUMP_V[1]), (0, BUMP_V[0]))
    o += line((0, BUMP_V[0]), (0, R))
    o += arc((0, R), (R - R * s2, R - R * s2), (R, 0))                              # bottom left
    return o


# ---------------------------------------------------------------- footprints
def arc_pts(cx, cy, r, a0, a1, n=24):
    """Points on an arc, y-down footprint coordinates, angles in degrees."""
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
             cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]


def touch_pad_polygon():
    """Copper outline of one touch pad in footprint coords (origin = middle of the pad's top edge, which sits on
    the board edge; y down). Top corners square (edge-plating), bottom corners rounded."""
    hw = PAD_W / 2
    pts = [(-hw, 0), (hw, 0), (hw, PAD_H - PAD_R)]
    pts += arc_pts(hw - PAD_R, PAD_H - PAD_R, PAD_R, 0, 90)          # bottom right
    pts += arc_pts(-hw + PAD_R, PAD_H - PAD_R, PAD_R, 90, 180)       # bottom left
    return pts


def touch_pad_footprint(at=None, ref="TCH"):
    hx = PAD_W / 2 - ANT_M                                          # antenna holes' x offset from the pad centre
    anchor = (-hx, ANT_M)
    pts = [(x - anchor[0], y - anchor[1]) for x, y in touch_pad_polygon()]   # primitives are relative to the pad
    xy_list = "\n".join(f"            (xy {f(x)} {f(y)})" for x, y in pts)
    name = "TouchPad_40x20"
    head = (f'(footprint "{"cassette:" if at else ""}{name}" ' +
            (f'(layer "F.Cu") (tstamp {uid()}) (at {f(at[0])} {f(at[1])})\n' if at else
             '(version 20221018) (generator pcbnew)\n  (layer "F.Cu")\n'))
    return (
        head +
        f'  (descr "Exposed-copper capacitive touch pad, {PAD_W:g} x {PAD_H:g} mm, top edge on the board edge, with two '
        f'plated M3 holes in the top corners for bolt-on antennas. Origin = middle of the top edge.")\n'
        f'  (attr through_hole)\n'
        f'  (fp_text reference "{ref}" (at 0 -3) (layer "F.Fab")\n'
        f'    (effects (font (size 1 1) (thickness 0.15))) (tstamp {uid()}))\n'
        f'  (fp_text value "{name}" (at 0 {PAD_H + 3}) (layer "F.Fab")\n'
        f'    (effects (font (size 1 1) (thickness 0.15))) (tstamp {uid()}))\n'
        f'  (pad "1" thru_hole custom (at {f(anchor[0])} {f(anchor[1])}) (size 6 6) (drill {M3_D}) (layers "F.Cu" "B.Cu" "*.Mask")\n'
        f'    (options (clearance outline) (anchor circle))\n'
        f'    (primitives\n'
        f'      (gr_poly\n'
        f'        (pts\n{xy_list}\n        )\n'
        f'        (width 0) (fill yes))\n'
        f'    ) (tstamp {uid()}))\n'
        f'  (pad "1" thru_hole circle (at {f(hx)} {f(ANT_M)}) (size 6 6) (drill {M3_D}) (layers "F.Cu" "B.Cu" "*.Mask") (tstamp {uid()}))\n'
        f')\n')


def tooling_hole(ref=None, u=0.0, v=0.0):
    """NPTH tooling hole. With ref/u/v: an instance for the board; without: the library footprint."""
    pad = (f'(pad "" np_thru_hole circle (at 0 0) (size {TOOL_D} {TOOL_D}) (drill {TOOL_D}) (layers "*.Cu" "*.Mask") '
           f'(solder_mask_margin {TOOL_MASK}) (tstamp {uid()}))')
    text = (f'(fp_text reference "{ref or "REF**"}" (at 0 -2) (layer "F.Fab")\n      (effects (font (size 1 1) (thickness 0.15))) (tstamp {uid()}))\n'
            f'    (fp_text value "ToolingHole_{TOOL_D}mm" (at 0 2) (layer "F.Fab")\n      (effects (font (size 1 1) (thickness 0.15))) (tstamp {uid()}))')
    descr = f'(descr "NPTH tooling hole, {TOOL_D} mm drill, {TOOL_MASK} mm solder-mask expansion")'
    if ref is None:
        return (f'(footprint "ToolingHole_{TOOL_D}mm" (version 20221018) (generator pcbnew)\n  (layer "F.Cu")\n  {descr}\n'
                f'  (attr exclude_from_pos_files exclude_from_bom)\n  {text}\n  {pad}\n)\n')
    x, y = xy(u, v)
    return (f'  (footprint "cassette:ToolingHole_{TOOL_D}mm" (layer "F.Cu") (tstamp {uid()}) (at {f(x)} {f(y)})\n'
            f'    {descr}\n    (attr exclude_from_pos_files exclude_from_bom)\n    {text}\n    {pad}\n  )\n')


def outline_poly(pts, layer, w=0.15):
    body = " ".join(f"(xy {pt(*p)})" for p in pts)
    return (f'  (gr_poly (pts {body})\n'
            f'    (stroke (width {w}) (type default)) (fill none) (layer "{layer}") (tstamp {uid()}))\n')


def hub_silk(cx, cv):
    """Hub rings plus the six inward sprocket teeth, on the silkscreen."""
    o = "".join(circle((cx, cv), r, "F.SilkS", 0.15) for r in HUB_SILK)
    r_out = HUB_SILK[0] - TOOTH_GAP
    r_in = r_out - TOOTH_LEN
    for a in TOOTH_ANGLES:
        c, s_ = math.cos(math.radians(a)), math.sin(math.radians(a))
        corners = [(r_out, -TOOTH_W / 2), (r_out, TOOTH_W / 2), (r_in, TOOTH_W / 2), (r_in, -TOOTH_W / 2)]
        o += outline_poly([(cx + x * c - y * s_, cv + x * s_ + y * c) for x, y in corners], "F.SilkS")
    return o


def screw5_silk():
    u, v = SCREW5
    o = circle((u, v), 1.6, "F.SilkS", 0.15)
    o += line((u - 2.6, v), (u + 2.6, v), "F.SilkS", 0.15) + line((u, v - 2.6), (u, v + 2.6), "F.SilkS", 0.15)
    return o + text(u, v - 4.2, "5th screw", size=1.0)


# SparkFun Thing Plus pad names, by pad number (see ThingPlus.kicad_mod in
# sparkfun/SparkFun-KiCad-Libraries, CC BY 4.0; rebuilt here in KiCad 7 format)
TP_LEFT = ["SDA", "SCL", "D6", "D5", "D4", "D3", "D2", "D1", "D0", "VBUS", "EN", "VBAT"]
TP_RIGHT = ["FREE", "TX", "RX", "POCI", "PICO", "SCK", "A5", "A4", "A3", "A2", "A1", "A0", "GND", "VREF", "3V3", "RST"]


def thing_plus_footprint(at=None, ref="U1"):
    """SparkFun Thing Plus RA6M5 (the long variant: 2.70 in PCB, antenna module overhanging to 2.871 in).
    Origin = centre of the 22.86 x 68.58 mm PCB; USB-C toward +y, antenna toward -y. Header pads are unchanged from
    SparkFun's KiCad footprint (shifted 5.08 mm because the origin moved); silkscreen positions of the switches, LEDs,
    USB-C shell, mounting holes and module are read off SparkFun's Thing Plus RA6M5 dimension drawing."""
    rot = 270 if at else 0                                   # 270: USB-C end faces left
    pre = "cassette:" if at else ""
    IN = 25.4
    L = lambda x_in, y_in: ((x_in - 0.45) * IN, (1.35 - y_in) * IN)      # drawing (in, from left/bottom edge) -> local mm
    head = (f'(footprint "{pre}ThingPlus" (layer "F.Cu") (tstamp {uid()}) (at {f(at[0])} {f(at[1])} {rot})\n' if at else
            '(footprint "ThingPlus" (version 20221018) (generator pcbnew)\n  (layer "F.Cu")\n')
    o = head
    o += ('  (descr "SparkFun Thing Plus RA6M5, long (2.70 in) variant with the BLE module overhanging the PCB end. Pad geometry '
          'from SparkFun-KiCad-Libraries (CC BY 4.0); outline, LED, switch and module positions from SparkFun\'s Thing Plus '
          'RA6M5 dimension drawing. Origin = PCB centre, USB-C toward +y.")\n'
          '  (attr through_hole)\n')
    o += (f'  (fp_text reference "{ref}" (at 0 -41 0) (layer "F.Fab")\n'
          f'    (effects (font (size 1 1) (thickness 0.15))) (tstamp {uid()}))\n'
          f'  (fp_text value "SparkFun_Thing_Plus_RA6M5" (at 0 38 0) (layer "F.Fab")\n'
          f'    (effects (font (size 1 1) (thickness 0.15))) (tstamp {uid()}))\n')

    def ln(x1, y1, x2, y2, layer="F.SilkS", w=0.15):
        return (f'  (fp_line (start {f(x1)} {f(y1)}) (end {f(x2)} {f(y2)}) (stroke (width {w}) (type default)) '
                f'(layer "{layer}") (tstamp {uid()}))\n')

    def box(x1, y1, x2, y2, layer="F.SilkS", w=0.15):
        return ln(x1, y1, x2, y1, layer, w) + ln(x2, y1, x2, y2, layer, w) + ln(x2, y2, x1, y2, layer, w) + ln(x1, y2, x1, y1, layer, w)

    def circ(cx, cy, r, layer="F.SilkS", w=0.15):
        return (f'  (fp_circle (center {f(cx)} {f(cy)}) (end {f(cx + r)} {f(cy)}) (stroke (width {w}) (type default)) '
                f'(fill none) (layer "{layer}") (tstamp {uid()}))\n')

    def label(txt, x, y, size=0.9, justify="", ang=0):
        j = f" (justify {justify})" if justify else ""
        return (f'  (fp_text user "{txt}" (at {f(x)} {f(y)} {ang}) (layer "F.SilkS")\n'
                f'    (effects (font (size {size} {size}) (thickness 0.15)){j}) (tstamp {uid()}))\n')

    hw, hl, cr = 11.43, 34.29, 0.10 * IN                     # PCB half-width, half-length, corner radius
    s2 = math.sqrt(0.5)
    for sx in (-1, 1):                                        # PCB outline: 4 lines + 4 rounded corners
        o += ln(sx * hw, -(hl - cr), sx * hw, hl - cr)
    for sy in (-1, 1):
        o += ln(-(hw - cr), sy * hl, hw - cr, sy * hl)
    for sx in (-1, 1):
        for sy in (-1, 1):
            cx, cy = sx * (hw - cr), sy * (hl - cr)
            o += (f'  (fp_arc (start {f(cx + sx * cr)} {f(cy)}) (mid {f(cx + sx * cr * s2)} {f(cy + sy * cr * s2)}) '
                  f'(end {f(cx)} {f(cy + sy * cr)}) (stroke (width 0.15) (type default)) (layer "F.SilkS") (tstamp {uid()}))\n')
    for hx_in, hy_in in ((0.10, 0.10), (0.80, 0.10), (0.10, 2.20), (0.80, 2.20)):     # the Thing Plus's own M3 holes
        o += circ(*L(hx_in, hy_in), 1.7, w=0.12)
    (x1, y1), (x2, y2) = L(0.20, 2.871), L(0.70, 2.30)                                   # BLE module (overhangs the PCB end)
    o += box(x1, y1, x2, y2)
    o += label("BLE", 0, (y1 + y2) / 2)
    (x1, y1), (x2, y2) = L(0.28, 0.25), L(0.62, -0.045)                                  # USB-C shell (overhangs slightly)
    o += box(x1, y1, x2, y2)
    o += label("USB-C", 0, (y1 + y2) / 2 + 0.3)
    for name, (a1, b1), (a2, b2), (pcx, pcy), (tx, ty) in (                              # tactile switches, with plunger circles
            ("USR", (0.531, 0.715), (0.641, 0.539), (0.587, 0.631), (0.586, 0.77)),
            ("RST", (0.618, 0.453), (0.783, 0.343), (0.700, 0.396), (0.700, 0.28))):
        (x1, y1), (x2, y2) = L(a1, b1), L(a2, b2)
        o += box(x1, y1, x2, y2) + circ(*L(pcx, pcy), 0.8, w=0.12) + label(name, *L(tx, ty), size=0.8)
    for name, (a1, b1), (a2, b2), y_in in (("RGB", (0.040, 2.620), (0.126, 2.535), 2.578),   # LEDs
                                           ("PWR", (0.030, 2.490), (0.140, 2.440), 2.465),
                                           ("STAT", (0.030, 2.380), (0.140, 2.335), 2.358)):
        (x1, y1), (x2, y2) = L(a1, b1), L(a2, b2)
        o += box(x1, y1, x2, y2) + label(name, -4.9, L(0, y_in)[1], size=0.8, ang=90)
    for (x1, y1, x2, y2, layer, w) in ((-11.7, -38.9, 11.7, -38.9, "F.CrtYd", 0.05), (11.7, -38.9, 11.7, 35.7, "F.CrtYd", 0.05),
                                        (11.7, 35.7, -11.7, 35.7, "F.CrtYd", 0.05), (-11.7, 35.7, -11.7, -38.9, "F.CrtYd", 0.05)):
        o += ln(x1, y1, x2, y2, layer, w)
    n = 0
    for names, x in ((TP_LEFT, -10.16), (TP_RIGHT, 10.16)):
        for i, name in enumerate(names):
            n += 1
            shape = "rect" if n == 1 else "circle"
            o += (f'  (pad "{n}" thru_hole {shape} (at {x} {f(-17.78 + 2.54 * i)}) (size 1.8796 1.8796) (drill 1.016) '
                  f'(layers "*.Cu" "*.Mask") (pinfunction "{name}") (tstamp {uid()}))\n')
    return o + ")\n"


# RV112FF-40 (Taiwan Alpha), shaft up, pins bent down at the factory. Origin = shaft axis; drawn as in the
# datasheet's "P.C.B. MOUNTING HOLE DETAIL" (pin row 10.0 mm on the +y side of the axis, y down). Pads, left to
# right in that view: wiper B, power, wiper A, ground.
POT_PINS = (("1", "WIPER_B", -3.75, "B"), ("2", "PWR", -1.25, "V"), ("3", "WIPER_A", 1.25, "A"), ("4", "GND", 3.75, "G"))


def pot_footprint(at=None, ref="RV", rot=0, text_rot=0):
    pre = "cassette:" if at else ""
    head = (f'(footprint "{pre}RV112FF-40" (layer "F.Cu") (tstamp {uid()}) (at {f(at[0])} {f(at[1])} {rot})\n' if at else
            '(footprint "RV112FF-40" (version 20221018) (generator pcbnew)\n  (layer "F.Cu")\n')
    o = head
    o += ('  (descr "Taiwan Alpha RV112FF-40 dual endless pot, shaft up, factory-bent pins. Footprint per the datasheet PCB '
          'hole detail: 4x 1.0 mm holes at 2.5 mm pitch, 10.0 mm from the shaft axis; two plated 2.0x2.1 mm case-clip holes on the '
          'axis, 13.0 mm apart. Origin = shaft axis.")\n  (attr through_hole)\n')
    o += (f'  (fp_text reference "{ref}" (at 0 -9 {text_rot}) (layer "F.Fab")\n'
          f'    (effects (font (size 1 1) (thickness 0.15))) (tstamp {uid()}))\n'
          f'  (fp_text value "RV112FF-40-25A-0B10K" (at 0 13 {text_rot}) (layer "F.Fab")\n'
          f'    (effects (font (size 1 1) (thickness 0.15))) (tstamp {uid()}))\n')
    def ln(x1, y1, x2, y2, layer, w):
        return (f'  (fp_line (start {f(x1)} {f(y1)}) (end {f(x2)} {f(y2)}) (stroke (width {w}) (type default)) '
                f'(layer "{layer}") (tstamp {uid()}))\n')
    def box(x1, y1, x2, y2, layer, w):
        return ln(x1, y1, x2, y1, layer, w) + ln(x2, y1, x2, y2, layer, w) + ln(x2, y2, x1, y2, layer, w) + ln(x1, y2, x1, y1, layer, w)
    o += box(-5.5, -6.7, 5.5, 8.7, "F.SilkS", 0.15)                       # case outline (11.0 wide, 6.7 / 8.7 mm about the axis)
    o += box(-6.3, -7.0, 6.3, 10.9, "F.CrtYd", 0.05)
    for num, cx in (("5", -6.5), ("6", 6.5)):                              # case-clip holes: plated so the clips can be soldered
        o += (f'  (pad "{num}" thru_hole rect (at {cx} 0) (size 3.6 3.7) (drill oval 2.0 2.1) (layers "*.Cu" "*.Mask") '
              f'(pinfunction "CASE") (tstamp {uid()}))\n')
    o += (f'  (fp_circle (center 0 0) (end 3 0) (stroke (width 0.1) (type default)) (fill none) (layer "F.Fab") (tstamp {uid()}))\n')
    for num, name, x, lab in POT_PINS:
        o += (f'  (pad "{num}" thru_hole circle (at {x} 10) (size 1.7 1.7) (drill 1.0) (layers "*.Cu" "*.Mask") '
              f'(pinfunction "{name}") (tstamp {uid()}))\n')
        o += (f'  (fp_text user "{lab}" (at {x} 12 {text_rot}) (layer "F.SilkS")\n'
              f'    (effects (font (size 1 1) (thickness 0.15))) (tstamp {uid()}))\n')
    return o + ")\n"



# Kycon STX-3120-3B 3.5 mm stereo jack (through-hole, right angle). From Kycon's drawing (a BOTTOM view, mirrored here to
# top-view): origin = centre of the barrel's front face, +x toward the back of the jack, y down. Pin 1 (sleeve) is 6.5 mm
# back on the axis; pins 2 and 5 are 11.5 mm back, 5.0 mm either side. Tip/ring on pins 2/5 is UNVERIFIED (schematic
# symbol suggests 5 = tip, 2 = ring): check with a continuity test on the real part before wiring.
JACK_PADS = (("1", "SLEEVE", 6.5, 0.0), ("2", "PIN2", 11.5, -5.0), ("5", "PIN5", 11.5, 5.0))
JACK_PEGS = ((6.5, -5.0), (6.5, 5.0), (11.5, 0.0))


def jack_footprint(at=None, ref="J1", rot=0, text_rot=0):
    pre = "cassette:" if at else ""
    head = (f'(footprint "{pre}Kycon_STX-3120-3B" (layer "F.Cu") (tstamp {uid()}) (at {f(at[0])} {f(at[1])} {rot})\n' if at else
            '(footprint "Kycon_STX-3120-3B" (version 20221018) (generator pcbnew)\n  (layer "F.Cu")\n')
    o = head
    o += ('  (descr "Kycon STX-3120-3B 3.5 mm stereo jack, all plastic, right angle. Origin = centre of the barrel front face, '
          '+x toward the back. 3x 1.3 mm pin holes + 3x 1.6 mm locating pegs. Tip/ring on pins 2/5 unverified.")\n'
          '  (attr through_hole)\n')
    o += (f'  (fp_text reference "{ref}" (at 7.5 -8 {text_rot}) (layer "F.Fab")\n'
          f'    (effects (font (size 1 1) (thickness 0.15))) (tstamp {uid()}))\n'
          f'  (fp_text value "STX-3120-3B" (at 7.5 8 {text_rot}) (layer "F.Fab")\n'
          f'    (effects (font (size 1 1) (thickness 0.15))) (tstamp {uid()}))\n')
    def ln(x1, y1, x2, y2, layer, w):
        return (f'  (fp_line (start {f(x1)} {f(y1)}) (end {f(x2)} {f(y2)}) (stroke (width {w}) (type default)) '
                f'(layer "{layer}") (tstamp {uid()}))\n')
    def box(x1, y1, x2, y2, layer, w):
        return ln(x1, y1, x2, y1, layer, w) + ln(x2, y1, x2, y2, layer, w) + ln(x2, y2, x1, y2, layer, w) + ln(x1, y2, x1, y1, layer, w)
    o += box(3.5, -6.0, 14.1, 6.0, "F.SilkS", 0.15)          # housing 12.0 wide
    o += box(0.0, -3.4, 3.5, 3.4, "F.Fab", 0.1)              # barrel, 6.8 wide
    o += box(-0.2, -6.2, 15.3, 6.2, "F.CrtYd", 0.05)
    for num, name, x, y in JACK_PADS:
        o += (f'  (pad "{num}" thru_hole circle (at {x} {y}) (size 2.2 2.2) (drill 1.3) (layers "*.Cu" "*.Mask") '
              f'(pinfunction "{name}") (tstamp {uid()}))\n')
    for x, y in JACK_PEGS:
        o += (f'  (pad "" np_thru_hole circle (at {x} {y}) (size 1.6 1.6) (drill 1.6) (layers "*.Cu" "*.Mask") (tstamp {uid()}))\n')
    return o + ")\n"


# ---------------------------------------------------------------- assemble
HEADER = open(pathlib.Path(__file__).with_name("board_header.txt")).read()


def trapezoid_keepout():
    """No copper pour on any layer inside the trapezoid; tracks, vias and pads are still
    allowed. Edit the layer list to let a plane through (e.g. drop "In1.Cu")."""
    pts = " ".join(f"(xy {pt(*q)})" for q in TRAP)
    return (f'  (zone (net 0) (net_name "") (layers "F.Cu" "In1.Cu" "In2.Cu" "B.Cu") (tstamp {uid()})\n'
            f'    (name "Trapezoid: no copper pour") (hatch edge 0.5)\n'
            f'    (connect_pads (clearance 0))\n'
            f'    (min_thickness 0.25) (filled_areas_thickness no)\n'
            f'    (keepout (tracks allowed) (vias allowed) (pads allowed) (copperpour not_allowed) (footprints allowed))\n'
            f'    (fill (thermal_gap 0.5) (thermal_bridge_width 0.5))\n'
            f'    (polygon (pts {pts})))\n')


def build():
    s = HEADER
    s += outline()
    for hx in HUB_X:
        s += circle((hx, HUB_V), HUB_HOLE_D / 2, "Edge.Cuts")
        s += hub_silk(hx, HUB_V)
    s += text(HUB_X[0], HUB_V - HUB_SILK[1] - 2, "A")
    s += text(HUB_X[1], HUB_V - HUB_SILK[1] - 2, "B")
    s += trapezoid_keepout()
    s += poly(TRAP, "F.Mask") + poly(TRAP, "B.Mask")        # mask open both sides: bare FR4
    s += screw5_silk()
    for ref, cu in (("TCH1", PAD_INSET + PAD_W / 2), ("TCH2", W - PAD_INSET - PAD_W / 2)):
        x, y = xy(cu, H); s += "  " + touch_pad_footprint((x, y), ref).replace("\n", "\n  ").rstrip() + "\n"
    for ref, hx in (("RV1", HUB_X[0]), ("RV2", HUB_X[1])):     # pin row toward the top of the board = rotate 180
        x, y = xy(hx, HUB_V); s += "  " + pot_footprint((x, y), ref, 180, 0).replace("\n", "\n  ").rstrip() + "\n"
    x, y = xy(W / 2, H); s += "  " + jack_footprint((x, y), "J1", 270, 0).replace("\n", "\n  ").rstrip() + "\n"   # barrel flush with the top edge, facing up
    x, y = xy(*TP_CENTRE); s += "  " + thing_plus_footprint((x, y), "U1").replace("\n", "\n  ").rstrip() + "\n"
    for ref, (u, v) in (("TH1", (R, H - R)), ("TH2", (W - R, H - R)), ("TH3", (R, R)), ("TH4", (W - R, R))):
        s += tooling_hole(ref, u, v)
    s += ")\n"
    return s


if __name__ == "__main__":
    _uid_n = 0
    (OUT / "cassette.pretty").mkdir(exist_ok=True)
    (OUT / "cassette.pretty" / "TouchPad_40x20.kicad_mod").write_text(touch_pad_footprint())
    (OUT / "cassette.pretty" / "ThingPlus.kicad_mod").write_text(thing_plus_footprint())
    (OUT / "cassette.pretty" / "RV112FF-40.kicad_mod").write_text(pot_footprint())
    (OUT / "cassette.pretty" / "Kycon_STX-3120-3B.kicad_mod").write_text(jack_footprint())
    (OUT / "cassette.pretty" / "ToolingHole_1.152mm.kicad_mod").write_text(tooling_hole())
    (OUT / "cassette-carrier.kicad_pcb").write_text(build())
    (OUT / "cassette-carrier.kicad_pro").write_text('{\n  "meta": {\n    "filename": "cassette-carrier.kicad_pro",\n    "version": 1\n  }\n}\n')
    (OUT / "fp-lib-table").write_text(
        '(fp_lib_table\n  (version 7)\n'
        '  (lib (name "cassette")(type "KiCad")(uri "${KIPRJMOD}/cassette.pretty")(options "")(descr "Project footprints"))\n)\n')
    print(f"trapezoid top v={TP_TOP_V:.2f}, top width {TRAP[2][0]-TRAP[3][0]:.1f}, bottom width {TRAP[1][0]-TRAP[0][0]:.1f}")
    print(f"board {W:.3f} x {H:.3f} mm, hubs at u={HUB_X[0]:.3f},{HUB_X[1]:.3f} v={HUB_V:.3f}, corner R={R:.3f}")
