// Tilted desk stand for the Waveshare ESP32-S3-Knob-Touch-LCD-1.8.
// The cup grips only the black base: the blue ring above it is the encoder
// and must turn freely. Prints flat as modelled, no supports.

/* [Device] */
device_d     = 66.0;  // Waveshare drawing: 66 mm
base_band_h  = 9.0;   // black base below the rotating ring (22 mm total)
clearance    = 0.4;   // radial, per side

/* [Stand] */
tilt         = 25;    // screen tilt back from horizontal
wall         = 2.4;
floor_t      = 3.0;
lip_h        = 5.0;   // must stay below base_band_h so the ring clears
ground_gap   = 3.0;   // stand height under the cup's lowest point
port_angle   = 90;    // 90 = ports face the back; turn to match your UI's "up"
port_span    = 120;   // USB-C, jack, power switch, BOOT and mic span ~75°
lead_in      = 0.8;   // chamfer at the cup mouth

$fn = 160;

assert(lip_h < base_band_h - 1, "lip would rub the rotating ring");

r_pocket = device_d / 2 + clearance;
r_outer  = r_pocket + wall;
cup_h    = floor_t + lip_h;
lift     = r_outer * sin(tilt) + ground_gap;

module tilted() {
    translate([0, 0, lift]) rotate([tilt, 0, 0]) children();
}

module cup_solid() {
    tilted() cylinder(r = r_outer, h = cup_h);
}

module body() {
    hull() {
        cup_solid();
        linear_extrude(0.01) projection() cup_solid();
    }
}

module pocket() {
    tilted() translate([0, 0, floor_t]) {
        cylinder(r = r_pocket, h = 60);
        translate([0, 0, lip_h - lead_in + 0.001])
            cylinder(r1 = r_pocket, r2 = r_pocket + lead_in + 0.5, h = lead_in + 0.5);
    }
}

// Opens the wall and everything above the floor in the ports' direction so
// USB-C plugs and the headphone jack go straight in.
module port_cutout() {
    tilted() translate([0, 0, floor_t])
        rotate([0, 0, port_angle - port_span / 2])
            rotate_extrude(angle = port_span) square([80, 60]);
}

difference() {
    body();
    pocket();
    port_cutout();
}
