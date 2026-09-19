#!/usr/bin/env python3
"""
Generate the starting KiCad 7 board for the Compact-Cassette-shaped carrier PCB:
outline, hub cutouts, M3 holes, touch-pad footprints, hub silkscreen, and the
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
PAD_W, PAD_H, PAD_INNER_R = 40.0, 20.0, 5.0
# hub sprocket outline (from the patent drawing): six inward teeth on the hub bore
TOOTH_ANGLES = (90, 30, -30, -90, -150, 150)
TOOTH_LEN, TOOTH_W, TOOTH_GAP = 1.73 * S, 1.5 * S, 0.3
# fifth screw: bottom centre, ~13.6 mm above the bottom edge on the (unscaled) patent drawing
SCREW5 = (W / 2, 13.6 * S)
# Thing Plus: rows 20.32 apart, USB facing left, lower row's copper 2 mm above the board edge
TP_PAD_R = 1.8796 / 2
TP_ROW_V = 2.0 + TP_PAD_R
TP_CENTRE = (W / 2, TP_ROW_V + 20.32 / 2)
TP_TOP_V = TP_CENTRE[1] + 11.43       # top edge of the Thing Plus footprint outline

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


def uid():
    return str(uuid.uuid4())


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


def touch_pad_polygon(mirror):
    """Copper outline for the top-left corner pad, in footprint coords (origin =
    M3 hole centre, y down). The outer corner follows the board's corner arc."""
    pts = [(0, R)]
    pts += arc_pts(R, R, R, 180, 270)                       # outer corner (board radius)
    pts += [(PAD_W, 0), (PAD_W, PAD_H - PAD_INNER_R)]
    pts += arc_pts(PAD_W - PAD_INNER_R, PAD_H - PAD_INNER_R, PAD_INNER_R, 0, 90)   # inner corner
    pts += [(0, PAD_H)]
    pts = [(x - R, y - R) for x, y in pts]                 # move origin to hole centre
    if mirror:
        pts = [(-x, y) for x, y in pts]
    return pts


def touch_pad_footprint(name, mirror, at=None, ref="TCH"):
    pts = touch_pad_polygon(mirror)
    xy_list = "\n".join(f"            (xy {f(x)} {f(y)})" for x, y in pts)
    head = (f'(footprint "{"cassette:" if at else ""}{name}" ' +
            (f'(layer "F.Cu") (tstamp {uid()}) (at {f(at[0])} {f(at[1])})\n' if at else
             '(version 20221018) (generator pcbnew)\n  (layer "F.Cu")\n'))
    return (
        head +
        f'  (descr "Exposed-copper capacitive touch pad, {PAD_W:g} x {PAD_H:g} mm in a board corner, '
        f'with a plated M3 hole for a bolt-on antenna. Origin = hole centre.")\n'
        f'  (attr through_hole)\n'
        f'  (fp_text reference "{ref}" (at 0 -7) (layer "F.Fab")\n'
        f'    (effects (font (size 1 1) (thickness 0.15))) (tstamp {uid()}))\n'
        f'  (fp_text value "{name}" (at 0 7) (layer "F.Fab")\n'
        f'    (effects (font (size 1 1) (thickness 0.15))) (tstamp {uid()}))\n'
        f'  (pad "1" thru_hole custom (at 0 0) (size 6 6) (drill {M3_D}) (layers "F.Cu" "B.Cu" "*.Mask")\n'
        f'    (options (clearance outline) (anchor circle))\n'
        f'    (primitives\n'
        f'      (gr_poly\n'
        f'        (pts\n{xy_list}\n        )\n'
        f'        (width 0) (fill yes))\n'
        f'    ) (tstamp {uid()}))\n'
        f')\n')


def mounting_hole(ref, u, v):
    x, y = xy(u, v)
    return (
        f'  (footprint "cassette:MountingHole_3.2mm_M3_Plated" (layer "F.Cu") (tstamp {uid()}) (at {f(x)} {f(y)})\n'
        f'    (descr "Plated M3 mounting hole, 3.2 mm drill / 6 mm pad (net it to GND, or leave floating)")\n'
        f'    (attr exclude_from_pos_files exclude_from_bom)\n'
        f'    (fp_text reference "{ref}" (at 0 -4.2) (layer "F.Fab")\n'
        f'      (effects (font (size 1 1) (thickness 0.15))) (tstamp {uid()}))\n'
        f'    (fp_text value "MountingHole_3.2mm_M3_Plated" (at 0 4.2) (layer "F.Fab")\n'
        f'      (effects (font (size 1 1) (thickness 0.15))) (tstamp {uid()}))\n'
        f'    (pad "1" thru_hole circle (at 0 0) (size 6 6) (drill {M3_D}) (layers "*.Cu" "*.Mask") (tstamp {uid()}))\n'
        f'  )\n')



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
    rot = 270 if at else 0                                   # 270: USB-C end faces left
    pre = "cassette:" if at else ""
    head = (f'(footprint "{pre}ThingPlus" (layer "F.Cu") (tstamp {uid()}) (at {f(at[0])} {f(at[1])} {rot})\n' if at else
            '(footprint "ThingPlus" (version 20221018) (generator pcbnew)\n  (layer "F.Cu")\n')
    o = head
    o += ('  (descr "SparkFun Thing Plus (RA6M5) header footprint: 2.54 mm pitch, rows 20.32 mm apart. '
          'Pad geometry from SparkFun-KiCad-Libraries (CC BY 4.0). Origin = module centre, USB-C toward +y.")\n'
          '  (attr through_hole)\n')
    o += (f'  (fp_text reference "{ref}" (at 0 -31.5 {rot}) (layer "F.Fab")\n'
          f'    (effects (font (size 1 1) (thickness 0.15))) (tstamp {uid()}))\n'
          f'  (fp_text value "SparkFun_Thing_Plus_RA6M5" (at 0 31.5 {rot}) (layer "F.Fab")\n'
          f'    (effects (font (size 1 1) (thickness 0.15))) (tstamp {uid()}))\n')
    for txt, y in (("USB-C", 26.0), ("BLE ANT", -26.0)):
        o += (f'  (fp_text user "{txt}" (at 0 {y} {rot}) (layer "F.SilkS")\n'
              f'    (effects (font (size 1 1) (thickness 0.15))) (tstamp {uid()}))\n')
    for (x1, y1, x2, y2, layer, w) in ((-11.43, -29.21, 11.43, -29.21, "F.SilkS", 0.15), (11.43, -29.21, 11.43, 29.21, "F.SilkS", 0.15),
                                        (11.43, 29.21, -11.43, 29.21, "F.SilkS", 0.15), (-11.43, 29.21, -11.43, -29.21, "F.SilkS", 0.15),
                                        (-11.7, -29.5, 11.7, -29.5, "F.CrtYd", 0.05), (11.7, -29.5, 11.7, 29.5, "F.CrtYd", 0.05),
                                        (11.7, 29.5, -11.7, 29.5, "F.CrtYd", 0.05), (-11.7, 29.5, -11.7, -29.5, "F.CrtYd", 0.05)):
        o += (f'  (fp_line (start {x1} {y1}) (end {x2} {y2}) (stroke (width {w}) (type default)) '
              f'(layer "{layer}") (tstamp {uid()}))\n')
    n = 0
    for col, names, x in ((0, TP_LEFT, -10.16), (1, TP_RIGHT, 10.16)):
        for i, name in enumerate(names):
            n += 1
            shape = "rect" if n == 1 else "circle"
            o += (f'  (pad "{n}" thru_hole {shape} (at {x} {f(-22.86 + 2.54 * i)}) (size 1.8796 1.8796) (drill 1.016) '
                  f'(layers "*.Cu" "*.Mask") (pinfunction "{name}") (tstamp {uid()}))\n')
    return o + ")\n"


def mounting_hole_lib():
    return (
        '(footprint "MountingHole_3.2mm_M3_Plated" (version 20221018) (generator pcbnew)\n'
        '  (layer "F.Cu")\n'
        '  (descr "Plated M3 mounting hole, 3.2 mm drill / 6 mm pad (net it to GND, or leave floating)")\n'
        '  (attr exclude_from_pos_files exclude_from_bom)\n'
        f'  (fp_text reference "REF**" (at 0 -4.2) (layer "F.Fab")\n'
        f'    (effects (font (size 1 1) (thickness 0.15))) (tstamp {uid()}))\n'
        f'  (fp_text value "MountingHole_3.2mm_M3_Plated" (at 0 4.2) (layer "F.Fab")\n'
        f'    (effects (font (size 1 1) (thickness 0.15))) (tstamp {uid()}))\n'
        f'  (pad "1" thru_hole circle (at 0 0) (size 6 6) (drill {M3_D}) (layers "*.Cu" "*.Mask") (tstamp {uid()}))\n'
        ')\n')


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
    tl_hole = (R, H - R)
    tr_hole = (W - R, H - R)
    s = HEADER
    s += outline()
    for i, (hx) in enumerate(HUB_X, 1):
        s += circle((hx, HUB_V), HUB_HOLE_D / 2, "Edge.Cuts")
        s += hub_silk(hx, HUB_V)
    s += text(HUB_X[0], HUB_V - HUB_SILK[1] - 2, "A")
    s += text(HUB_X[1], HUB_V - HUB_SILK[1] - 2, "B")
    s += trapezoid_keepout()
    s += poly(TRAP, "F.Mask") + poly(TRAP, "B.Mask")        # mask open both sides: bare FR4
    s += screw5_silk()
    x, y = xy(*tl_hole); s += "  " + touch_pad_footprint("TouchPad_TL", False, (x, y), "TCH1").replace("\n", "\n  ").rstrip() + "\n"
    x, y = xy(*tr_hole); s += "  " + touch_pad_footprint("TouchPad_TR", True, (x, y), "TCH2").replace("\n", "\n  ").rstrip() + "\n"
    x, y = xy(*TP_CENTRE); s += "  " + thing_plus_footprint((x, y), "U1").replace("\n", "\n  ").rstrip() + "\n"
    s += mounting_hole("H1", R, R)
    s += mounting_hole("H2", W - R, R)
    s += ")\n"
    return s


if __name__ == "__main__":
    (OUT / "cassette.pretty").mkdir(exist_ok=True)
    (OUT / "cassette.pretty" / "TouchPad_TL.kicad_mod").write_text(touch_pad_footprint("TouchPad_TL", False))
    (OUT / "cassette.pretty" / "TouchPad_TR.kicad_mod").write_text(touch_pad_footprint("TouchPad_TR", True))
    (OUT / "cassette.pretty" / "ThingPlus.kicad_mod").write_text(thing_plus_footprint())
    (OUT / "cassette.pretty" / "MountingHole_3.2mm_M3_Plated.kicad_mod").write_text(mounting_hole_lib())
    (OUT / "cassette-carrier.kicad_pcb").write_text(build())
    (OUT / "cassette-carrier.kicad_pro").write_text('{\n  "meta": {\n    "filename": "cassette-carrier.kicad_pro",\n    "version": 1\n  }\n}\n')
    (OUT / "fp-lib-table").write_text(
        '(fp_lib_table\n  (version 7)\n'
        '  (lib (name "cassette")(type "KiCad")(uri "${KIPRJMOD}/cassette.pretty")(options "")(descr "Project footprints"))\n)\n')
    print(f"trapezoid top v={TP_TOP_V:.2f}, top width {TRAP[2][0]-TRAP[3][0]:.1f}, bottom width {TRAP[1][0]-TRAP[0][0]:.1f}")
    print(f"board {W:.3f} x {H:.3f} mm, hubs at u={HUB_X[0]:.3f},{HUB_X[1]:.3f} v={HUB_V:.3f}, corner R={R:.3f}")
