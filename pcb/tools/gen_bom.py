#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Mykle Hansen
# Specified by Mykle Hansen, implemented by Claude (Anthropic).

"""Write cassette-carrier-bom.csv: one line per distinct part to buy, grouped from the schematic.

Reference designators, values and footprints come from the schematic's netlist, so the counts can't drift
from the design. What to BUY (spec, manufacturer, part number) lives in PARTS below, keyed by
"value|footprint". Board-only items (touch pads, mounting holes, the cut jumper) are skipped.
Mouser part numbers live in MOUSER (and in EXTRA for the knobs, battery and hardware).

Usage:  python3 tools/gen_bom.py              (run from pcb/; needs kicad-cli 7, path below)
        python3 tools/gen_bom.py --order 2    also write a Mouser order for 2 boards (needs a Mouser API key,
                                              see tools/mouser_search.py) - upload it at mouser.com/bom
"""
import argparse, csv, math, pathlib, subprocess, sys, tempfile
import xml.etree.ElementTree as ET
from collections import defaultdict

HERE = pathlib.Path(__file__).resolve().parent.parent
KICAD_CLI = "/Applications/KiCad 7/KiCad.app/Contents/MacOS/kicad-cli"
SKIP = ("TCH", "MH", "JP")             # board-only: pads, mounting holes, and the jumper inside R3's footprint
R_AXIAL = "Resistor_THT:R_Axial_DIN0204_L3.6mm_D1.6mm_P7.62mm_Horizontal"
C_DISC = "Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm"

# (description, requirements, purpose, manufacturer, MPN, note).  Blank MPN = generic part: buy to the requirements.
PARTS = {
    f"1.0n|{C_DISC}": ("1.0 nF capacitor, radial", "C0G/NP0 or film, 5% or better, >=16 V, 5 mm pitch",
                       "Audio low-pass with R1/R2 (5.1k): sets the corner, so avoid X7R",  "", "",
                       "For the firmware's default, DAC output amplifiers on. With them off: 6.8 nF and 1 kOhm"),
    "2.2u|cassette:C_DCBlock_Dual": ("2.2 uF film capacitor, radial box", "Film (PET/PP), >=25 V, 5-15 mm pitch, under 12 mm tall",
                                     "Audio DC blockers, one per channel", "", "",
                                     "Film boxes of 2.2 uF are 10.5-12.5 mm tall, lowest at 15 mm pitch. Either way round: leads in the "
                                     "square pad and the hole farthest from it"),
    f"100n|{C_DISC}": ("100 nF capacitor, radial", "X7R or better, >=16 V, 5 mm pitch", "Pot wiper noise filter (4x)", "", "", ""),
    "10u|Capacitor_THT:CP_Radial_D5.0mm_P2.00mm": ("10 uF aluminium electrolytic, radial", ">=16 V, 5 mm dia, 2.0 mm pitch, under 12 mm tall",
                                                   "Pot supply filter", "", "", "Polarised: long lead = +"),
    f"10n|{C_DISC}": ("10 nF capacitor, radial", "C0G or X7R, 10% or better, >=16 V, 5 mm pitch",
                      "Touch sensing (CTSU) TSCAP reference: touch won't work without it", "", "", ""),
    f"5.1k|{R_AXIAL}": ("5.1 kOhm resistor, axial", "1% metal film, 1/8-1/6 W (0204 body, 7.62 mm pitch)",
                        "Audio low-pass with C1/C2, and the DAC output amplifiers' load (5 kOhm min)", "", "", ""),
    "22|cassette:R_Axial_DIN0204_P7.62mm_CutJumper": ("22 Ohm resistor, axial", "1% metal film, 1/8-1/6 W (0204 body)",
                                                     "Optional pot supply filter", "", "",
                                                     "Shorted by a jumper in the footprint: cut the jumper to use it"),
    f"560|{R_AXIAL}": ("560 Ohm resistor, axial", "1% metal film, 1/8-1/6 W (0204 body)", "Touch pad series resistor", "", "", ""),
    "RV112FF-40-25A-0B10K|cassette:RV112FF-40": ("Dual 10 kOhm endless rotary pot", "25 mm plastic shaft (with the SPK-013A knob and 17 mm lid)",
                                                 "The two knobs", "Taiwan Alpha", "RV112FF-40-25A-0B10K",
                                                 "Shaft length is in the part number; the footprint fits every length"),
    "STX-3120-3B|cassette:Kycon_STX-3120-3B": ("3.5 mm stereo jack, right angle", "This exact part (custom footprint)", "Audio out",
                                               "Kycon", "STX-3120-3B", "Colour variants differ only in suffix"),
    "EG1224|Button_Switch_THT:SW_E-Switch_EG1224_SPDT_Angled": ("Slide switch, SPDT, right angle", "This exact part (stock footprint)",
                                                                "Power: switches the battery line to the Thing Plus's VBAT", "E-Switch", "EG1224", ""),
    "LiPo JST-PH|Connector_JST:JST_PH_S2B-PH-K_1x02_P2.00mm_Horizontal": ("JST PH 2-pin socket, side entry", "This exact part",
                                                                          "Battery socket (use this one, not the Thing Plus's own)", "JST",
                                                                          "S2B-PH-K-S(LF)(SN)", "Vendor photos often show a longer PH header"),
    "1x12 socket|None": ("1x12 female header, 2.54 mm, low profile", "Turned-pin (round), 2.4-3 mm tall", "Holds the Thing Plus's left column",
                         "HCTL", "PM254-1-12-Z-3.0-C", "Wants round male pins: see the TS headers"),
    "1x16 socket|None": ("1x16 female header, 2.54 mm, low profile", "Turned-pin (round), 2.4-3 mm tall", "Holds the Thing Plus's right column",
                         "HCTL", "PM254-1-16-Z-3.0-C", "Wants round male pins: see the TS headers"),
    "Thing Plus RA6M5|cassette:ThingPlus": ("SparkFun Thing Plus RA6M5", "Sold without headers", "The microcontroller board",
                                            "SparkFun", "WRL-24243", "Buy from SparkFun: listed but not stocked at Mouser"),
}
# LCSC / JLCPCB loose-parts picks, looked up 2026-09-21 (price = USD at qty 1, stock as listed then; recheck before ordering).
# key -> (LCSC #, LCSC part, price, stock, note).  No entry = not stocked there.
LCSC = {
    "2.2u|cassette:C_DCBlock_Dual": ("C53070641", "DGCX MPE225J2AB070B200ZR", 0.35, 750, "Polyester film 100 V 10%, 7.5 mm pitch. Alts: C3019042 KYET 5 mm $0.578; C46638925 WALSON 15 mm $0.203. Check height < 10 mm"),
    f"100n|{C_DISC}": ("C5632426", "KNSCHA 178MU0002", 0.0138, 5076, "X7R 50 V 10%, 5.08 mm pitch"),
    "10u|Capacitor_THT:CP_Radial_D5.0mm_P2.00mm": ("C55265437", "TOPAZ ESS1V100M050700B", 0.0093, 3244, "35 V, 5 x 7 mm, 2 mm pitch"),
    f"10n|{C_DISC}": ("C454414", "TORCH CC4-0805-CG-50V-0.01uF-J", 0.1285, 6016, "C0G 5%, 5.08 mm pitch. Cheaper X7R: C5632432 KNSCHA $0.0196"),
    "22|cassette:R_Axial_DIN0204_P7.62mm_CutJumper": ("C2848542", "VO MF1/8W-22R-1%-OT52", 0.0066, 2805, "1% metal film, 1/8 W, 2.3 x 3.7 mm"),
    f"560|{R_AXIAL}": ("", "", None, None, "No 1/8 W metal film in stock. 1/4 W bodies are ~6.3 mm long (tight on 7.62 pitch): C2848609 VO $0.0065, C1369978 YAGEO $0.0183"),
    "EG1224|Button_Switch_THT:SW_E-Switch_EG1224_SPDT_Angled": ("C273395", "E-Switch EG1224", 1.7269, 157, ""),
    "LiPo JST-PH|Connector_JST:JST_PH_S2B-PH-K_1x02_P2.00mm_Horizontal": ("C173752", "JST S2B-PH-K-S(LF)(SN)", 0.0395, 193766, "Looked up 2026-09-25"),
    "1x12 socket|None": ("C5156451", "HCTL PM254-1-12-Z-3.0-C", 0.4265, 496, "$0.3337 at 10+"),
    "1x16 socket|None": ("C5159943", "HCTL PM254-1-16-Z-3.0-C", 0.4704, 223, "$0.3728 at 10+"),
}
# Mouser picks, looked up 2026-09-26 with tools/mouser_search.py (stock/prices move: gen_bom.py --order rechecks them).
# key -> (Mouser #, note).  No entry = not stocked at Mouser.
MOUSER = {
    f"1.0n|{C_DISC}": ("810-FG28C0G1H102JNT6", "TDK FG28C0G1H102JNT06: C0G 5% 50 V, 5 mm pitch"),
    "2.2u|cassette:C_DCBlock_Dual": ("871-B32522C225J", "TDK B32522C0225J000: PET film 5% 63 V, 15 mm pitch, 18 x 5 x 10.5 mm (lowest 2.2 uF film found)"),
    f"100n|{C_DISC}": ("810-FG28X7R1H104KNT0", "TDK FG28X7R1H104KNT00: X7R 10% 50 V, 5 mm pitch"),
    "10u|Capacitor_THT:CP_Radial_D5.0mm_P2.00mm": ("667-ECA-1VM100", "Panasonic ECA-1VM100: 35 V 20%, 5 x 11 mm, 2.0 mm pitch; long-running M series (Lelon SS 5 x 5 went EOL)"),
    f"10n|{C_DISC}": ("810-FG28C0G1H103JNT6", "TDK FG28C0G1H103JNT06: C0G 5% 50 V, 5 mm pitch"),
    f"5.1k|{R_AXIAL}": ("603-MFR-12FTF52-5K1", "YAGEO MFR-12FTF52-5K1: 1% metal film, 1/6 W, 0204 body"),
    "22|cassette:R_Axial_DIN0204_P7.62mm_CutJumper": ("603-MFR-12FTF52-22R", "YAGEO MFR-12FTF52-22R: 1% metal film, 1/6 W, 0204 body"),
    f"560|{R_AXIAL}": ("603-MFR-12FTF52-560R", "YAGEO MFR-12FTF52-560R: 1% metal film, 1/6 W, 0204 body"),
    "RV112FF-40-25A-0B10K|cassette:RV112FF-40": ("317-1240FF-25A-B10K", ""),
    "STX-3120-3B|cassette:Kycon_STX-3120-3B": ("806-STX-3120-3B", ""),
    "EG1224|Button_Switch_THT:SW_E-Switch_EG1224_SPDT_Angled": ("612-EG1224", ""),
    "LiPo JST-PH|Connector_JST:JST_PH_S2B-PH-K_1x02_P2.00mm_Horizontal": ("306-S2BPHKSLFSNPP", ""),
    "1x12 socket|None": ("200-SL112TT12", "Samtec SL-112-TT-12: body 1.27 mm + contact rim 0.38-0.71 mm = ~2 mm tall; 4.83 mm pin (drawing SX-1XX-XX-XX-X-X)"),
    "1x16 socket|None": ("200-SL116TT12", "Samtec SL-116-TT-12: body 1.27 mm + contact rim 0.38-0.71 mm = ~2 mm tall; 4.83 mm pin (drawing SX-1XX-XX-XX-X-X)"),
}
# Case stack, measured on the first builds (2026-11). At each corner, top to bottom: M3 x 30 screw / lid PCB / 16 mm spacer /
# carrier / 5 mm spacer / base PCB, 25.8 mm in all, so 4.2 mm of screw comes out under the base. In front it takes an M3 nut;
# at the back a 10 mm female-female hex standoff, which doubles as a taller foot and gives the case a forward rake on a table.
# Top 16 mm: with the header pins left untrimmed they bottom out in the low-profile sockets, so the Thing Plus's underside
# sits just under 8 mm above the carrier, and its own JST socket and the knobs set the rest. The battery rides in this layer too,
# between the pots. Bottom 5 mm: nothing lives there; it only has to clear the hold-down nuts and screw tips (~2 mm min).
# M3 x 30 is the longest stock length that's easy to find; a bigger corner hole would open up more choices.
# Touch points: a brass M3 screw through each touch pad's hole in the lid / 16 mm spacer / carrier / brass nut. 22 mm is the
# length that fits: lid + spacer + carrier is 19.2 mm and the nut 2.4, which leaves the tip 2.2 mm clear of the base. A 25 mm
# screw touches the base; a 20 mm one leaves too little thread for the nut.
# Thing Plus hold-down: only three of its four holes are drilled in the carrier - the fourth (top left, by C3) would have cut
# into the hub hole beside it. Stack: M2.5 screw / Thing Plus / 7 mm spacer + thin washers (to fill the gap) / carrier / nut.
# 11 mm of boards and gap, then the 1.8 mm nut: 12 mm is too short. A 16 mm tip reaches the base, 5 mm under the carrier,
# but the base is another copy of the carrier, with a hole in the same place, so the tip just goes into it. Snug the screws gently: pulling the Thing Plus down onto its bottomed-out pins
# strains the header joints.
# (description, requirements, purpose, qty per board, manufacturer, MPN, references, note, Mouser #)
EXTRA = [
    ("Knob, 24.1 mm dia x 10.3 mm, black, white indicator", "D-bore to fit the RV112FF shaft", "Knob (small)", 1,
     "Same Sky", "SPK-013A", "RV1 or RV2", "One of each size to try; either fits either pot", "179-SPK-013A"),
    ("Knob, 32.5 mm dia x 10.3 mm, black, white indicator", "D-bore to fit the RV112FF shaft", "Knob (large)", 1,
     "Same Sky", "SPK-014A", "RV1 or RV2", "Same height and 7.2 mm bore depth as the SPK-013A", "179-SPK-014A"),
    ("1x12 male header, 2.54 mm, round pins", "Screw-machine (round) pins", "Solders to the Thing Plus's left column", 1,
     "Samtec", "TS-112-T-A", "U1", "", "200-TS112TA"),
    ("1x16 male header, 2.54 mm, round pins", "Screw-machine (round) pins", "Solders to the Thing Plus's right column", 1,
     "Samtec", "TS-116-T-A", "U1", "", "200-TS116TA"),
    ("LiPo battery, 3.7 V 350 mAh", "JST PH 2-pin, + on pin 1, protection circuit, fits between the pots",
     "Battery; rides in the middle layer, between the pots", 1, "Adafruit", "2750", "BT (plugs into J2)",
     "Check polarity before plugging in. Mouser ships lithium cells by ground only", "485-2750"),
    ("Round spacer, 16 mm, M3 clearance, aluminium, 6 mm dia", "Unthreaded, 3.2 mm bore", "Lid to carrier: 4 corners + 2 touch screws", 6,
     "RAF Electronic Hardware", "M0545-3-AL", "MH1-MH4, TCH1, TCH2", "Mouser stock is modest (181): check before ordering kits",
     "761-M0545-3-AL"),
    ("Round spacer, 5 mm, M3 clearance, aluminium, 6 mm dia", "Unthreaded, 3.2 mm bore", "Carrier to base", 4,
     "RAF Electronic Hardware", "M0534-3-AL", "MH1-MH4", "Not the M2.5 M0504-25-AL, which looks the same", "761-M0534-3-AL"),
    ("M3 x 30 mm pan Phillips screw, nylon, black", "", "Clamps each corner of the case", 4, "Essentra", "50M030050P030B", "MH1-MH4",
     "Mouser's search finds no steel M3 x 30; steel from a hardware store works too", "144-50M030050P030B"),
    ("Hex standoff, M3 x 10 mm, female-female, nickel-plated brass", "5 mm across flats", "Back corners: nut and rear foot", 2,
     "Wurth Elektronik", "970100354", "MH1-MH4 (back two)", "The screw goes 4.2 mm in. Gives the case its forward rake", "710-970100354"),
    ("M3 hex nut, zinc-plated steel", "5.5 mm across flats, 2.4 mm thick", "Front corners, under the base", 2,
     "Keystone", "4708", "MH1-MH4 (front two)", "Silver, to match the back standoffs. A nylock nut works too, if you have one", "534-4708"),
    ("M3 x 22 mm pan screw, brass", "Brass or other bare metal: it's the touch point", "Touch points, through the lid's touch pads", 2,
     "", "", "TCH1, TCH2", "Not found at Mouser: hardware store. Or 25 mm with 3 mm of washers under the head", ""),
    ("M3 hex nut, brass", "", "Under the carrier, for the touch screws", 2, "", "", "TCH1, TCH2", "Not found at Mouser: hardware store", ""),
    ("M2.5 x 16 mm pan Phillips screw, nylon, black", ">= 13 mm: Thing Plus 1.6 + gap ~7.8 + carrier 1.6 + nut 1.8",
     "Optional Thing Plus hold-down (the sockets hold it already)", 3, "Essentra", "50M025045P016B", "U1 mounting",
     "The tip goes into the base's matching hole and stops short of its far side. Thing Plus holes are 3.05 mm", "144-50M025045P016B"),
    ("Round spacer, 7 mm, M2.5 clearance, aluminium", "Height = Thing Plus underside above the carrier: just under 8 mm, untrimmed pins",
     "Between the Thing Plus and the carrier", 3, "RAF Electronic Hardware", "M0506-25-AL", "U1 mounting",
     "An 8 mm spacer would lift the Thing Plus off its pins: washers make up the rest", "761-M0506-25-AL"),
    ("Flat washer, M2.5, thin", "~0.25-0.5 mm thick, fits under the spacer", "Shims the 7 mm spacer to the gap", 6,
     "Essentra", "16M02556080", "U1 mounting", "Thickness not listed at Mouser: use one or two per spacer, whichever fills the gap",
     "144-16M02556080"),
    ("M2.5 hex nut", "", "Under the carrier, for the hold-down screws", 3, "Essentra", "04M025045HN", "U1 mounting", "", "144-04M025045HN"),
    ("Lid and base PCBs", "Two more copies of this carrier PCB", "Case top and bottom", 0, "", "", "",
     "Come with the board order (OSH Park makes 3); not bought separately", ""),
]

COLUMNS = ["References", "Qty", "Value", "Description", "Requirements", "Purpose", "Manufacturer", "MPN", "Footprint", "Notes",
           "Mouser #", "Mouser notes", "LCSC #", "LCSC part", "LCSC price (qty 1, USD)", "LCSC stock", "LCSC notes"]
REF, QTY, DESC, PURPOSE, MOUSER_PN = (COLUMNS.index(c) for c in ("References", "Qty", "Description", "Purpose", "Mouser #"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--order", type=int, metavar="BOARDS", help="also write a Mouser order CSV for this many boards")
    boards = ap.parse_args().order
    net = pathlib.Path(tempfile.mkdtemp()) / "net.xml"
    subprocess.run([KICAD_CLI, "sch", "export", "netlist", "--format", "kicadxml", "-o", str(net), str(HERE / "cassette-carrier.kicad_sch")],
                   check=True, capture_output=True)
    groups = defaultdict(list)
    for c in ET.parse(net).getroot().find("components"):
        ref = c.get("ref")
        if ref.startswith(SKIP):
            continue
        groups[(c.findtext("value"), c.findtext("footprint") or "None")].append(ref)

    def key(item):                                            # sort by first reference letter group, then number
        first = item[1][0]
        return (first.rstrip("0123456789"), int("".join(ch for ch in first if ch.isdigit()) or 0))
    rows, missing = [], []
    for (value, fp), refs in sorted(groups.items(), key=lambda kv: key((kv[0], sorted(kv[1], key=lambda r: (len(r), r))))):
        refs.sort(key=lambda r: (r.rstrip("0123456789"), int("".join(ch for ch in r if ch.isdigit()))))
        spec = PARTS.get(f"{value}|{fp}")
        if not spec:
            missing.append(f"{value}|{fp}"); spec = ("",) * 6
        desc, req, purpose, mfr, mpn, note = spec
        l = LCSC.get(f"{value}|{fp}", ("", "", None, None, "not stocked at LCSC" if mpn else ""))
        m = MOUSER.get(f"{value}|{fp}", ("", "not stocked at Mouser"))
        rows.append([", ".join(refs), len(refs), value, desc, req, purpose, mfr, mpn, fp.split(":")[-1], note, m[0], m[1],
                     l[0], l[1], "" if l[2] is None else l[2], "" if l[3] is None else l[3], l[4]])
    for desc, req, purpose, qty, mfr, mpn, ref, note, mouser in EXTRA:
        rows.append([ref, qty, "", desc, req, purpose, mfr, mpn, "", note, mouser, "", "", "", "", "", ""])
    out = HERE / "cassette-carrier-bom.csv"
    with open(out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(COLUMNS)
        w.writerows(rows)
    print(f"wrote {out.name}: {len(rows)} lines")
    if missing:
        print("no purchasing entry for:", *missing, sep="\n  ")
    if boards:
        write_order(rows, boards)


def order_qty(part, need):
    """Cheapest quantity >= need: buying up to the next price break often costs no more (10 caps for the price of 4)."""
    step = max(int(part.get("Mult") or 1), 1)
    start = max(need, int(part.get("Min") or 1))
    options = {math.ceil(start / step) * step} | {b["Quantity"] for b in part.get("PriceBreaks", []) if b["Quantity"] >= start}
    cost = lambda q: q * (mouser_search.unit_price(part, q) or math.inf)
    return min(options, key=lambda q: (round(cost(q), 2), -q))


def write_order(rows, boards):
    """Look up every Mouser # live and write a BOM-upload CSV (mouser.com/bom) for this many boards."""
    global mouser_search
    sys.path.insert(0, str(HERE / "tools"))
    import mouser_search
    wanted = {}                                               # Mouser # -> (qty per board, references, source BOM row)
    for r in rows:
        if r[MOUSER_PN] and r[QTY]:
            q, refs, _ = wanted.get(r[MOUSER_PN], (0, [], r))
            wanted[r[MOUSER_PN]] = (q + r[QTY], refs + [r[REF]], r)
    found = {p["MouserPartNumber"]: p for p in mouser_search.part_search(list(wanted))}
    out = HERE / f"cassette-carrier-mouser-order-{boards}x.csv"
    total, problems = 0.0, []
    with open(out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Mouser Part Number", "Quantity", "Customer Part Number", "Unit price (USD)", "Line total (USD)", "Stock",
                    "Lifecycle", "Description", "Purpose"])
        for pn, (per_board, refs, row) in wanted.items():
            p, need = found.get(pn), per_board * boards
            if not p:
                problems.append(f"{pn}: not found at Mouser"); continue
            q = order_qty(p, need)
            unit = mouser_search.unit_price(p, q)
            lifecycle = p.get("LifecycleStatus") or ""
            if any(word in lifecycle.lower() for word in ("end of life", "obsolete", "not recommended", "discontinued")):
                problems.append(f"{pn}: {lifecycle} - find a replacement Mouser will keep stocking")
            if mouser_search.stock(p) < q:
                problems.append(f"{pn}: need {q}, Mouser has {mouser_search.stock(p)}")
            total += q * (unit or 0)
            w.writerow([pn, q, "; ".join(refs)[:30], unit, round(q * (unit or 0), 2), mouser_search.stock(p),
                        lifecycle, row[DESC], row[PURPOSE]])
    print(f"wrote {out.name}: {len(wanted)} lines, about ${total:.2f} before shipping and tax")
    for msg in problems:
        print("  CHECK", msg)
    print("  buy elsewhere:", *[f"{r[QTY] * boards} x {r[DESC]}" for r in rows if not r[MOUSER_PN] and r[QTY]], sep="\n    ")


if __name__ == "__main__":
    main()
