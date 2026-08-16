"""Ask POV-Ray what finish{specular} does with roughness, instead of guessing.

The renderer's highlight is a Gaussian in the half-angle, exp(-(acos(N.H) / roughness)^2)
(scene_finish.hlsl). That was measured once, at roughness 0.6875 on a checker scene, and held.
The arm fixture (specular 0.15, roughness 0.7656, a tan diffuse) then disagreed with POV at
mean 11.9 with the diff bisecting to the specular term -- so the shape is wrong somewhere in
the rough end, and this asks where.

A plane facing the camera, `finish{ambient 0 diffuse 0 specular 1 roughness r}`, pigment white
(the highlight is white regardless), one white point light theta off the normal in the x-z
plane, far enough to land parallel. The centre pixel then reads the highlight alone: the half
vector between the light and the eye sits at theta/2 off the normal, so sweeping theta reads
f(alpha = theta/2, r) directly. Compared against the Gaussian the shader uses and against the
Blinn-Phong power that was the first (wrong) attempt.

Run: python spike/pov_specular_probe.py
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pov_filter_probe import render_centre, to_linear  # noqa: E402


def probe(theta_deg, roughness):
    t = math.radians(theta_deg)
    lx, lz = math.sin(t) * 1000.0, -math.cos(t) * 1000.0
    body = (
        f"light_source{{ <{lx:.3f},0,{lz:.3f}> color rgb 1 }}\n"
        "plane{ z, 0 pigment{ color rgb 1 } "
        f"finish{{ ambient 0 diffuse 0 specular 1 roughness {roughness} }} }}\n"
    )
    r, _, _ = render_centre(f"spec_{int(theta_deg)}_{roughness}", body)
    return to_linear(r)


def gaussian(alpha, r):
    return math.exp(-(alpha / r) ** 2)


def power(alpha, r):
    return math.cos(alpha) ** (1.0 / r)


def main():
    print("roughness  theta  alpha   POV     gauss   pow(1/r)")
    worst = {}
    for r in (0.05, 0.16, 0.5, 0.6875, 0.7656, 1.0):
        for theta in (0, 10, 20, 30, 45, 60, 75, 90):
            alpha = math.radians(theta) / 2.0
            pov = probe(theta, r)
            g, p = gaussian(alpha, r), power(alpha, r)
            print(f"{r:9.4f}  {theta:5}  {alpha:.3f}  {pov:.4f}  {g:.4f}  {p:.4f}")
            worst[('gauss', r)] = max(worst.get(('gauss', r), 0.0), abs(pov - g))
            worst[('pow', r)] = max(worst.get(('pow', r), 0.0), abs(pov - p))
    print()
    print("largest |POV - model| per roughness")
    for r in (0.05, 0.16, 0.5, 0.6875, 0.7656, 1.0):
        print(f"  r={r:<7}  gauss {worst[('gauss', r)]:.4f}   pow {worst[('pow', r)]:.4f}")


if __name__ == "__main__":
    main()
