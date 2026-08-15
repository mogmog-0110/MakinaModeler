"""Ask POV-Ray how a color_map with several stops is read, instead of guessing.

The public reference says a color_map lists [position color] entries and that colors are
blended between them. Two things it does not pin: whether the blend inside an interval is
linear in the pattern value, and what happens to a pattern value that falls before the first
stop or after the last. This renders a gradient x plane under `finish{ambient 1 diffuse 0}`,
so each pixel is exactly the map's color at that x, and reads a row of pixels back.

Run: python spike/pov_colormap_probe.py
"""

import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pov_filter_probe import POVINC, POVRAY, SCRATCH  # noqa: E402

W = 256


def render_row(name, body):
    """Renders at Wx4 with an orthographic camera spanning x in [0,1) and returns the middle row."""
    os.makedirs(SCRATCH, exist_ok=True)
    name = os.path.join(SCRATCH, name)
    header = (
        "global_settings{ assumed_gamma 1.0 }\n"
        "background{ color rgb<0,0,0> }\n"
        # x runs 0..1 across the frame. Looking down +z from -z in POV's left-handed frame, a
        # positive right vector puts +x on the right of the picture; the first run of this probe
        # used <-1,0,0> and read the map mirrored (0.2's color came out at x=0.8).
        "camera{ orthographic location <0.5,0,-5> look_at <0.5,0,0> "
        "right <1,0,0> up <0,4.0/" + str(W) + ",0> }\n"
    )
    with open(name + ".pov", "w") as f:
        f.write(header + body)
    subprocess.run(
        [POVRAY, "+I" + name + ".pov", "+O" + name + ".bmp", "+W" + str(W), "+H4",
         "+FS", "-A", "-D", "+L" + POVINC],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    data = open(name + ".bmp", "rb").read()
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    stride = (width * 3 + 3) // 4 * 4
    row = height // 2
    out = []
    for x in range(width):
        i = offset + (height - 1 - row) * stride + x * 3
        out.append(data[i + 2] / 255.0)
    return out


def main():
    # Three stops, uneven, red channel only, and the map does not start at 0 or end at 1 so
    # the clamping question is asked as well.
    body = (
        "plane{ z, 0 pigment{ gradient x color_map{ [0.2 color rgb 0.2] [0.5 color rgb 1.0] "
        "[0.8 color rgb 0.4] } } finish{ ambient 1 diffuse 0 } }\n"
    )
    row = render_row("cmap3", body)
    print("x      POV     linear-in-interval, clamped outside")
    for x in (0.05, 0.15, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.95):
        px = min(W - 1, int(x * W))
        # sRGB out; the values were chosen so linear vs sRGB is visible in the residual.
        pov = row[px]
        if x <= 0.2:
            model = 0.2
        elif x <= 0.5:
            model = 0.2 + (1.0 - 0.2) * (x - 0.2) / 0.3
        elif x <= 0.8:
            model = 1.0 + (0.4 - 1.0) * (x - 0.5) / 0.3
        else:
            model = 0.4
        # POV writes sRGB-encoded bytes for assumed_gamma 1.0 (the brilliance probe learned this).
        enc = model / 12.92 if model <= 0.0031308 else 1.055 * model ** (1 / 2.4) - 0.055
        print(f"{x:.2f}   {pov:.3f}   {enc:.3f}   {'ok' if abs(pov - enc) < 0.02 else 'NO'}")


if __name__ == "__main__":
    main()
