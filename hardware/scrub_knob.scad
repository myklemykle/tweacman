// SPDX-License-Identifier: CERN-OHL-S-2.0
// Copyright (C) 2026 Mykle Hansen
// Specified by Mykle Hansen, implemented by Claude (Anthropic).

// Scrub knob for the RV112FF-40 endless pots (see ../CLAUDE.md), made for
// smoothly scrubbing audio in SDResampleDemo rather than for setting a
// value once and leaving it.
//
// Two ways to hold it:
//   - the wide, low base disc, spun like a record or a jog wheel - six
//     fingertip divots on its top face let a finger rest on it and push it
//     around without slipping, and its knurled rim can be flicked;
//   - the small tapered "pot knob" in the middle, pinched between finger
//     and thumb for finer moves.
//
// Every measurement is a parameter: open this file in OpenSCAD and use
// Window > Customizer to adjust them with sliders and watch the preview
// update. The Customizer saves tweaked values to scrub_knob.json, not to
// this file, so the defaults below are only a starting point - export
// with that saved parameter set (`make knob` from the repo root does this):
//   openscad --render -p scrub_knob.json -P "design default values" -o scrub_knob.stl scrub_knob.scad
// A single value can also be overridden on the command line with
// -D 'name=value', which wins over both the file and the .json.
//
// Prints as modeled, flat bottom down, with no supports: the knurling
// flutes run vertically, the center knob only narrows going up, the
// divots are bowls opening upward, and the blind shaft bore ends in a
// short 6mm bridge.
//
// The D-shaft bore follows the RV112FF datasheet (../datasheets/RV112FF.pdf):
// 6.0mm shaft, 4.5mm across the flat, and the flat only runs the last F mm
// of the shaft (4.5mm on the 15mm "15A" shaft, 7-12mm on longer ones) - so
// the bore is D-shaped only for its deepest shaft_flat_length and round
// below that. shaft_fit_undersize starts from the value that fit well on
// the crank in ../gemtest.git/hardware/crank.scad. Same caveat as there:
// FDM fits depend on the printer, so print one and adjust.

/* [Base disc] */
// Diameter of the wide lower disc
base_d = 44; // [30:0.5:70]
// Height of the lower disc
base_h = 5; // [3:0.5:12]
// 45-degree chamfer on the disc's top rim
base_top_chamfer = 1; // [0:0.1:3]
// Small chamfer on the bottom rim, to counter first-layer "elephant's foot"
base_bottom_chamfer = 0.5; // [0:0.1:2]
// Number of grip flutes around the disc's rim
base_knurl_count = 48; // [0:1:120]
// Radius of each rim flute
base_knurl_r = 0.8; // [0.2:0.1:2]

/* [Center knob] */
// Diameter where the center knob meets the disc
center_base_d = 12; // [6:0.5:30]
// Diameter at the center knob's top
center_top_d = 10; // [6:0.5:30]
// How far the center knob rises above the disc's top face
center_h = 10; // [3:0.5:25]
// 45-degree chamfer on the center knob's top edge
center_top_chamfer = 0.8; // [0:0.1:3]
// Number of grip flutes around the center knob
center_knurl_count = 18; // [0:1:60]
// Radius of each center knob flute
center_knurl_r = 0.5; // [0.2:0.1:1.5]

/* [Fingertip divots] */
// Number of divots, spaced evenly around the disc
divot_count = 6; // [0:1:12]
// Distance from the knob's axis to each divot's center
divot_ring_r = 14; // [5:0.5:35]
// Diameter of each divot where it meets the disc's top face
divot_d = 12; // [4:0.5:25]
// Depth of each divot at its center (must stay under base_h)
divot_depth = 2; // [0.5:0.1:6]
// Rotates the whole ring of divots
divot_angle_offset = 0; // [0:1:60]

/* [Shaft bore] */
// Nominal shaft diameter
shaft_d = 6;
// Datasheet: 4.5mm across the flat on a 6mm shaft
shaft_flat_depth = 1.5; // [0.5:0.1:2]
// Length of the shaft's flat, measured back from its tip (datasheet "F")
shaft_flat_length = 4.5; // [2:0.5:12]
// How far the shaft reaches into the knob
shaft_insert_depth = 8; // [3:0.5:14]
// Positive tightens the bore, negative loosens it
shaft_fit_undersize = -0.1; // [-0.4:0.05:0.3]
// Chamfer at the bore's mouth, to ease starting it onto the shaft
bore_lead_in = 0.5; // [0:0.1:1.5]

/* [Hidden] */
$fn = 96;
eps = 0.01; // small overlap so touching solids fuse cleanly and preview doesn't flicker
bore_d = shaft_d - shaft_fit_undersize;
divot_sphere_r = (pow(divot_d / 2, 2) + pow(divot_depth, 2)) / (2 * divot_depth); // sphere giving a cap of divot_d wide by divot_depth deep

assert(divot_depth < base_h, "divot_depth must be less than base_h, or the divots break through the bottom");
assert(shaft_insert_depth < base_h + center_h - 1, "shaft_insert_depth would break through the top of the center knob");

// 2D circle with a flat chord flat_depth in from the edge, facing +Y -
// matches a D-shaft cross-section.
module d_shaft_profile(d, flat_depth) {
	difference() {
		circle(d = d);
		translate([-d, d / 2 - flat_depth])
			square([2 * d, d]);
	}
}

// 2D circle with count semicircular notches cut evenly around its edge.
module knurl_profile(d, count, groove_r) {
	difference() {
		circle(d = d);
		if(count > 0) for(i = [0 : count - 1])
			rotate(i * 360 / count)
				translate([d / 2, 0])
					circle(r = groove_r, $fn = 16);
	}
}

// A fluted frustum, d1 wide at the bottom and d2 at the top, with
// 45-degree chamfers on its bottom and top rims. linear_extrude's scale
// tapers the knurled profile along with the body, so the flutes follow
// the slope of a tapered knob all the way up. The chamfers are cut by
// intersecting with a plain chamfered frustum.
module knurled_frustum(d1, d2, h, count, groove_r, bottom_chamfer, top_chamfer) {
	function r_at(z) = d1 / 2 + (d2 - d1) / 2 * z / h;
	intersection() {
		linear_extrude(height = h, scale = d2 / d1)
			knurl_profile(d1, count, groove_r);
		rotate_extrude()
			polygon([
				[0, 0],
				[d1 / 2 - bottom_chamfer, 0],
				[r_at(bottom_chamfer), bottom_chamfer],
				[r_at(h - top_chamfer), h - top_chamfer],
				[d2 / 2 - top_chamfer, h],
				[0, h]
			]);
	}
}

// Fingertip divots: each one is the cap of a sphere, sunk so the cap is
// exactly divot_d across at the disc's top face and divot_depth deep in
// the middle - a shallow bowl with no sharp floor for a fingertip to feel.
module divots() {
	if(divot_count > 0) for(i = [0 : divot_count - 1])
		rotate([0, 0, divot_angle_offset + i * 360 / divot_count])
			translate([divot_ring_r, 0, base_h + divot_sphere_r - divot_depth])
				sphere(r = divot_sphere_r, $fn = 96);
}

// Blind bore up from the bottom face: round for most of its depth, then
// D-shaped for the last shaft_flat_length where the shaft's flat is, plus
// a small lead-in chamfer at the mouth.
module shaft_bore() {
	flat_len = min(shaft_flat_length, shaft_insert_depth);
	translate([0, 0, -1])
		cylinder(d = bore_d, h = shaft_insert_depth - flat_len + 1 + eps);
	translate([0, 0, shaft_insert_depth - flat_len])
		linear_extrude(height = flat_len)
			d_shaft_profile(bore_d, shaft_flat_depth);
	translate([0, 0, -eps])
		cylinder(d1 = bore_d + 2 * bore_lead_in, d2 = bore_d, h = bore_lead_in + eps);
}

module scrub_knob() {
	difference() {
		union() {
			difference() {
				knurled_frustum(base_d, base_d, base_h, base_knurl_count, base_knurl_r,
					base_bottom_chamfer, base_top_chamfer);
				divots();
			}
			translate([0, 0, base_h - eps])
				knurled_frustum(center_base_d, center_top_d, center_h + eps, center_knurl_count,
					center_knurl_r, 0, center_top_chamfer);
		}
		shaft_bore();
	}
}

scrub_knob();
