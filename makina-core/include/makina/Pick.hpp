// Selecting by clicking: one ray against the same distance field the picture is made of.
//
// No id buffer, no read-back. A mesh renderer picks by drawing the scene again into an integer
// target and reading a pixel; here the click casts a single ray on the CPU and is done in a
// millisecond. It also cannot disagree with the picture, because it is the same field -- there is
// no way to end up with something visible that cannot be clicked, or the reverse.
//
// The awkward part is not finding the surface, it is deciding *what the user meant*. A ray that
// lands on the wall of a bore has hit the blade that cut it, and nobody clicking a hole means "the
// cylinder that was subtracted". So the hit primitive is where the answer starts, not where it
// ends: by default the selection walks up to the outermost group under the root, and a modifier
// walks it back down one level at a time. That is Maya's rule, and it is the one people expect.

#pragma once

#include "Bounds.hpp"
#include "Camera.hpp"
#include "Eval.hpp"
#include "Op.hpp"
#include "Scene.hpp"
#include "Surface.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

namespace makina {

struct PickResult {
    bool          hit = false;
    /// Node the click should select, after the hierarchy rule below.
    std::uint32_t id = 0;
    /// The primitive the ray actually landed on. Useful for a status line; not what gets selected.
    std::uint32_t primitiveId = 0;
    double        point[3]{};
    double        distance = 0.0;
    /// How many levels the selection could still descend. Zero means the primitive itself.
    int           remainingDepth = 0;
};

namespace detail {

/// Marches the field until it is within `eps` of a surface.
///
/// The step is scaled back for the same reason the renderer's is: Difference returns a lower bound
/// on the distance, so a full step can pass through a thin seam (PLAN.md R-03). A picker that
/// tunnels selects whatever is behind the thing that was clicked.
inline bool marchToSurface(const Scene& s, const double origin[3], const double dir[3],
                           double maxDistance, double eps, double& tOut) {
    double t = 0.0;
    for (int i = 0; i < 512; ++i) {
        double p[3];
        for (int k = 0; k < 3; ++k) {
            p[k] = origin[k] + dir[k] * t;
        }
        const double d = eval(s, p);
        if (isEmpty(d)) {
            return false;
        }
        if (d < eps) {
            tOut = t;
            return true;
        }
        t += d * 0.85;
        if (t > maxDistance) {
            return false;
        }
    }
    return false;
}

/// The primitive whose own surface passes closest to `p`.
///
/// Each primitive is measured in its own space with its ancestors' transforms undone, which is
/// what makes a scaled or rotated part answer for itself rather than for a copy at the origin.
inline std::uint16_t nearestPrimitive(const Scene& s, const double p[3]) {
    std::vector<PrimRef> prims;
    collectPrims(s, 0, ancestorMatrix(s, 0), prims, Fidelity{});

    std::uint16_t best = kNoChild;
    double bestDistance = 1e300;
    for (const PrimRef& ref : prims) {
        const double d = std::fabs(eval(s, ref.index, p));
        if (d < bestDistance) {
            bestDistance = d;
            best = ref.index;
        }
    }
    return best;
}

/// Ancestors of `index` from the root's direct child down to the node itself.
///
/// This is the ladder a click walks: entry 0 is what a plain click selects, and each modifier
/// press moves one step along it.
inline std::vector<std::uint16_t> selectionLadder(const Scene& s, std::uint16_t index) {
    std::vector<std::uint16_t> chain;
    for (std::uint16_t a = index; a != kNoParent; a = s.nodes[a].parent) {
        if (a == 0) {
            break;   // the root itself is never a selection
        }
        chain.push_back(a);
    }
    // Collected leaf-first; the ladder reads outermost-first.
    for (std::size_t i = 0, j = chain.size(); i + 1 < j; ++i, --j) {
        const std::uint16_t tmp = chain[i];
        chain[i] = chain[j - 1];
        chain[j - 1] = tmp;
    }
    return chain;
}

}  // namespace detail

/// Casts a ray and says what to select.
///
/// `depth` is how far down the hierarchy to go: 0 is the outermost group under the root, and each
/// further step descends one level toward the primitive. Clamped, so holding the modifier past the
/// bottom settles on the primitive rather than losing the selection.
inline PickResult pick(const Scene& s, const double origin[3], const double dir[3],
                       double maxDistance, int depth = 0) {
    PickResult r;
    if (s.nodes.count == 0) {
        return r;
    }

    const BoundsResult b = worldBounds(s);
    double radius = 1.0;
    if (b.box.valid) {
        double diag = 0.0;
        for (int i = 0; i < 3; ++i) {
            const double span = b.box.hi[i] - b.box.lo[i];
            diag += span * span;
        }
        radius = std::sqrt(diag) * 0.5;
        if (radius < 1e-6) {
            radius = 1e-6;
        }
    }

    double t = 0.0;
    if (!detail::marchToSurface(s, origin, dir, maxDistance, radius * 1e-4, t)) {
        return r;
    }

    r.hit = true;
    r.distance = t;
    for (int i = 0; i < 3; ++i) {
        r.point[i] = origin[i] + dir[i] * t;
    }

    const std::uint16_t prim = detail::nearestPrimitive(s, r.point);
    if (prim == kNoChild) {
        // The ray found a surface but nothing owns it. That means the tree holds geometry the
        // primitive walk does not know about, which is worth saying rather than papering over.
        r.hit = false;
        return r;
    }
    r.primitiveId = s.nodes[prim].id;

    const std::vector<std::uint16_t> ladder = detail::selectionLadder(s, prim);
    if (ladder.empty()) {
        r.id = s.nodes[prim].id;
        return r;
    }
    const int last = static_cast<int>(ladder.size()) - 1;
    const int step = depth < 0 ? 0 : (depth > last ? last : depth);
    r.id = s.nodes[ladder[static_cast<std::size_t>(step)]].id;
    r.remainingDepth = last - step;
    return r;
}

/// Picks through a camera, from a point on the screen.
///
/// `u`/`v` are fractions from the centre, -0.5..0.5, +v up -- the same convention `cameraRay`
/// takes, so a caller never converts between two of them.
inline PickResult pickThroughCamera(const Scene& s, const Camera& c, double u, double v,
                                    double aspect, int depth = 0) {
    double origin[3], dir[3];
    cameraRay(c, u, v, aspect, origin, dir);
    return pick(s, origin, dir, c.distance + c.farClip, depth);
}

}  // namespace makina
