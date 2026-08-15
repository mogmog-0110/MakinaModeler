"""Ask POV-Ray what finish{brilliance} does to the diffuse term, instead of guessing.

The public reference says brilliance makes a surface "shine more at glancing angles" and that
1.0 is plain Lambert. That describes the effect, not the formula: an exponent on N.L, a
multiplier on it, or something applied after the light color would all read that way. The
renderer chose pow(N.L, brilliance) from the description; when pingu.pov's brilliance 0.9 was
added on both sides the picture got farther from POV (mean 2.50 -> 2.62), which is the sign the
choice was wrong. So this asks.

A flat plane facing the camera, `finish{ambient 0 diffuse 1 brilliance b}`, one white point
light at a known angle theta off the normal and far enough that it lands parallel: the centre
pixel is diffuse * f(cos theta, b) and nothing else, so sweeping theta and b reports f directly.

Run: python spike/pov_brilliance_probe.py
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pov_filter_probe import render_centre  # noqa: E402  (the BMP reader and POV runner)


def probe(theta_deg, brilliance):
    t = math.radians(theta_deg)
    # The plane faces the camera at -z, so the light goes out on the -z side, theta off the
    # normal, and far enough that its rays land parallel to within a pixel.
    lx, lz = math.sin(t) * 1000.0, -math.cos(t) * 1000.0
    body = (
        f"light_source{{ <{lx:.3f},0,{lz:.3f}> color rgb 1 }}\n"
        "plane{ z, 0 pigment{ color rgb 1 } "
        f"finish{{ ambient 0 diffuse 1 brilliance {brilliance} }} }}\n"
    )
    r, _, _ = render_centre(f"bril_{int(theta_deg)}_{brilliance}", body)
    return r


def main():
    print("theta   cos    b     POV      cos^b    cos*b   (cos^b close?)")
    for b in (1.0, 0.9, 0.5, 2.0):
        for theta in (0, 30, 60, 75):
            c = math.cos(math.radians(theta))
            pov = probe(theta, b)
            print(f"{theta:5}  {c:.3f}  {b:.1f}  {pov:.4f}   {c ** b:.4f}   {min(1.0, c * b):.4f}"
                  f"   {'yes' if abs(pov - c ** b) < 0.01 else 'NO'}")


if __name__ == "__main__":
    main()
