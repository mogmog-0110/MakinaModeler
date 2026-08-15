"""Does POV shade an isosurface the way it shades the same shape as a primitive?

The warp comparison (D-14) writes warped subtrees as isosurfaces. Its outline agreed with the
march to 0.1 px but the shaded picture came out ~12 mean darker even with the warp rate at
zero -- so before blaming the warp, this puts a plain box and the same box as an isosurface
side by side under one light and reads the same pixel of each. If they differ, the isosurface
path itself shades differently in POV and the pixel comparison has to know that.

Run: python spike/pov_iso_probe.py
"""

import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pov_filter_probe import POVINC, POVRAY, SCRATCH  # noqa: E402


def render(name, body, w=200, h=100):
    os.makedirs(SCRATCH, exist_ok=True)
    name = os.path.join(SCRATCH, name)
    with open(name + ".pov", "w") as f:
        f.write(body)
    subprocess.run([POVRAY, "+I" + name + ".pov", "+O" + name + ".bmp", "+W" + str(w), "+H" + str(h),
                    "+FS", "-A", "-D", "+L" + POVINC], stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, check=False)
    data = open(name + ".bmp", "rb").read()
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    stride = (width * 3 + 3) // 4 * 4

    def px(x, y):
        i = offset + (height - 1 - y) * stride + x * 3
        return (data[i + 2], data[i + 1], data[i])
    return px


HEAD = (
    "global_settings{ assumed_gamma 1.0 }\n"
    "background{ color rgb<0,0,0> }\n"
    "camera{ location <0,2,-6> look_at <0,0,0> right <-2,0,0> angle 40 }\n"
    "light_source{ <4,7,-5> color rgb 1 }\n"
)
FINISH = "pigment{color rgb<0.86,0.55,0.24>} finish{ambient 0.1 specular 0.2 roughness 0.6875}"


def main():
    # A box on the left as a primitive, on the right as an isosurface with the same finish.
    body = HEAD + (
        "box{ <-2.2,-0.5,-0.5>, <-0.8,0.5,0.5> " + FINISH + " }\n"
        "isosurface{ function{ max(max(abs(x-1.5)-0.7, abs(y)-0.5), abs(z)-0.5) }\n"
        "  contained_by{ box{ <0.7,-0.6,-0.6>, <2.3,0.6,0.6> } } threshold 0 max_gradient 2\n"
        "  accuracy 0.0005 " + FINISH + " }\n"
    )
    px = render("iso_vs_box", body)
    # Front faces at the same height: mirror x about the frame centre (right vector is
    # negative, so +x is on the right of the picture at width/2 + ...).
    print("y   box(left)        iso(right)")
    for y in (40, 50, 60):
        print(y, px(60, y), px(140, y))
    print("top faces:")
    for y in (28, 32):
        print(y, px(60, y), px(140, y))


if __name__ == "__main__":
    main()
