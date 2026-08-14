// The shared shape of a Sor's profile: its control points and the per-segment Hermite terms.
//
// Eval.hpp walks the curve into a polyline for the distance, Bounds.hpp bounds it without
// walking; both must read the same spline or the box could exclude surface. This header is the
// one statement of that spline: per segment, the unique cubic in r-squared over h through the
// four surrounding control points, so the first and last points steer the ends without lying on
// the surface (POV's sor, public reference; the form is pinned by measurement -- see
// sorSegment).

#pragma once

#include "Scene.hpp"

#include <cmath>
#include <cstdint>

namespace makina {
namespace detail {

/// Control points a Sor can hold, and how finely each spline segment is walked into a polyline.
///
/// 24 keeps the sag of the chord below ~1e-4 of the radius for profiles as curved as any
/// measured file; the silhouette comparison against POV is what watches that this stays true.
constexpr int kMaxSorPoints = 32;
constexpr int kSorSamplesPerSegment = 24;

/// The Sor's control points in file order, as r-squared and height. Returns the count.
inline int sorControls(const Scene& s, std::uint16_t index, double r2[kMaxSorPoints],
                       double h[kMaxSorPoints]) {
    const CsgNode& n = s.nodes[index];
    int count = 0;
    for (std::uint16_t i = 0; i < n.childCount && count < kMaxSorPoints; ++i) {
        const CsgNode& c = s.nodes[static_cast<std::uint16_t>(n.firstChild + i)];
        if (static_cast<Op>(c.op) != Op::SorPoint) {
            continue;
        }
        r2[count] = static_cast<double>(c.params[0]) * c.params[0];
        h[count] = c.params[1];
        ++count;
    }
    return count;
}

/// Slope at node k (0..3) of the one cubic through the four points (x[], y[]).
inline double lagrangeSlopeAt(const double x[4], const double y[4], int k) {
    double sum = 0.0;
    for (int j = 0; j < 4; ++j) {
        if (j == k) {
            double s = 0.0;
            for (int m = 0; m < 4; ++m) {
                if (m != k) {
                    s += 1.0 / (x[k] - x[m]);
                }
            }
            sum += y[k] * s;
        } else {
            double numer = 1.0;
            double denom = 1.0;
            for (int m = 0; m < 4; ++m) {
                if (m == j) {
                    continue;
                }
                if (m != k) {
                    numer *= x[k] - x[m];
                }
                denom *= x[j] - x[m];
            }
            sum += y[j] * numer / denom;
        }
    }
    return sum;
}

/// Height step and end slopes of segment `seg`: the unique cubic in r-squared over h through
/// the four surrounding control points, evaluated between the middle two.
///
/// Chosen by measurement, not taste. Three tangent rules were rendered against POV's own sor
/// (spike/sor-silhouette-check.bat): neighbour secants (Catmull-Rom) missed by 1.6 px of edge,
/// a clamped C2 spline bulged fatter than POV on one whole side, and the four-point cubic is
/// what the public reference's "computed from four consecutive points" turned out to mean.
inline void sorSegment(const double r2[], const double h[], int seg, double& dh, double& m0,
                       double& m1) {
    dh = h[seg + 1] - h[seg];
    m0 = lagrangeSlopeAt(h + seg - 1, r2 + seg - 1, 1);
    m1 = lagrangeSlopeAt(h + seg - 1, r2 + seg - 1, 2);
}

/// Most vertices a walked profile can hold: one side, bottom to top.
constexpr int kMaxSorSide = (kMaxSorPoints - 3) * kSorSamplesPerSegment + 1;

/// Walks the spline into the right-side polyline, (radius, height) pairs bottom to top.
/// Returns the vertex count, or 0 when the Sor holds too few points or a folded height run --
/// the importer refuses both, so a fold here is a hand-built scene and stays empty.
inline int sorPolyline(const Scene& s, std::uint16_t index, double side[][2]) {
    double r2[kMaxSorPoints];
    double h[kMaxSorPoints];
    const int n = sorControls(s, index, r2, h);
    if (n < 4) {
        return 0;
    }
    for (int i = 1; i + 2 < n; ++i) {
        if (h[i + 1] <= h[i]) {
            return 0;
        }
    }
    int num = 0;
    for (int seg = 1; seg + 2 < n; ++seg) {
        double dh, m0, m1;
        sorSegment(r2, h, seg, dh, m0, m1);
        const int first = seg == 1 ? 0 : 1;
        for (int k = first; k <= kSorSamplesPerSegment; ++k) {
            const double t = static_cast<double>(k) / kSorSamplesPerSegment;
            const double t2 = t * t, t3 = t2 * t;
            const double v = (2 * t3 - 3 * t2 + 1) * r2[seg] + (t3 - 2 * t2 + t) * dh * m0 +
                             (-2 * t3 + 3 * t2) * r2[seg + 1] + (t3 - t2) * dh * m1;
            side[num][0] = std::sqrt(v > 0.0 ? v : 0.0);
            side[num][1] = h[seg] + t * dh;
            ++num;
        }
    }
    return num;
}

/// Signed distance from (rho, y) to the revolved solid's full cross-section, negative inside.
///
/// `xy` holds the right-side polyline as flat (radius, height) pairs; the left side is the
/// mirror, taken on the fly so no caller stores it. Even-odd crossings decide the sign, so the
/// axis is interior and never mistaken for surface. T is float in the flattened program and
/// double in the tree evaluator -- one implementation, so the two cannot drift.
template <typename T>
inline double sorSideDistance(const T* xy, int num, double px, double py) {
    const int total = 2 * num;
    const auto vx = [&](int j) {
        return j < num ? static_cast<double>(xy[2 * j]) : -xy[2 * (total - 1 - j)];
    };
    const auto vy = [&](int j) {
        return static_cast<double>(j < num ? xy[2 * j + 1] : xy[2 * (total - 1 - j) + 1]);
    };
    double d = 1e60;
    double sgn = 1.0;
    for (int i = 0, j = total - 1; i < total; j = i++) {
        const double ex = vx(j) - vx(i), ey = vy(j) - vy(i);
        const double wx = px - vx(i), wy = py - vy(i);
        const double ee = ex * ex + ey * ey;
        double t = ee > 0.0 ? (wx * ex + wy * ey) / ee : 0.0;
        t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
        const double bx = wx - ex * t, by = wy - ey * t;
        const double q = bx * bx + by * by;
        if (q < d) {
            d = q;
        }
        const bool c1 = py >= vy(i);
        const bool c2 = py < vy(j);
        const bool c3 = ex * wy > ey * wx;
        if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) {
            sgn = -sgn;
        }
    }
    return sgn * std::sqrt(d);
}

}  // namespace detail
}  // namespace makina
