"""Ask POV-Ray where a known direction lands on the film, instead of guessing the projection.

A camera model is a map from a direction to a pixel. Reading it out of prose is how this project
got `panoramic` wrong twice -- two readings of "the vertical" that measured 159 and 95 against a
limit of 6, with no way to tell which part was wrong. So this measures the map instead: put one
small bright sphere at a direction that is known exactly, render, and find which pixel it lands on.
Sweep the direction and the whole mapping falls out.

What it reports, per camera:

    ndc.x against the yaw       -- how the horizontal axis is parameterised
    ndc.y against the elevation -- and the vertical, which is the half that was ambiguous

ndc runs -1..1 across the frame, which is what mkCameraRay in scene_shading.hlsl takes. A model
that matches is one whose inverse reproduces these pairs; a model that does not shows up as a
curve where the candidate is a straight line, at a glance, rather than as a mean pixel difference
that says only "somewhere".
"""

import math
import os
import struct
import subprocess
import sys

POVRAY = r"D:\sandbox\Grasp3D\povray\bin\povray.exe"
POVINC = r"D:\sandbox\Grasp3D\povray\include"
SCRATCH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "build", "camprobe")

WIDTH = 640
HEIGHT = 360

# The camera the exporter writes, minus the projection keyword. Left-handed POV with a negated
# right vector is what makes a right-handed scene come out unmirrored (COORDINATES.md), and the
# measurement is worthless if it is taken through a different camera than the one in use.
def camera(kind, angle_degrees):
    aspect = WIDTH / float(HEIGHT)
    return ("camera{\n"
            "    %s\n"
            "    right<-%.10f,0,0>\n"
            "    up<0,1,0>\n"
            "    angle %.10f\n"
            "    location<0,0,0>\n"
            "    look_at<0,0,1>\n"
            "}\n" % (kind, aspect, angle_degrees))


def marker(yaw_degrees, elevation_degrees):
    """One emissive sphere at a direction, far enough away to be small on the film."""
    yaw = math.radians(yaw_degrees)
    elev = math.radians(elevation_degrees)
    r = 60.0
    x = r * math.cos(elev) * math.sin(yaw)
    y = r * math.sin(elev)
    z = r * math.cos(elev) * math.cos(yaw)
    return ("sphere{ <%.10f,%.10f,%.10f>, 0.9\n"
            "    pigment{ color rgb<1,1,1> } finish{ ambient 1 diffuse 0 } }\n" % (x, y, z))


def render_centroid(name, body):
    """Renders and returns the ndc of the bright pixels' centroid, or None if nothing showed."""
    os.makedirs(SCRATCH, exist_ok=True)
    path = os.path.join(SCRATCH, name)
    with open(path + ".pov", "w") as f:
        f.write("global_settings{ assumed_gamma 1.0 }\nbackground{ color rgb<0,0,0> }\n" + body)
    subprocess.run(
        [POVRAY, "+I" + path + ".pov", "+O" + path + ".bmp",
         "+W%d" % WIDTH, "+H%d" % HEIGHT, "+FS", "-A", "-D", "+L" + POVINC],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    if not os.path.exists(path + ".bmp"):
        raise RuntimeError("POV-Ray produced no image for " + name)
    data = open(path + ".bmp", "rb").read()
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    stride = (width * 3 + 3) // 4 * 4

    sx = sy = 0.0
    n = 0
    for y in range(height):
        row = offset + (height - 1 - y) * stride
        for x in range(width):
            if data[row + x * 3] > 100:
                sx += x
                sy += y
                n += 1
    if n == 0:
        return None
    # Pixel centres to ndc, the same convention mkCameraRay is fed.
    return ((sx / n + 0.5) / width * 2.0 - 1.0, (sy / n + 0.5) / height * 2.0 - 1.0)


def sweep(kind, angle_degrees):
    print("\n%s, angle %g" % (kind, angle_degrees))
    print("  horizontal: yaw -> ndc.x        (a straight line means the axis is the angle itself)")
    for yaw in range(-60, 61, 15):
        p = render_centroid("h_%s_%d" % (kind, yaw + 90), camera(kind, angle_degrees) +
                            marker(yaw, 0.0))
        if p is None:
            print("    yaw %+4d  off the film" % yaw)
            continue
        print("    yaw %+4d  ndc.x %+8.4f   yaw/(angle/2) %+8.4f   tan ratio %+8.4f"
              % (yaw, p[0], yaw / (angle_degrees / 2.0),
                 math.tan(math.radians(yaw)) / math.tan(math.radians(angle_degrees / 2.0))))

    print("  vertical: elevation -> ndc.y")
    for elev in range(-40, 41, 10):
        p = render_centroid("v_%s_%d" % (kind, elev + 90), camera(kind, angle_degrees) +
                            marker(0.0, elev))
        if p is None:
            print("    elev %+4d  off the film" % elev)
            continue
        e = math.radians(elev)
        print("    elev %+4d  ndc.y %+8.4f   -tan %+8.4f   -sin %+8.4f   -angle/(pi/4) %+8.4f"
              % (elev, p[1], -math.tan(e), -math.sin(e), -e / (math.pi / 4.0)))


def main():
    if not os.path.exists(POVRAY):
        print("POV-Ray not found at " + POVRAY)
        return 1
    kinds = sys.argv[1:] or ["panoramic", "perspective"]
    for kind in kinds:
        sweep(kind, 90.0)
    return 0


if __name__ == "__main__":
    sys.exit(main())
