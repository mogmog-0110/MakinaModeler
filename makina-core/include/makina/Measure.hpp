// Numeric checks on a scene: a port of Grasp3D's SceneMeasure.
//
// These put a number on placement errors the eye cannot catch -- is this part actually touching, or
// floating a hair above; does this bore really clear that boss; is the flange symmetric. It is the
// asset-validation half of the tool, and the reason a solid modeller can say things a mesh cannot.
//
// What the numbers mean, unchanged from the reference:
//   - distances inherit SceneSdf's guarantee: the sign is exact, the magnitude is a conservative
//     lower bound (Eval.hpp)
//   - a gap is measured from surface samples, so its resolution is bounded by the sample density;
//     `tol` is what absorbs that quantisation
//   - the overlap volume is a Monte-Carlo estimate, not an integral

#pragma once

#include "Bounds.hpp"
#include "Eval.hpp"
#include "Scene.hpp"
#include "Surface.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace makina {

/// Sample budget per side of a gap measurement.
constexpr int kGapBudget = 3000;
/// Points thrown at the AABB overlap for the volume estimate.
constexpr int kMonteCarloSamples = 20000;
/// Reduced budget for the pairwise contact test, which runs O(n^2) times.
constexpr int kPairBudget = 1200;

namespace detail {

/// Java's java.util.Random, reproduced.
///
/// Not a recommendation -- a 48-bit LCG is a poor generator. It is here so the Monte-Carlo volume
/// can be compared against the reference exactly rather than statistically: with the same seed and
/// the same sequence the two implementations must agree to the last bit, which turns a fuzzy "close
/// enough" into a real check. Volume estimation in three dimensions is well within what this
/// generator can do.
class JavaRandom {
public:
    explicit JavaRandom(std::int64_t seed)
        : m_seed((seed ^ 0x5DEECE66DLL) & ((1LL << 48) - 1)) {}

    double nextDouble() {
        const std::int64_t hi = next(26);
        const std::int64_t lo = next(27);
        return static_cast<double>((hi << 27) + lo) / static_cast<double>(1LL << 53);
    }

private:
    std::int32_t next(int bits) {
        m_seed = (m_seed * 0x5DEECE66DLL + 0xBLL) & ((1LL << 48) - 1);
        return static_cast<std::int32_t>(m_seed >> (48 - bits));
    }

    std::int64_t m_seed;
};

}  // namespace detail

// ---------------------------------------------------------------- gap

struct GapResult {
    double        distance = kEmpty;   ///< negative when the two interpenetrate
    double        closest[3]{};        ///< world point that produced the minimum
    std::uint16_t fromPrim = kNoChild; ///< primitive that point came from
    int           samples = 0;
    int           unsupported = 0;
    bool          valid = false;
};

namespace detail {

/// Measures src's surface points against dst's distance field.
inline void probe(const Scene& s, std::uint16_t src, std::uint16_t dst, int budget,
                  GapResult& r, Fidelity f) {
    const std::vector<SurfaceSample> samples = surfaceSamples(s, src, budget, f);
    for (const SurfaceSample& sample : samples) {
        const double d = eval(s, dst, sample.p, f);
        if (d < r.distance) {
            r.distance = d;
            r.closest[0] = sample.p[0];
            r.closest[1] = sample.p[1];
            r.closest[2] = sample.p[2];
            r.fromPrim = sample.prim;
            r.valid = true;
        }
    }
    r.samples += static_cast<int>(samples.size());
}

}  // namespace detail

/// Signed minimum distance between two subtrees.
///
/// Sampled from both sides and the smaller kept. One direction alone is not enough: points on a
/// large flat plate are sparse where a small pin touches it, so probing only plate-against-pin
/// reports a gap that the pin-against-plate probe finds to be zero.
inline GapResult gap(const Scene& s, std::uint16_t a, std::uint16_t b, Fidelity f = {}) {
    GapResult r;
    r.unsupported = countUnsupported(s, a) + countUnsupported(s, b);
    detail::probe(s, a, b, kGapBudget, r, f);
    detail::probe(s, b, a, kGapBudget, r, f);
    return r;
}

/// Axis-aligned distance between two world boxes, 0 when they intersect. A lower bound on the
/// true distance, which is what makes it usable as a cheap rejection before sampling.
inline double aabbGap(const Aabb& a, const Aabb& b) {
    if (!a.valid || !b.valid) {
        return kEmpty;
    }
    double d2 = 0.0;
    for (int i = 0; i < 3; ++i) {
        const double d = std::max(0.0, std::max(a.lo[i] - b.hi[i], b.lo[i] - a.hi[i]));
        d2 += d * d;
    }
    return std::sqrt(d2);
}

// ---------------------------------------------------------------- overlap

struct OverlapResult {
    bool   aabbIntersects = false;
    bool   overlapping = false;
    double maxPenetration = 0.0;   ///< from surface samples; a lower bound
    double witness[3]{};           ///< deepest point found
    double volume = 0.0;           ///< Monte-Carlo estimate; 0 when the boxes miss
    int    samples = 0;
};

/// Penetration depth plus a Monte-Carlo estimate of the shared volume.
inline OverlapResult overlap(const Scene& s, std::uint16_t a, std::uint16_t b, double tol,
                             Fidelity f = {}) {
    OverlapResult r;

    const BoundsResult ba = worldBounds(s, a, f);
    const BoundsResult bb = worldBounds(s, b, f);
    if (!ba.box.valid || !bb.box.valid) {
        return r;
    }

    const GapResult g = gap(s, a, b, f);
    r.samples = g.samples;
    if (g.valid && g.distance < 0.0) {
        r.maxPenetration = -g.distance;
        r.witness[0] = g.closest[0];
        r.witness[1] = g.closest[1];
        r.witness[2] = g.closest[2];
    }

    double lo[3];
    double hi[3];
    r.aabbIntersects = true;
    for (int i = 0; i < 3; ++i) {
        lo[i] = std::max(ba.box.lo[i], bb.box.lo[i]);
        hi[i] = std::min(ba.box.hi[i], bb.box.hi[i]);
        if (!(lo[i] < hi[i])) {
            r.aabbIntersects = false;
        }
    }

    if (r.aabbIntersects) {
        detail::JavaRandom rnd(12345);
        int hits = 0;
        for (int i = 0; i < kMonteCarloSamples; ++i) {
            double p[3];
            for (int k = 0; k < 3; ++k) {
                p[k] = lo[k] + rnd.nextDouble() * (hi[k] - lo[k]);
            }
            if (eval(s, a, p, f) < 0.0 && eval(s, b, p, f) < 0.0) {
                ++hits;
            }
        }
        r.volume = static_cast<double>(hits) / kMonteCarloSamples *
                   (hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]);
    }

    r.overlapping = r.maxPenetration > tol || r.volume > 0.0;
    return r;
}

// ---------------------------------------------------------------- floating

struct FloatItem {
    std::uint16_t node = kNoChild;
    double        minY = 0.0;
    bool          supported = false;
    bool          sunk = false;          ///< below the ground plane
    double        gapToNearest = kEmpty; ///< distance to the nearest part, when unsupported
};

namespace detail {

/// Distance between two parts on a reduced budget. The contact graph needs O(n^2) of these, so the
/// full gap budget would make the check quadratically expensive for no extra decision quality.
inline double pairGap(const Scene& s, std::uint16_t a, std::uint16_t b, Fidelity f) {
    GapResult r;
    probe(s, a, b, kPairBudget, r, f);
    probe(s, b, a, kPairBudget, r, f);
    return r.valid ? r.distance : kEmpty;
}

}  // namespace detail

/// Parts that cannot be reached from the ground through contacts.
///
/// Builds a contact graph over the direct children of `parent` and walks it from whatever is
/// already grounded. Anything unreachable is floating. Labels and children without bounds -- a lone
/// Plane, say -- are left out, since neither can support anything.
inline std::vector<FloatItem> floating(const Scene& s, std::uint16_t parent, double groundY,
                                       double tol, Fidelity f = {}) {
    std::vector<FloatItem> items;
    std::vector<Aabb> boxes;

    const CsgNode& p = s.nodes[parent];
    for (std::uint16_t i = 0; i < p.childCount; ++i) {
        const std::uint16_t c = static_cast<std::uint16_t>(p.firstChild + i);
        if (static_cast<Op>(s.nodes[c].op) == Op::Label) {
            continue;
        }
        const BoundsResult b = worldBounds(s, c, f);
        if (!b.box.valid) {
            continue;
        }
        FloatItem it;
        it.node = c;
        it.minY = b.box.lo[1];
        items.push_back(it);
        boxes.push_back(b.box);
    }

    const std::size_t n = items.size();
    if (n == 0) {
        return items;
    }

    std::vector<std::uint8_t> touch(n * n, 0);
    std::vector<double> dist(n * n, kEmpty);

    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            const double ad = aabbGap(boxes[i], boxes[j]);
            // Far apart by the boxes alone means far apart in fact, because the box distance is a
            // lower bound. Sampling those pairs would only confirm what is already known.
            const double d = (ad > tol * 4.0)
                                 ? ad
                                 : detail::pairGap(s, items[i].node, items[j].node, f);
            dist[i * n + j] = dist[j * n + i] = d;
            const std::uint8_t t = d <= tol ? 1 : 0;
            touch[i * n + j] = touch[j * n + i] = t;
        }
    }

    std::vector<std::uint8_t> reached(n, 0);
    std::vector<std::size_t> queue;
    for (std::size_t i = 0; i < n; ++i) {
        if (items[i].minY <= groundY + tol) {
            reached[i] = 1;
            queue.push_back(i);
        }
    }
    while (!queue.empty()) {
        const std::size_t i = queue.back();
        queue.pop_back();
        for (std::size_t j = 0; j < n; ++j) {
            if (!reached[j] && touch[i * n + j]) {
                reached[j] = 1;
                queue.push_back(j);
            }
        }
    }

    for (std::size_t i = 0; i < n; ++i) {
        items[i].supported = reached[i] != 0;
        items[i].sunk = items[i].minY < groundY - tol;
        if (!items[i].supported) {
            // The ground counts as a candidate: a part hovering just above it is nearer to the
            // ground than to anything else, and reporting the distance to a far-off neighbour
            // instead would point at the wrong thing.
            double best = items[i].minY - groundY;
            for (std::size_t j = 0; j < n; ++j) {
                if (j != i && dist[i * n + j] < best) {
                    best = dist[i * n + j];
                }
            }
            items[i].gapToNearest = best;
        }
    }
    return items;
}

// ---------------------------------------------------------------- symmetry

struct SymmetryOffender {
    std::uint16_t prim = kNoChild;
    double        deviation = 0.0;
};

struct SymmetryResult {
    double maxDev = 0.0;
    double meanDev = 0.0;
    int    samples = 0;
    std::vector<SymmetryOffender> offenders;   ///< above tol, worst first
};

/// Mirrors the subtree's surface points across a plane and measures how far each lands from the
/// surface. A perfectly symmetric subtree gives zero everywhere.
///
/// axis is 0/1/2 for X/Y/Z.
inline SymmetryResult symmetry(const Scene& s, std::uint16_t index, int axis, double plane,
                               double tol, Fidelity f = {}) {
    SymmetryResult r;
    const std::vector<SurfaceSample> samples = surfaceSamples(s, index, kGapBudget, f);
    if (samples.empty()) {
        return r;
    }

    std::vector<std::uint16_t> prims;
    std::vector<double> worst;
    double sum = 0.0;

    for (const SurfaceSample& sample : samples) {
        double m[3] = {sample.p[0], sample.p[1], sample.p[2]};
        m[axis] = 2.0 * plane - m[axis];
        const double dev = std::fabs(eval(s, index, m, f));

        sum += dev;
        if (dev > r.maxDev) {
            r.maxDev = dev;
        }

        std::size_t slot = prims.size();
        for (std::size_t i = 0; i < prims.size(); ++i) {
            if (prims[i] == sample.prim) {
                slot = i;
                break;
            }
        }
        if (slot == prims.size()) {
            prims.push_back(sample.prim);
            worst.push_back(dev);
        } else if (dev > worst[slot]) {
            worst[slot] = dev;
        }
    }

    r.samples = static_cast<int>(samples.size());
    r.meanDev = sum / samples.size();

    for (std::size_t i = 0; i < prims.size(); ++i) {
        if (worst[i] > tol) {
            r.offenders.push_back(SymmetryOffender{prims[i], worst[i]});
        }
    }
    std::sort(r.offenders.begin(), r.offenders.end(),
              [](const SymmetryOffender& a, const SymmetryOffender& b) {
                  return a.deviation > b.deviation;
              });
    return r;
}

}  // namespace makina
