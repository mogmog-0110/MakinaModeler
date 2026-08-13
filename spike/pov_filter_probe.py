"""Ask POV-Ray what its own filter weights are, instead of guessing them.

A pigment with `filter` hides part of what is behind it and lets part through. Two numbers decide
the picture: how much of the surface's own shading survives, and what tint the rest is multiplied
by on the way through. Both are documented loosely enough that three plausible readings of them
fit the same white-glass scene, and only one of the three is right -- the renderer spent two
attempts on wrong ones before this probe existed.

The trick is a surface with `finish{ambient 1 diffuse 0}`. Its shading is exactly its pigment,
with no light, normal or camera in the way, so the rendered pixel *is* the weight times a color
that is already known. Sweeping filter and color therefore reports the weight directly.

What it finds, and what scene_shading.hlsl implements:

    surface   1 - filter * max(pigment.r, pigment.g, pigment.b)   -- not 1 - filter
    through   filter * pigment

Run it when POV-Ray is upgraded, or when a filtered scene starts disagreeing.
"""

import os
import struct
import subprocess
import sys

POVRAY = r"D:\sandbox\Grasp3D\povray\bin\povray.exe"
POVINC = r"D:\sandbox\Grasp3D\povray\include"

HEADER = (
    "global_settings{ assumed_gamma 1.0 }\n"
    "background{ color rgb<0,0,0> }\n"
    "camera{ location <0,0,-5> look_at <0,0,0> right <-1,0,0> up <0,1,0> }\n"
)


SCRATCH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "build", "probe")


def render_centre(name, body):
    """Render `body` at 32x32 and return the centre pixel as three floats in 0..1.

    Small on purpose: every probe scene is flat, so one pixel carries the whole answer and the
    sweep stays under a second.
    """
    os.makedirs(SCRATCH, exist_ok=True)
    name = os.path.join(SCRATCH, name)
    with open(name + ".pov", "w") as f:
        f.write(HEADER + body)
    subprocess.run(
        [POVRAY, "+I" + name + ".pov", "+O" + name + ".bmp", "+W32", "+H32",
         "+FS", "-A", "-D", "+L" + POVINC],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    if not os.path.exists(name + ".bmp"):
        raise RuntimeError("POV-Ray produced no image for " + name + ".pov")
    data = open(name + ".bmp", "rb").read()
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    stride = (width * 3 + 3) // 4 * 4
    i = offset + (height - 1 - height // 2) * stride + (width // 2) * 3
    return (data[i + 2] / 255.0, data[i + 1] / 255.0, data[i] / 255.0)


def flat_sphere(color, filt):
    return ("sphere{ <0,0,0>, 8 pigment{ color rgbf<%.6f,%.6f,%.6f,%.6f> }"
            " finish{ ambient 1 diffuse 0 } }\n" % (color[0], color[1], color[2], filt))


def to_linear(encoded):
    """Undo the sRGB transfer POV-Ray writes into a BMP.

    Checked rather than assumed: a ramp of twenty known linear values comes back within one level
    of this curve, so the weights below are read in the space POV computes them in.
    """
    if encoded <= 0.04045:
        return encoded / 12.92
    return ((encoded + 0.055) / 1.055) ** 2.4


def main():
    if not os.path.exists(POVRAY):
        print("POV-Ray not found at " + POVRAY)
        return 1

    glass = (120 / 255.0, 200 / 255.0, 160 / 255.0)
    print("surface weight -- what survives of the surface's own shading\n")
    print("  %-22s %-24s %-9s %s" % ("probe", "measured (r g b)", "1-f", "1-f*max3"))
    worst = 0.0
    for name, color, filt in [("white  f=0.25", (1.0, 1.0, 1.0), 0.25),
                               ("white  f=0.50", (1.0, 1.0, 1.0), 0.50),
                               ("white  f=0.75", (1.0, 1.0, 1.0), 0.75),
                               ("glass  f=0.25", glass, 0.25),
                               ("glass  f=0.50", glass, 0.50),
                               ("glass  f=0.75", glass, 0.75)]:
        pixel = render_centre("probe_%s" % name.split()[0] + str(int(filt * 100)),
                              flat_sphere(color, filt))
        weights = [to_linear(pixel[k]) / color[k] for k in range(3)]
        predicted = 1.0 - filt * max(color)
        worst = max(worst, max(abs(w - predicted) for w in weights))
        print("  %-22s %.4f %.4f %.4f    %.4f    %.4f"
              % (name, weights[0], weights[1], weights[2], 1.0 - filt, predicted))

    print("\n  worst departure from 1-f*max3: %.4f" % worst)

    # The transmitted tint, checked through a sphere -- which a ray enters and leaves, so the
    # prediction has to compose two layers. Getting this wrong looks identical to getting the
    # surface weight wrong on a single flat surface, which is why it is a separate probe.
    backing = (0.9, 0.4, 0.2)
    filt = 0.75
    body = ("sphere{ <0,0,0>, 1.5 pigment{ color rgbf<%.6f,%.6f,%.6f,%.6f> }"
            " finish{ ambient 1 diffuse 0 } }\n"
            "plane{ <0,0,-1>, -4 pigment{ color rgb<%.6f,%.6f,%.6f> }"
            " finish{ ambient 1 diffuse 0 } }\n"
            % (glass[0], glass[1], glass[2], filt, backing[0], backing[1], backing[2]))
    measured = [to_linear(v) for v in render_centre("probe_through", body)]
    weight = 1.0 - filt * max(glass)
    front = [weight * glass[k] for k in range(3)]
    tint = [filt * glass[k] for k in range(3)]
    predicted = [front[k] + tint[k] * (front[k] + tint[k] * backing[k]) for k in range(3)]

    print("\nthrough a sphere onto a lit plane -- two layers of glass\n")
    print("  measured   %.4f %.4f %.4f" % tuple(measured))
    print("  predicted  %.4f %.4f %.4f" % tuple(predicted))
    print("  worst departure: %.4f"
          % max(abs(measured[k] - predicted[k]) for k in range(3)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
