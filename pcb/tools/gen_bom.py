#!/usr/bin/env python3
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

# (what to buy, manufacturer, MPN, note).  Blank MPN = generic part: buy to the spec in the first column.
PARTS = {
    f"6.8n|{C_DISC}": ("6.8 nF, C0G/NP0 (or film), 50 V, radial, 5 mm lead pitch, 5% or better", "", "",
                       "Sets the audio low-pass corner with R1/R2 (1k): tolerance and dielectric matter, avoid X7R here"),
    "2.2u|cassette:C_DCBlock_Dual": ("2.2 uF non-polarised, film box (PET/PP) or radial MLCC, >=25 V, lead pitch 5.0-15.0 mm, under 12 mm tall", "", "",
                                     "Audio DC blocker; the footprint takes 5.0/7.5/10.0/12.5/15.0 mm lead spacing. Film preferred for audio. "
                                     "The lid is 15 mm up; 2.2 uF film boxes are 10.5-12.5 mm tall, lowest at 15 mm pitch"),
    f"100n|{C_DISC}": ("100 nF, X7R, 50 V, radial, 5 mm lead pitch", "", "", "Pot wiper filter (4x) and general decoupling"),
    "10u|Capacitor_THT:CP_Radial_D5.0mm_P2.00mm": ("10 uF aluminium electrolytic, >=16 V, radial, 5 mm dia, 2.0 mm lead pitch", "", "",
                                                   "Pot supply filter; watch the polarity (long lead = +)"),
    f"10n|{C_DISC}": ("10 nF, C0G/NP0 or X7R, 50 V, radial, 5 mm lead pitch, 10% or better", "", "",
                      "CTSU TSCAP reference cap: needed for touch to work"),
    f"1k|{R_AXIAL}": ("1 kOhm 1/8 W (or 1/4 W) axial, 1% metal film", "", "", "Audio low-pass resistor"),
    "22|cassette:R_Axial_DIN0204_P7.62mm_CutJumper": ("22 Ohm 1/8 W axial, 1% metal film", "", "",
                                                     "Optional pot-supply filter resistor; the footprint's jumper shorts it until you cut it"),
    f"560|{R_AXIAL}": ("560 Ohm 1/8 W (or 1/4 W) axial, 1% metal film", "", "", "Series resistor for each touch pad (CTSU)"),
    "RV112FF-40-25A-0B10K|cassette:RV112FF-40": ("Dual 10 kOhm endless rotary potentiometer, 25 mm plastic shaft", "Taiwan Alpha",
                                                 "RV112FF-40-25A-0B10K", "Shaft length is in the part number; the footprint is the same for other lengths"),
    "STX-3120-3B|cassette:Kycon_STX-3120-3B": ("3.5 mm stereo jack, right angle, all plastic", "Kycon", "STX-3120-3B",
                                               "Colour variants exist; check the suffix at the vendor"),
    "EG1224|Button_Switch_THT:SW_E-Switch_EG1224_SPDT_Angled": ("Slide switch, SPDT, right angle, 0.1 in pin pitch", "E-Switch", "EG1224",
                                                                "Power switch in the battery line (J2 to the Thing Plus's VBAT pin)"),
    "LiPo JST-PH|Connector_JST:JST_PH_S2B-PH-K_1x02_P2.00mm_Horizontal": ("JST PH 2-pin header, 2.0 mm, right angle (side entry), through hole", "JST",
                                                                          "S2B-PH-K-S(LF)(SN)", "LiPo battery socket; rated 2 A"),
    "1x12 socket|None": ("1x12 female header, 2.54 mm, low profile (2.4-3 mm insulation height), round-hole turned pin", "HCTL", "PM254-1-12-Z-3.0-C",
                         "Carries the Thing Plus left column. Turned-pin sockets want round pins: see the male header lines"),
    "1x16 socket|None": ("1x16 female header, 2.54 mm, low profile (2.4-3 mm insulation height), round-hole turned pin", "HCTL", "PM254-1-16-Z-3.0-C",
                         "Carries the Thing Plus right column. Turned-pin sockets want round pins: see the male header lines"),
    "Thing Plus RA6M5|cassette:ThingPlus": ("SparkFun Thing Plus RA6M5 (sold without headers)", "SparkFun", "WRL-24243",
                                            "The MCU board; not made by this project. Solder the round-pin male headers (below) to it"),
}
# LCSC / JLCPCB loose-parts picks, looked up 2026-09-21 (price = USD at qty 1, stock as listed then; recheck before ordering).
# key -> (LCSC #, LCSC part, price, stock, note).  No entry = not stocked there.
LCSC = {
    f"6.8n|{C_DISC}": ("C390177", "KYET PE682J2A0501", 0.0437, 2400, "Metallised polyester film 100 V 5%, 5 mm pitch. Only C0G found is C454391 (TORCH), stock 19, $0.454"),
    "2.2u|cassette:C_DCBlock_Dual": ("C53070641", "DGCX MPE225J2AB070B200ZR", 0.35, 750, "Polyester film 100 V 10%, 7.5 mm pitch. Alts: C3019042 KYET 5 mm $0.578; C46638925 WALSON 15 mm $0.203. Check height < 10 mm"),
    f"100n|{C_DISC}": ("C5632426", "KNSCHA 178MU0002", 0.0138, 5076, "X7R 50 V 10%, 5.08 mm pitch"),
    "10u|Capacitor_THT:CP_Radial_D5.0mm_P2.00mm": ("C55265437", "TOPAZ ESS1V100M050700B", 0.0093, 3244, "35 V, 5 x 7 mm, 2 mm pitch"),
    f"10n|{C_DISC}": ("C454414", "TORCH CC4-0805-CG-50V-0.01uF-J", 0.1285, 6016, "C0G 5%, 5.08 mm pitch. Cheaper X7R: C5632432 KNSCHA $0.0196"),
    f"1k|{R_AXIAL}": ("C1371015", "YAGEO MFR-12FTF52-1K", 0.0045, 3000, "1% metal film, 1/6 W, 1.9 x 3.4 mm"),
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
    f"6.8n|{C_DISC}": ("810-FG28C0G1H682JNT6", "TDK FG28C0G1H682JNT06: C0G 5% 50 V, 5 mm pitch"),
    "2.2u|cassette:C_DCBlock_Dual": ("871-B32522C225J", "TDK B32522C0225J000: PET film 5% 63 V, 15 mm pitch, 18 x 5 x 10.5 mm (lowest 2.2 uF film found)"),
    f"100n|{C_DISC}": ("810-FG28X7R1H104KNT0", "TDK FG28X7R1H104KNT00: X7R 10% 50 V, 5 mm pitch"),
    "10u|Capacitor_THT:CP_Radial_D5.0mm_P2.00mm": ("140-SS100M1V0505P", "Lelon SS100M1VBK-0505P: 35 V, 5 x 5 mm. Check the 2.0 mm lead pitch"),
    f"10n|{C_DISC}": ("810-FG28C0G1H103JNT6", "TDK FG28C0G1H103JNT06: C0G 5% 50 V, 5 mm pitch"),
    f"1k|{R_AXIAL}": ("603-MFR-12FTF52-1K", "YAGEO MFR-12FTF52-1K: 1% metal film, 1/6 W, 0204 body"),
    "22|cassette:R_Axial_DIN0204_P7.62mm_CutJumper": ("603-MFR-12FTF52-22R", "YAGEO MFR-12FTF52-22R: 1% metal film, 1/6 W, 0204 body"),
    f"560|{R_AXIAL}": ("603-MFR-12FTF52-560R", "YAGEO MFR-12FTF52-560R: 1% metal film, 1/6 W, 0204 body"),
    "RV112FF-40-25A-0B10K|cassette:RV112FF-40": ("317-1240FF-25A-B10K", ""),
    "STX-3120-3B|cassette:Kycon_STX-3120-3B": ("806-STX-3120-3B", ""),
    "EG1224|Button_Switch_THT:SW_E-Switch_EG1224_SPDT_Angled": ("612-EG1224", ""),
    "LiPo JST-PH|Connector_JST:JST_PH_S2B-PH-K_1x02_P2.00mm_Horizontal": ("306-S2BPHKSLFSNPP", ""),
    "1x12 socket|None": ("200-SL112TT12", "Samtec SL-112-TT-12: 2.41 mm insulation height (HCTL's is 3.0)"),
    "1x16 socket|None": ("200-SL116TT12", "Samtec SL-116-TT-12: 2.41 mm insulation height (HCTL's is 3.0)"),
}
# Case stack, top to bottom: lid PCB / 15 mm spacer / carrier / 10 mm spacer / base PCB, clamped by one M3 bolt per corner.
# 15 mm: tallest parts (the Thing Plus's own JST and Qwiic sockets) reach ~11.5 mm; the lid top at 16.6 mm leaves ~1.2 mm
# under the knobs (25 mm shaft). 10 mm: a 5 mm LiPo + ~1 mm foam tape + ~2 mm of trimmed leads, with room to spare.
# Bolt: 3 x 1.6 mm PCB + 15 + 10 = 29.8 mm grip + 4 mm nylock = M3 x 35, leaving ~1 mm of thread past the nut.
# (description, qty per board, manufacturer, MPN, references, note, Mouser #)
EXTRA = [
    ("Knob, black, 24.1 mm dia x 10.3 mm tall, round, with a white indicator notch", 2, "Same Sky", "SPK-013A", "RV1, RV2",
     "Check that the bore matches the RV112FF-40 shaft", "179-SPK-013A"),
    ("1x12 male header, 2.54 mm, round (screw-machine) pins", 1, "Samtec", "TS-112-T-A", "U1",
     "Soldered to the Thing Plus's left column; round pins suit the turned-pin sockets", "200-TS112TA"),
    ("1x16 male header, 2.54 mm, round (screw-machine) pins", 1, "Samtec", "TS-116-T-A", "U1",
     "Soldered to the Thing Plus's right column; round pins suit the turned-pin sockets", "200-TS116TA"),
    ("LiPo battery, 3.7 V 1200 mAh, 34 x 62 x 5 mm, JST PH 2-pin, with protection circuit", 1, "Adafruit", "258", "BT (plugs into J2)",
     "Rides under the carrier, foam-taped to the base. Check polarity (+ on J2 pin 1) before plugging in. "
     "Mouser ships lithium cells by ground only", "485-258"),
    ("Round spacer, unthreaded, 15 mm long, 3.2 mm bore (M3), 4.5 mm OD, aluminium", 4, "RAF Electronic Hardware", "M0514-3-AL",
     "MH1-MH4", "Lid to carrier", "761-M0514-3-AL"),
    ("Round spacer, unthreaded, 10 mm long, 3.2 mm bore (M3), aluminium", 4, "RAF Electronic Hardware", "M0509-3-AL",
     "MH1-MH4", "Carrier to base; room for a 5 mm LiPo", "761-M0509-3-AL"),
    ("M3 x 35 mm hex socket cap screw (ISO 4762 / DIN 912), stainless A2", 4, "", "", "MH1-MH4",
     "Mouser has no metal M3 socket caps; any hardware supplier. Fallback at Mouser: Essentra 50M030050P035 pan Phillips", ""),
    ("M3 nylon-insert lock nut (nylock, DIN 985), stainless A2", 4, "", "", "MH1-MH4",
     "Mouser has no free-running M3 nylocks (PEM's locking nuts are press-in); any hardware supplier", ""),
    ("Adhesive rubber foot, taller than 6 mm, placed clear of the four nuts", 4, "", "", "base",
     "Nut + bolt tail stand ~5.2 mm proud of the base. NOT CHOSEN YET: e.g. 3M Bumpon SJ-5023 (pad of 36) - check height", ""),
    ("M2.5 x 12 mm screw, for the Thing Plus's own mounting holes (3.05 mm)", 4, "Essentra", "50M025045P012", "U1 mounting",
     "Optional: the sockets hold the board. Pan Phillips; Mouser has no M2.5 socket caps either", "144-50M025045P012"),
    ("Round spacer, unthreaded, 5 mm long, M2.5 (2.7 mm bore), aluminium", 4, "RAF Electronic Hardware", "M0504-25-AL", "U1 mounting",
     "Between the Thing Plus and the carrier. Check: socket (2.41) + male header body; 5 mm assumes a ~2.5 mm body", "761-M0504-25-AL"),
    ("M2.5 hex nut", 4, "Essentra", "04M025045HN", "U1 mounting", "Under the carrier", "144-04M025045HN"),
    ("Lid and base: two more of this carrier board's own PCB order (OSH Park makes 3)", 0, "", "", "",
     "Not bought separately", ""),
]

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
            missing.append(f"{value}|{fp}"); spec = ("", "", "", "")
        l = LCSC.get(f"{value}|{fp}", ("", "", None, None, "not stocked at LCSC" if spec[2] else ""))
        m = MOUSER.get(f"{value}|{fp}", ("", "not stocked at Mouser"))
        rows.append([", ".join(refs), len(refs), value, spec[0], spec[1], spec[2], fp.split(":")[-1], spec[3], m[0], m[1],
                     l[0], l[1], "" if l[2] is None else l[2], "" if l[3] is None else l[3], l[4]])
    for desc, qty, mfr, mpn, ref, note, mouser in EXTRA:
        rows.append([ref, qty, "", desc, mfr, mpn, "", note, mouser, "", "", "", "", "", ""])
    out = HERE / "cassette-carrier-bom.csv"
    with open(out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["References", "Qty", "Value", "Buy (spec)", "Manufacturer", "MPN", "Footprint", "Notes",
                    "Mouser #", "Mouser notes", "LCSC #", "LCSC part", "LCSC price (qty 1, USD)", "LCSC stock", "LCSC notes"])
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
    wanted = {}                                               # Mouser # -> (qty per board, references)
    for r in rows:
        if r[8] and r[1]:
            q, refs = wanted.get(r[8], (0, []))
            wanted[r[8]] = (q + r[1], refs + [r[0]])
    found = {p["MouserPartNumber"]: p for p in mouser_search.part_search(list(wanted))}
    out = HERE / f"cassette-carrier-mouser-order-{boards}x.csv"
    total, problems = 0.0, []
    with open(out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Mouser Part Number", "Quantity", "Customer Part Number", "Unit price (USD)", "Line total (USD)", "Stock"])
        for pn, (per_board, refs) in wanted.items():
            p, need = found.get(pn), per_board * boards
            if not p:
                problems.append(f"{pn}: not found at Mouser"); continue
            q = order_qty(p, need)
            unit = mouser_search.unit_price(p, q)
            if mouser_search.stock(p) < q:
                problems.append(f"{pn}: need {q}, Mouser has {mouser_search.stock(p)}")
            total += q * (unit or 0)
            w.writerow([pn, q, "; ".join(refs)[:30], unit, round(q * (unit or 0), 2), mouser_search.stock(p)])
    print(f"wrote {out.name}: {len(wanted)} lines, about ${total:.2f} before shipping and tax")
    for msg in problems:
        print("  CHECK", msg)
    print("  buy elsewhere:", *[f"{r[1] * boards} x {r[3]}" for r in rows if not r[8] and r[1]], sep="\n    ")


if __name__ == "__main__":
    main()
