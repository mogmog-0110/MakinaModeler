// CPU evaluator over the authoring tree: a port of Grasp3D's SceneSdf.java.
//
// This is the measurement and verification path, not the drawing path. The GPU walks a flattened
// program instead; this one stays close enough to the Java original that the two can be compared
// point by point, which is Phase 1's exit criterion.
//
// What callers may rely on, unchanged from the reference:
//   - the sign is exact: negative inside, positive outside
//   - the magnitude may be a conservative lower bound, because min/max CSG combination and a
//     non-uniform Scale corrected by its smallest axis both err on the safe side
//   - Plane is a half space (inside is y <= Y); Disc and Triangle have no thickness and therefore
//     no interior, so their distance is never negative
//   - an op with no geometry returns kEmpty
//
// Three behaviours here look odd until you check the original, and all three are deliberate:
//   - a transform node is a *container*: it unions its children after applying its inverse
//   - a primitive may also have children, and evaluates to min(its own surface, those children)
//   - Intersection skips empty children rather than letting one collapse the result
//
// One behaviour is *not* the original's: a Label's children are geometry here. SceneSdf returns
// empty for a Label and never descends, which no other part of Grasp3D does -- see Fidelity.hpp.
// Pass kGrasp3D to get the reference's answer back.

#pragma once

#include "Bounds.hpp"
#include "Fidelity.hpp"
#include "Scene.hpp"
#include "Sdf.hpp"
#include "SorProfile.hpp"
#include "SweepProfile.hpp"
#include "Warp.hpp"

#include <cmath>
#include <cstdint>

namespace makina {

constexpr double kEmpty = MK_EMPTY;
constexpr double kEmptyThreshold = MK_EMPTY_THRESHOLD;

inline bool isEmpty(double d) {
    return d >= kEmptyThreshold;
}

namespace detail {

/// SceneSdf.nz: a zero scale factor would divide by zero, so it is nudged rather than rejected.
inline double nz(double v) {
    return v == 0.0 ? 1e-9 : v;
}

/// Distance correction for a transform: the smallest absolute axis factor of a Scale, 1 otherwise.
///
/// A non-uniform Scale cannot be undone by a single factor, so the reference takes the smallest
/// one, which understates the distance. That is safe for sphere tracing but shortens the step, and
/// it is why heavily non-uniform models march slower (PLAN.md R-13).
inline double scaleFactorOf(const CsgNode& n) {
    if (static_cast<Op>(n.op) != Op::Scale) {
        return 1.0;
    }
    const double sx = std::fabs(nz(n.params[0]));
    const double sy = std::fabs(nz(n.params[1]));
    const double sz = std::fabs(nz(n.params[2]));
    return sx < sy ? (sx < sz ? sx : sz) : (sy < sz ? sy : sz);
}

inline int warpKindOf(Op op) {
    return op == Op::Twist ? MK_WARP_TWIST : (op == Op::Bend ? MK_WARP_BEND : MK_WARP_TAPER);
}

/// The rate a warp node stores, in the units Warp.hpp takes: radians per unit for the two that
/// turn, the plain ratio for Taper.
inline double warpRateOf(const CsgNode& n) {
    const double r = n.params[0];
    return static_cast<Op>(n.op) == Op::Taper ? r : r * 3.14159265358979323846 / 180.0;
}

/// Distance correction for a transform including a warp's Lipschitz bound, which needs the
/// subtree's extent (D-14) and therefore the scene. The factor is what the field is divided by
/// afterwards, so a warp contributes 1/L: the callers multiply corrections together and divide
/// the primitive distance by the product.
inline double scaleFactorOf(const Scene& s, std::uint16_t index) {
    const CsgNode& n = s.nodes[index];
    if (!isWarp(static_cast<Op>(n.op))) {
        return scaleFactorOf(n);
    }
    const WarpExtent e = warpExtent(s, index, Fidelity{});
    if (!e.valid) {
        return 1.0;
    }
    return 1.0 / mkWarpLipschitz(warpKindOf(static_cast<Op>(n.op)), warpRateOf(n), e.R, e.H);
}

/// Applies the inverse of one transform node to p. A warp also needs the subtree's reach along
/// its axis (Warp.hpp freezes the map beyond it), which the scene-aware overload supplies; this
/// one is for the affine transforms only.
inline void invApply(const CsgNode& n, const double p[3], double out[3]) {
    switch (static_cast<Op>(n.op)) {
        case Op::Translate:
            out[0] = p[0] - n.params[0];
            out[1] = p[1] - n.params[1];
            out[2] = p[2] - n.params[2];
            return;
        case Op::Scale:
            out[0] = p[0] / nz(n.params[0]);
            out[1] = p[1] / nz(n.params[1]);
            out[2] = p[2] / nz(n.params[2]);
            return;
        default:
            break;
    }

    // Rotate: single axis, angle negated. Grasp3D has no Euler rotation node; stacking Rotate
    // nodes is how a multi-axis rotation is expressed (CSG_NODE.md 4.2).
    const double a = -n.params[0] * 3.14159265358979323846 / 180.0;
    const double c = std::cos(a);
    const double s = std::sin(a);

    switch (n.flags & flags::kAxisMask) {
        case flags::kAxisY:
            out[0] = c * p[0] + s * p[2];
            out[1] = p[1];
            out[2] = -s * p[0] + c * p[2];
            return;
        case flags::kAxisZ:
            out[0] = c * p[0] - s * p[1];
            out[1] = s * p[0] + c * p[1];
            out[2] = p[2];
            return;
        default:
            out[0] = p[0];
            out[1] = c * p[1] - s * p[2];
            out[2] = s * p[1] + c * p[2];
            return;
    }
}

/// Applies the inverse of one transform node, warps included.
inline void invApply(const Scene& s, std::uint16_t index, const double p[3], double out[3]) {
    const CsgNode& n = s.nodes[index];
    if (!isWarp(static_cast<Op>(n.op))) {
        invApply(n, p, out);
        return;
    }
    const WarpExtent e = warpExtent(s, index, Fidelity{});
    mkWarpInv(warpKindOf(static_cast<Op>(n.op)), n.flags & flags::kAxisMask, warpRateOf(n),
              e.valid ? e.H : 0.0, p[0], p[1], p[2], out[0], out[1], out[2]);
}

/// The width of the band inside the guard where the field, not the guard, answers even though
/// the point may be past the children: the guard is kWarpGuardMargin times the children, and
/// the band is the difference along the radius.
inline double warpGuardBand(const Scene& s, std::uint16_t index) {
    const WarpExtent e = warpExtent(s, index, Fidelity{});
    if (!e.valid) {
        return 0.0;
    }
    return e.R * (1.0 - 1.0 / kWarpGuardMargin);
}

/// Distance from p (in the warp's own space) to the guard cylinder, or 0 inside it.
inline double warpBoxDistance(const Scene& s, std::uint16_t index, const double p[3]) {
    const WarpExtent e = warpExtent(s, index, Fidelity{});
    if (!e.valid) {
        return 0.0;
    }
    return mkWarpGuardDistance(s.nodes[index].flags & flags::kAxisMask, e.R, e.H, p[0], p[1], p[2]);
}

/// Surface distance of a primitive in its own local space. kEmpty for anything without geometry.
inline double primSdf(const CsgNode& n, const double p[3]) {
    const double x = p[0], y = p[1], z = p[2];
    const float* q = n.params;

    switch (static_cast<Op>(n.op)) {
        case Op::Box:
            return mkSdBox(x, y, z, q[0], q[1], q[2], q[3], q[4], q[5]);
        case Op::Sphere:
            return mkSdSphere(x, y, z, q[0]);
        case Op::Cylinder:
            // params order is capPoint, basePoint, radius (Op.hpp).
            return mkSdCylinder(x, y, z, q[2], q[1], q[0]);
        case Op::Cone:
            return mkSdCone(x, y, z, q[0], q[1]);
        case Op::Torus:
            return mkSdTorus(x, y, z, q[0], q[1]);
        case Op::Disc:
            return mkSdDisc(x, y, z, q[0], q[1]);
        case Op::Triangle:
            return mkSdTriangle(x, y, z, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8]);
        case Op::Plane:
            return mkSdPlane(y, q[0]);
        default:
            return kEmpty;  // Unsupported, and anything structural
    }
}

double evalNode(const Scene& s, std::uint16_t index, const double p[3], double scale, Fidelity f);

inline double unionChildren(const Scene& s, std::uint16_t index, const double p[3], double scale,
                            Fidelity f) {
    const CsgNode& n = s.nodes[index];
    double d = kEmpty;
    for (std::uint16_t i = 0; i < n.childCount; ++i) {
        const double c = evalNode(s, static_cast<std::uint16_t>(n.firstChild + i), p, scale, f);
        if (c < d) {
            d = c;
        }
    }
    return d;
}

/// d = max(first child, -min(the rest)). Labels do not count as the body.
inline double evalDifference(const Scene& s, std::uint16_t index, const double p[3], double scale,
                             Fidelity f) {
    const CsgNode& n = s.nodes[index];
    double base = kEmpty;
    double cut = kEmpty;
    bool haveBase = false;

    for (std::uint16_t i = 0; i < n.childCount; ++i) {
        const std::uint16_t child = static_cast<std::uint16_t>(n.firstChild + i);
        if (static_cast<Op>(s.nodes[child].op) == Op::Label) {
            continue;
        }
        const double d = evalNode(s, child, p, scale, f);
        if (!haveBase) {
            base = d;
            haveBase = true;
        } else if (d < cut) {
            cut = d;
        }
    }

    if (!haveBase) {
        return kEmpty;
    }
    if (isEmpty(cut)) {
        return base;
    }
    return base > -cut ? base : -cut;
}

inline double evalIntersection(const Scene& s, std::uint16_t index, const double p[3],
                               double scale, Fidelity f) {
    const CsgNode& n = s.nodes[index];
    double d = -kEmpty;
    bool any = false;

    for (std::uint16_t i = 0; i < n.childCount; ++i) {
        const std::uint16_t child = static_cast<std::uint16_t>(n.firstChild + i);
        if (static_cast<Op>(s.nodes[child].op) == Op::Label) {
            continue;
        }
        const double e = evalNode(s, child, p, scale, f);
        // An empty child means "not present", not "excludes everything": letting it through would
        // make one missing operand erase the whole intersection.
        if (isEmpty(e)) {
            continue;
        }
        any = true;
        if (e > d) {
            d = e;
        }
    }
    return any ? d : kEmpty;
}

// ------------------------------------------------------------------- blob field

/// What one walk over a Blob's components gathers at a point.
struct BlobTerms {
    double field = 0.0;      ///< sum of the component densities
    double lipschitz = 0.0;  ///< bound on |grad field| in blob space; the field/distance bridge
    double support = kEmpty; ///< distance to the nearest component support, never negative
};

/// Largest |d density / d r| of one component: 8*sqrt(3)/9 * |strength| / R, at r = R/sqrt(3).
inline double blobLipschitz(double radius, double strength) {
    return radius <= 0.0 ? 0.0 : 1.5396007178390020 * std::fabs(strength) / radius;
}

/// Distance squared from p to the segment a..b -- a blob cylinder's support is a capsule.
inline double segmentDistSq(const double p[3], const float* a, const float* b) {
    const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
    const double wx = p[0] - a[0], wy = p[1] - a[1], wz = p[2] - a[2];
    const double uu = ux * ux + uy * uy + uz * uz;
    double t = uu > 0.0 ? (wx * ux + wy * uy + wz * uz) / uu : 0.0;
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    const double dx = wx - t * ux, dy = wy - t * uy, dz = wz - t * uz;
    return dx * dx + dy * dy + dz * dz;
}

/// Adds one subtree's components to t. minScale carries the smallest axis factor of the
/// transforms crossed so far: dividing space by it stretches the field's gradient, so the
/// Lipschitz bound divides by it and the support distance multiplies, both erring safe.
inline void accumBlob(const Scene& s, std::uint16_t index, const double p[3], double minScale,
                      BlobTerms& t) {
    const CsgNode& n = s.nodes[index];
    const Op op = static_cast<Op>(n.op);

    if (isTransform(op)) {
        double q[3];
        invApply(s, index, p, q);
        const double k = minScale * scaleFactorOf(s, index);
        for (std::uint16_t i = 0; i < n.childCount; ++i) {
            accumBlob(s, static_cast<std::uint16_t>(n.firstChild + i), q, k, t);
        }
        return;
    }

    if (op == Op::BlobSphere || op == Op::BlobCylinder) {
        const float* q = n.params;
        double r2;
        double radius;
        double strength;
        if (op == Op::BlobSphere) {
            const double dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];
            r2 = dx * dx + dy * dy + dz * dz;
            radius = q[3];
            strength = q[4];
        } else {
            r2 = segmentDistSq(p, q, q + 3);
            radius = q[6];
            strength = q[7];
        }
        t.field += mkBlobFalloff(r2, radius, strength);
        t.lipschitz += blobLipschitz(radius, strength) / nz(minScale);
        const double sup = (std::sqrt(r2) - radius) * minScale;
        const double clamped = sup > 0.0 ? sup : 0.0;
        if (clamped < t.support) {
            t.support = clamped;
        }
        return;
    }

    // A Label or anything else structural: its children may still be components.
    for (std::uint16_t i = 0; i < n.childCount; ++i) {
        accumBlob(s, static_cast<std::uint16_t>(n.firstChild + i), p, minScale, t);
    }
}

/// The blob's distance estimate: (threshold - field) / Lipschitz, sharpened by the distance to
/// the union of supports, which the surface cannot leave. Both are lower bounds on the true
/// distance, so the larger one is the better step -- and both keep the sign convention exact,
/// since field > threshold is the inside by definition.
inline double evalBlob(const Scene& s, std::uint16_t index, const double p[3], double scale) {
    const CsgNode& n = s.nodes[index];
    BlobTerms t;
    for (std::uint16_t i = 0; i < n.childCount; ++i) {
        accumBlob(s, static_cast<std::uint16_t>(n.firstChild + i), p, 1.0, t);
    }
    if (t.lipschitz <= 0.0) {
        return kEmpty;
    }
    double d = (static_cast<double>(n.params[0]) - t.field) / t.lipschitz;
    // Only strictly outside every support: there the field is zero and the surface cannot be
    // nearer than the supports are. Inside the union the support distance is zero, and letting a
    // zero through the max would erase the negative interior distance.
    if (!isEmpty(t.support) && t.support > 0.0 && t.support > d) {
        d = t.support;
    }
    return d * scale;
}

// -------------------------------------------------------------- revolved profile

/// The revolved solid's distance: exact in the meridian plane, so the 3D answer is the 2D
/// distance from (rho, y) to the full cross-section -- both profile sides, so the axis is
/// interior and never mistaken for surface.
///
/// The profile is POV's: a cubic in r-squared over h through the interior points, the first and
/// last points steering the end slopes (public reference). SorProfile.hpp walks it into a
/// polyline and holds the distance; the sor-silhouette comparison against POV is the
/// measurement that this is the curve POV traces.
inline double evalSor(const Scene& s, std::uint16_t index, const double p[3], double scale) {
    double side[kMaxSorSide][2];
    const int num = sorPolyline(s, index, side);
    if (num == 0) {
        return kEmpty;
    }
    const double rho = std::sqrt(p[0] * p[0] + p[2] * p[2]);
    return sorSideDistance(&side[0][0], num, rho, p[1]) * scale;
}

/// The swept solid's distance: the least distance to any round-cone link of the sampled path.
/// Each link is exact (Sdf.hpp), so the only approximation is the sampling of the curve, which
/// the sweep-silhouette comparison against POV watches.
inline double evalSweep(const Scene& s, std::uint16_t index, const double p[3], double scale) {
    double samples[kMaxSweepSamples][4];
    const int num = sweepSamples(s, index, samples);
    if (num < 2) {
        return kEmpty;
    }
    double d = kEmpty;
    for (int i = 0; i + 1 < num; ++i) {
        const double* a = samples[i];
        const double* b = samples[i + 1];
        const double e = mkSdRoundCone(p[0], p[1], p[2], a[0], a[1], a[2], b[0], b[1], b[2],
                                       a[3], b[3]);
        if (e < d) {
            d = e;
        }
    }
    return d * scale;
}

inline double evalNode(const Scene& s, std::uint16_t index, const double p[3], double scale,
                       Fidelity f) {
    const CsgNode& n = s.nodes[index];
    const Op op = static_cast<Op>(n.op);

    if (!f.labelsAreGeometry && op == Op::Label) {
        return kEmpty;
    }

    if (op == Op::Blob) {
        return evalBlob(s, index, p, scale);
    }
    if (op == Op::Sor) {
        return evalSor(s, index, p, scale);
    }
    if (op == Op::SphereSweep) {
        return evalSweep(s, index, p, scale);
    }
    // A control point contributes through its Sor's or its sweep's walk and nowhere else.
    if (op == Op::SorPoint || op == Op::SweepPoint) {
        return kEmpty;
    }
    // A component contributes through its Blob's field walk and nowhere else: evaluated on its
    // own it would put a second, un-blended surface where the blob already has one.
    if (isBlobComponent(op)) {
        return kEmpty;
    }

    if (isWarp(op)) {
        // Outside the warp's own box the Lipschitz bound is not sized for the point (the shear
        // of a taper grows with distance from the axis, without limit), so the box's distance
        // stands in: it is a true lower bound on the distance to anything inside, and a march
        // that far out only needs to know it may step that far. Strictly outside only -- inside
        // the box the distance is 0 and would erase the field (memory: lower-bound-two-traps).
        // Only when clear of the cube by its margin: the guard's own wall must never be the
        // nearest thing a march sees, or it draws the wall (it did). Inside the margin band the
        // field is live and L is sized for it (warpExtent).
        const double outside = warpBoxDistance(s, index, p);
        if (outside > warpGuardBand(s, index)) {
            return outside * scale;
        }
    }
    if (isTransform(op)) {
        double q[3];
        invApply(s, index, p, q);
        return unionChildren(s, index, q, scale * scaleFactorOf(s, index), f);
    }

    if (op == Op::Difference) {
        return evalDifference(s, index, p, scale, f);
    }
    if (op == Op::Intersection) {
        return evalIntersection(s, index, p, scale, f);
    }

    // Merge, SceneRoot, Unsupported, and every primitive. A primitive with children contributes
    // both its own surface and theirs.
    double d = kEmpty;
    if (isPrimitive(op)) {
        const double pd = primSdf(n, p);
        if (!isEmpty(pd)) {
            d = pd * scale;
        }
    }
    const double dc = unionChildren(s, index, p, scale, f);
    return d < dc ? d : dc;
}

}  // namespace detail

/// Signed distance from the world point wp to the subtree at index, ancestor transforms included.
/// Returns kEmpty when the subtree contributes no geometry.
inline double eval(const Scene& s, std::uint16_t index, const double wp[3], Fidelity f = {}) {
    // Collect the ancestor chain, then apply the inverses root first: a transform closer to the
    // root has to be undone before one closer to the node.
    std::uint16_t chain[64];
    int depth = 0;
    for (std::uint16_t a = s.nodes[index].parent;
         a != kNoParent && depth < 64;
         a = s.nodes[a].parent) {
        chain[depth++] = a;
    }

    double p[3] = {wp[0], wp[1], wp[2]};
    double scale = 1.0;
    for (int i = depth - 1; i >= 0; --i) {
        const CsgNode& n = s.nodes[chain[i]];
        if (isTransform(static_cast<Op>(n.op))) {
            double q[3];
            detail::invApply(s, chain[i], p, q);
            p[0] = q[0];
            p[1] = q[1];
            p[2] = q[2];
            scale *= detail::scaleFactorOf(s, chain[i]);
        }
    }

    return detail::evalNode(s, index, p, scale, f);
}

/// Convenience: evaluate the whole scene from its root.
inline double eval(const Scene& s, const double wp[3], Fidelity f = {}) {
    return s.nodes.count == 0 ? kEmpty : eval(s, 0, wp, f);
}

}  // namespace makina
