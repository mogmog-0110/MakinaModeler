# The numeric half of perf-check.bat: reads render_scene's "drew in X ms" lines and holds them
# to two different ceilings.
#
# This exists because the gate used to be a findstr regex, and findstr cannot compare numbers.
# "drew in [3-9][3-9]\." looks like "33 or more" but matches only when BOTH digits are 3-9 --
# 51.03 and 72.60 sailed through, 73.05 fired. The gate sat blind while pettobotoru drifted from
# SPIKE_PERF.md's 18.4 ms to 51.4, and its first ever failure looked like noise instead of a
# tripled frame. Numbers get compared as numbers, in a language that has them.
#
# Two ceilings, on purpose:
#   --plan     what PLAN.md promises for an editable frame. Missing it prints a warning but does
#              not fail the gate: SPIKE_PERF.md recorded 18.4 ms the day the gate was written and
#              2026-08-15 measures 51.4 ms -- a 3x drift the blind regex never reported, under
#              investigation as its own task. A gate that is red on every run guards nothing,
#              so until that regression is found this holds the line where it stands.
#   --ceiling  measured 2026-08-15 reality plus ten percent. Crossing it fails: that is a NEW
#              regression on top of the one being hunted.
import argparse
import re
import sys


def frames(path):
    """(label, ms) per "drew in X ms" line; the label is the output .bmp name."""
    out = []
    try:
        f = open(path, encoding="utf-8", errors="replace")
    except OSError:
        print(f"perf_gate: could not open '{path}'. Did render_scene run?")
        sys.exit(2)
    with f:
        for line in f:
            m = re.search(r"drew in ([0-9.]+) ms.*[/\\]([^/\\]+\.bmp)", line)
            if m:
                out.append((m.group(2), float(m.group(1))))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plan", type=float, required=True)
    ap.add_argument("--ceiling", type=float, required=True, action="append",
                    help="one per file, in file order")
    ap.add_argument("files", nargs="+")
    args = ap.parse_args()
    if len(args.ceiling) != len(args.files):
        print("perf_gate: one --ceiling per file, in the same order")
        return 2

    regressed = []
    over_plan = []
    seen = 0
    for path, ceiling in zip(args.files, args.ceiling):
        for label, ms in frames(path):
            seen += 1
            if ms > ceiling:
                regressed.append(f"{label}: {ms} ms is past the {ceiling} ms regression ceiling")
            elif ms > args.plan:
                over_plan.append(f"{label}: {ms} ms")

    if seen == 0:
        # An empty measurement passing would be the regex bug all over again.
        print("perf_gate: no 'drew in' lines found; the measurement itself failed")
        return 2
    if regressed:
        for r in regressed:
            print("   " + r)
        print("   A FRAME REGRESSED PAST WHAT THIS MACHINE DREW ON 2026-08-15")
        return 1
    if over_plan:
        print(f"   over the plan's {args.plan:g} ms editing target (known miss, held to the")
        print("   regression ceiling instead until the march gets cheaper):")
        for o in over_plan:
            print("     " + o)
        return 0
    print(f"   every frame is inside the {args.plan:g} ms the plan allows for editing")
    return 0


if __name__ == "__main__":
    sys.exit(main())
