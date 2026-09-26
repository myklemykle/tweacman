#!/usr/bin/env python3
"""
Check the exported schematic netlist against the connections we designed.

    kicad-cli sch export netlist --format kicadxml -o /tmp/net.xml cassette-carrier.kicad_sch
    python3 tools/check_netlist.py /tmp/net.xml

KiCad 7's command line has no ERC, so this is the automated check: it prints every difference between the netlist and
the table below, and exits non-zero if there is one.
"""
import sys
import xml.etree.ElementTree as ET

# pin numbers: OLED 1=GND 2=VCC 3=SCL 4=SDA; mic 1=3V 2=GND 3=BCLK 4=DOUT 5=LRCLK 6=SEL; switch 2=common; J2 (JST-PH) 1=+ 2=GND; Thing Plus pads (see gen_board.TP_LEFT/TP_RIGHT), RV112FF 1=B 2=PWR 3=A 4=GND 5,6=CASE, jack 1=S 2=ring 5=tip
EXPECTED = {
    "DAC_L": {("U1", "24"), ("R1", "1")},
    "AUDIO_L": {("C3", "2"), ("J1", "5")},
    "DAC_R": {("U1", "23"), ("R2", "1")},
    "AUDIO_R": {("C4", "2"), ("J1", "2")},
    "POT1_A": {("U1", "22"), ("RV1", "3"), ("C5", "1")},
    "POT1_B": {("U1", "21"), ("RV1", "1"), ("C6", "1")},
    "POT2_A": {("U1", "20"), ("RV2", "3"), ("C7", "1")},
    "POT2_B": {("U1", "19"), ("RV2", "1"), ("C8", "1")},
    "VREF": {("U1", "26"), ("R3", "1"), ("JP1", "1")},
    "POT_V": {("R3", "2"), ("JP1", "2"), ("C9", "1"), ("RV1", "2"), ("RV2", "2")},
    "D20": {("U1", "14"), ("R4", "1")},
    "D21": {("U1", "15"), ("R5", "1")},
    "D6": {("U1", "3"), ("C10", "1")},
    "VBAT": {("U1", "12"), ("SW1", "2")},
    "BATT_IN": {("SW1", "1"), ("J2", "1")},
    "GND": {("U1", "25"), ("C1", "2"), ("C2", "2"), ("J1", "1"), ("C5", "2"), ("C6", "2"), ("C7", "2"), ("C8", "2"),
            ("C9", "2"), ("C10", "2"), ("RV1", "4"), ("RV1", "5"), ("RV1", "6"), ("RV2", "4"), ("RV2", "5"), ("RV2", "6"),
            ("J2", "2")},
}
# nets without a label: identified by their pins
UNNAMED = [
    {("R1", "2"), ("C1", "1"), ("C3", "1")},
    {("R2", "2"), ("C2", "1"), ("C4", "1")},
    {("R4", "2"), ("TCH1", "1")},
    {("R5", "2"), ("TCH2", "1")},
]


def main(path):
    root = ET.parse(path).getroot()
    nets = {}
    for net in root.find("nets"):
        name = net.get("name").lstrip("/")
        nodes = {(n.get("ref"), n.get("pin")) for n in net.findall("node")}
        nets[name] = nodes
    problems = 0
    for name, want in EXPECTED.items():
        got = nets.get(name)
        if got is None:
            print(f"MISSING net {name}")
            problems += 1
        elif got != want:
            print(f"DIFF   net {name}: missing {sorted(want - got)} extra {sorted(got - want)}")
            problems += 1
    remaining = [n for k, n in nets.items() if k not in EXPECTED]
    for want in UNNAMED:
        if want not in remaining:
            print(f"MISSING unnamed net with {sorted(want)}")
            problems += 1
    known = set().union(*EXPECTED.values(), *UNNAMED)
    extra = [(k, sorted(n)) for k, n in nets.items() if k not in EXPECTED and n not in UNNAMED and len(n) > 1]
    for k, n in extra:
        print(f"EXTRA  net {k}: {n}")
        problems += 1
    print("netlist OK" if not problems else f"{problems} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
