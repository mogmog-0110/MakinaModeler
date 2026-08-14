// The shared shape of a SphereSweep's path: control points walked into (center, radius)
// samples. Eval.hpp chains round cones through them for the distance, Flatten.hpp bakes them
// into the program's side table; one walk here, so the two cannot drift.
//
// The b_spline is the uniform cubic B-spline (public knowledge): each segment is steered by
// four consecutive control points and the curve touches none of them. linear_spline is the
// control polygon itself. Which of the two POV meant is measured, not assumed -- the
// sweep-silhouette comparison against POV's own sphere_sweep is the gate.

#pragma once

#include "Scene.hpp"

#include <cmath>
#include <cstdint>

namespace makina {
namespace detail {

constexpr int kMaxSweepPoints = 32;
constexpr int kSweepSamplesPerSegment = 24;

/// Most (center, radius) samples a walked sweep can hold.
constexpr int kMaxSweepSamples = (kMaxSweepPoints - 3) * kSweepSamplesPerSegment + 1;

/// The SphereSweep's control points in file order, (x, y, z, radius). Returns the count.
inline int sweepControls(const Scene& s, std::uint16_t index, double pts[][4]) {
    const CsgNode& n = s.nodes[index];
    int count = 0;
    for (std::uint16_t i = 0; i < n.childCount && count < kMaxSweepPoints; ++i) {
        const CsgNode& c = s.nodes[static_cast<std::uint16_t>(n.firstChild + i)];
        if (static_cast<Op>(c.op) != Op::SweepPoint) {
            continue;
        }
        for (int k = 0; k < 4; ++k) {
            pts[count][k] = c.params[k];
        }
        ++count;
    }
    return count;
}

/// Walks the path into (x, y, z, radius) samples. Returns the sample count, 0 when the sweep
/// holds too few points for its spline. Radii follow the same spline as the centers -- POV
/// treats the radius as a fourth coordinate, and treating it otherwise would fatten the solid
/// exactly between the control points where nobody stated a radius.
inline int sweepSamples(const Scene& s, std::uint16_t index, double out[][4]) {
    double pts[kMaxSweepPoints][4];
    const int n = sweepControls(s, index, pts);
    const bool bspline = (s.nodes[index].flags & flags::kSweepBspline) != 0;

    if (!bspline) {
        if (n < 2) {
            return 0;
        }
        for (int i = 0; i < n; ++i) {
            for (int k = 0; k < 4; ++k) {
                out[i][k] = pts[i][k];
            }
        }
        return n;
    }

    if (n < 4) {
        return 0;
    }
    int num = 0;
    for (int seg = 0; seg + 3 < n; ++seg) {
        const int first = seg == 0 ? 0 : 1;
        for (int k = first; k <= kSweepSamplesPerSegment; ++k) {
            const double t = static_cast<double>(k) / kSweepSamplesPerSegment;
            const double t2 = t * t, t3 = t2 * t;
            // The uniform cubic B-spline basis, times 6.
            const double b0 = (1 - t) * (1 - t) * (1 - t);
            const double b1 = 3 * t3 - 6 * t2 + 4;
            const double b2 = -3 * t3 + 3 * t2 + 3 * t + 1;
            const double b3 = t3;
            for (int c = 0; c < 4; ++c) {
                out[num][c] = (b0 * pts[seg][c] + b1 * pts[seg + 1][c] +
                               b2 * pts[seg + 2][c] + b3 * pts[seg + 3][c]) / 6.0;
            }
            ++num;
        }
    }
    return num;
}

}  // namespace detail
}  // namespace makina
