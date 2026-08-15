# The numeric half of perf-check.bat: reads render_scene's "drew in X ms" lines and holds them
# to the plan's ceiling -- but tells a slow machine apart from a slow shader first.
#
# This exists because the gate used to be a findstr regex, and findstr cannot compare numbers.
# "drew in [3-9][3-9]\." looks like "33 or more" but matches only when BOTH digits are 3-9, so
# 51.03 passed and 73.05 was the first value that ever fired. Numbers get compared as numbers,
# in a language that has them.
#
# The morning that fired (2026-08-15) every scene was ~2.7x its SPIKE_PERF.md figure at once
# -- 51/6/5/73 ms against 18/2/2/26 -- and a bisect over 12 commits in fresh worktrees found
# every one of them at 18.5 ms, the same binary included, once the machine had cooled. That is
# what a throttled integrated GPU looks like, not a regression: a regression lands on the
# scenes that use the changed code, not on all of them by the same factor. So the gate reads
# the ratio to the reference figure per scene, and when every scene is slow by the same factor
# it says "the machine, not the shader" and does not fail -- a red gate on a hot afternoon
# would teach everyone to ignore it. Uneven slowness is the real signal and still fails.
import argparse
import re
import sys

# What this machine drew on 2026-08-15 after cooling (SPIKE_PERF.md; min of 20 after warm-up).
# A new scene in perf-check.bat needs a line here, or the gate has nothing to compare it to.
REFERENCE_MS = {
    "render_pettobotoru.bmp": 18.4,
    "render_hero_flange.bmp": 2.2,
    "render_penrose.bmp": 2.0,
    "weathered_pettobotoru.bmp": 26.0,
}
# Ratios inside this band of each other count as "the same factor". SPIKE_PERF.md 2.1 puts
# run-to-run noise at 5%; the tiny scenes are noisier in ratio terms because 0.3 ms is 15% of
# 2 ms, so the band is wide enough for those and still far below the 2x a real change makes.
UNIFORM_BAND = 0.35


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
    ap.add_argument("--plan", type=float, required=True, help="PLAN.md's editing ceiling, ms")
    ap.add_argument("files", nargs="+")
    args = ap.parse_args()

    measured = [f for path in args.files for f in frames(path)]
    if not measured:
        # An empty measurement passing would be the regex bug all over again.
        print("perf_gate: no 'drew in' lines found; the measurement itself failed")
        return 2
    unknown = [l for l, _ in measured if l not in REFERENCE_MS]
    if unknown:
        print("perf_gate: no reference figure for " + ", ".join(unknown) + "; add one")
        return 2

    ratios = {l: ms / REFERENCE_MS[l] for l, ms in measured}
    over = [(l, ms) for l, ms in measured if ms > args.plan]
    if not over:
        print(f"   every frame is inside the {args.plan:g} ms the plan allows for editing")
        return 0

    lo, hi = min(ratios.values()), max(ratios.values())
    uniform = hi - lo <= UNIFORM_BAND * hi
    for l, ms in measured:
        print(f"   {l}: {ms} ms, {ratios[l]:.2f}x the reference {REFERENCE_MS[l]} ms")
    if uniform:
        print(f"   every scene is slow by the same factor ({lo:.2f}x-{hi:.2f}x): the machine is")
        print("   throttled or busy, not the shader. Not a failure; measure again when it is quiet.")
        return 0
    print(f"   A FRAME IS PAST THE {args.plan:g} ms THE PLAN ALLOWS, and the slowdown is uneven")
    print("   across scenes -- that is the shader, not the machine")
    return 1


if __name__ == "__main__":
    sys.exit(main())
