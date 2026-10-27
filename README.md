TWEACMAN is a two-knob pocket-sized sample scrubbing toy inspired by old-school reel-to-reel tape decks. TWEACMAN plays WAV files from a SD card at speeds from 0.001x to 1000x, forward or backward, or you can twist a knob to scrub your recording back and forth across a virtual play head. It has a few other audio tricks available as well, and a handy Bluetooth interface you can use from your phone.

TWEACMAN is based on the SparkFun Thing Plus RA6M5 and uses two Taiwan Alpha RV112FF dual potentiometers as analog rotary encoders.

This repo includes firmware for TWEACMAN (SDResampleDemo), plus a series of other demo sketches and general-purpose libraries for that board and those pots. There are also KiCad files for the schematic and PCB, and 3D-printable OpenSCAD files for the custom knobs.

TWEACMAN's BLE interface is entirely optional. To use it requires a Bluetooth-enabled browser. Android users can just use Chrome, but on an iPhone neither Safari nor Chrome nor Firefox will work. Sorry, it's an Apple privacy thing. I use Bluefy, a special BLE-enabled browser for iOS. See https://bluefy.app/ for details. In a Bluetooth-enabled browser, navigate to https://mykle.com/wble/tw.htm and press Connect to find your TWEACMAN.

License: see [LICENSE](LICENSE).
