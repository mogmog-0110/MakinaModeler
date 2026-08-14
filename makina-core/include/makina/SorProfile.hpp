// The shared shape of a Sor's profile: its control points and the per-segment Hermite terms.
//
// Eval.hpp walks the curve into a polyline for the distance, Bounds.hpp bounds it without
// walking; both must read the same spline or the box could exclude surface. This header is the
// one statement of that spline: a cubic in r-squared over h through the interior points, each
// segment a Hermite whose end slopes are the neighbours' secants, with the first and last
// points steering the ends without lying on the surface (POV's sor, public reference).

#pragma once

#include "Scene.hpp"

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

/// Height step and end slopes of segment `seg` (between points seg and seg+1, both interior).
inline void sorSegment(const double r2[], const double h[], int seg, double& dh, double& m0,
                       double& m1) {
    dh = h[seg + 1] - h[seg];
    m0 = (r2[seg + 1] - r2[seg - 1]) / (h[seg + 1] - h[seg - 1]);
    m1 = (r2[seg + 2] - r2[seg]) / (h[seg + 2] - h[seg]);
}

}  // namespace detail
}  // namespace makina
