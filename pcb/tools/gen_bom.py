#!/usr/bin/env python3
"""Write cassette-carrier-bom.csv: one line per distinct part to buy, grouped from the schematic.

Reference designators, values and footprints come from the schematic's netlist, so the counts can't drift
from the design. What to BUY (spec, manufacturer, part number) lives in PARTS below, keyed by
"value|footprint". Board-only items (touch pads, mounting holes, the cut jumper) are skipped.

Usage:  python3 tools/gen_bom.py            (run from pcb/; needs kicad-cli 7, path below)
"""
import csv, pathlib, subprocess, tempfile
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
    "2.2u|cassette:C_DCBlock_Dual": ("2.2 uF non-polarised, film box (PET/PP) or radial MLCC, >=25 V, lead pitch 5.0-15.0 mm, under 10 mm tall", "", "",
                                     "Audio DC blocker; the footprint takes 5.0/7.5/10.0/12.5/15.0 mm lead spacing. Film preferred for audio"),
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
    "1x12 socket|None": ("1x12 female header, 2.54 mm, 3 mm insulation height (low profile), round-hole turned pin", "HCTL", "PM254-1-12-Z-3.0-C",
                         "LCSC C5156451; carries the Thing Plus left column"),
    "1x16 socket|None": ("1x16 female header, 2.54 mm, 3 mm insulation height (low profile), round-hole turned pin", "HCTL", "PM254-1-16-Z-3.0-C",
                         "LCSC C5159943; carries the Thing Plus right column"),
    "Thing Plus RA6M5|cassette:ThingPlus": ("SparkFun Thing Plus RA6M5, long variant, with male headers fitted", "SparkFun", "",
                                            "The MCU board; not made by this project. Male-header pin tails must suit a 3 mm socket"),
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
# Bought once per board but not on the schematic: knobs and hardware.
EXTRA = [
    ("Knob, black, 24.1 mm dia x 10.3 mm tall, round, with a white indicator notch", 2, "Same Sky", "SPK-013A", "RV1, RV2",
     "Check that the bore matches the RV112FF-40 shaft"),
    ("Screws + spacers/bushings for the Thing Plus mounting holes (3.05 mm holes), standoff about 5.5 mm", 4, "", "", "U1 mounting",
     "Your choice: spacer height = 3 mm socket + 2.5 mm header body"),
    ("M3 screws/standoffs for the case-mounting holes", 4, "", "", "MH1-MH4", "Length depends on your case"),
]


def main():
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
        rows.append([", ".join(refs), len(refs), value, spec[0], spec[1], spec[2], fp.split(":")[-1], spec[3], l[0], l[1], "" if l[2] is None else l[2], "" if l[3] is None else l[3], l[4]])
    for desc, qty, mfr, mpn, ref, note in EXTRA:
        rows.append([ref, qty, "", desc, mfr, mpn, "", note, "", "", "", "", "not checked" if mpn else ""])
    out = HERE / "cassette-carrier-bom.csv"
    with open(out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["References", "Qty", "Value", "Buy (spec)", "Manufacturer", "MPN", "Footprint", "Notes",
                    "LCSC #", "LCSC part", "LCSC price (qty 1, USD)", "LCSC stock", "LCSC notes"])
        w.writerows(rows)
    print(f"wrote {out.name}: {len(rows)} lines")
    if missing:
        print("no purchasing entry for:", *missing, sep="\n  ")


if __name__ == "__main__":
    main()
