// World axis-aligned bounds of a subtree: a port of Grasp3D's SceneBounds, with one deliberate
// improvement, selectable through Fidelity.
//
// Grasp3D treats Difference and Intersection as plain containers and takes the union of their
// children's boxes, which its own comment calls "estimated conservatively". That is sound but
// loose. Here the boolean is respected:
//
//   Difference    A - B is a subset of A            -> the box of the first child alone
//   Intersection  A & B is a subset of both         -> the intersection of the children's boxes
//   Merge         the union                         -> unchanged
//
// Both replacements are strictly tighter and still enclose the true result, so nothing that was
// correct against the loose box becomes incorrect against this one. It matters later: these boxes
// are what subtree culling and the measurement commands work from, and a box inflated by a
// subtracted blade that contributes nothing is a box that never culls.
//
// This is a deliberate divergence from the reference (PLAN.md D-11, category B) and are recorded
// in PORT_STATUS.md rather than left for someone to discover.

#pragma once

#include "Fidelity.hpp"
#include "Scene.hpp"
#include "SorProfile.hpp"
#include "SweepProfile.hpp"

#include <cmath>
#include <cstdint>

namespace makina {

struct Aabb {
    double lo[3];
    double hi[3];
    bool   valid;   ///< false when the subtree contributes no geometry

    [[nodiscard]] bool contains(const double p[3], double slack = 0.0) const {
        if (!valid) {
            return false;
        }
        for (int i = 0; i < 3; ++i) {
            if (p[i] < lo[i] - slack || p[i] > hi[i] + slack) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool contains(const Aabb& inner) const {
        if (!inner.valid) {
            return true;   // nothing to contain
        }
        if (!valid) {
            return false;
        }
        for (int i = 0; i < 3; ++i) {
            if (inner.lo[i] < lo[i] || inner.hi[i] > hi[i]) {
                return false;
            }
        }
        return true;
    }
};

inline Aabb emptyAabb() {
    Aabb a{};
    a.valid = false;
    for (int i = 0; i < 3; ++i) {
        a.lo[i] = 0.0;
        a.hi[i] = 0.0;
    }
    return a;
}

namespace detail {

/// 4x4 row-major, matching SceneMatrix.
struct Mat4 {
    double m[16];
};

inline Mat4 identityMat() {
    return Mat4{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
}

inline Mat4 mulMat(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[i * 4 + k] * b.m[k * 4 + j];
            }
            r.m[i * 4 + j] = sum;
        }
    }
    return r;
}

inline void applyMat(const Mat4& m, const double p[3], double out[3]) {
    out[0] = m.m[0] * p[0] + m.m[1] * p[1] + m.m[2] * p[2] + m.m[3];
    out[1] = m.m[4] * p[0] + m.m[5] * p[1] + m.m[6] * p[2] + m.m[7];
    out[2] = m.m[8] * p[0] + m.m[9] * p[1] + m.m[10] * p[2] + m.m[11];
}

/// Forward transform of one node. Angles are degrees, right-handed, matching glRotate.
inline Mat4 matrixOf(const CsgNode& n) {
    switch (static_cast<Op>(n.op)) {
        case Op::Translate:
            return Mat4{{1, 0, 0, n.params[0],
                         0, 1, 0, n.params[1],
                         0, 0, 1, n.params[2],
                         0, 0, 0, 1}};
        case Op::Scale:
            return Mat4{{n.params[0], 0, 0, 0,
                         0, n.params[1], 0, 0,
                         0, 0, n.params[2], 0,
                         0, 0, 0, 1}};
        default:
            break;
    }

    const double a = n.params[0] * 3.14159265358979323846 / 180.0;
    const double c = std::cos(a);
    const double s = std::sin(a);
    switch (n.flags & flags::kAxisMask) {
        case flags::kAxisY:
            return Mat4{{c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0, 0, 0, 0, 1}};
        case flags::kAxisZ:
            return Mat4{{c, -s, 0, 0, s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
        default:
            return Mat4{{1, 0, 0, 0, 0, c, -s, 0, 0, s, c, 0, 0, 0, 0, 1}};
    }
}

inline void expand(Aabb& a, const double p[3]) {
    if (!a.valid) {
        a.valid = true;
        for (int i = 0; i < 3; ++i) {
            a.lo[i] = a.hi[i] = p[i];
        }
        return;
    }
    for (int i = 0; i < 3; ++i) {
        if (p[i] < a.lo[i]) a.lo[i] = p[i];
        if (p[i] > a.hi[i]) a.hi[i] = p[i];
    }
}

inline Aabb uniteAabb(const Aabb& a, const Aabb& b) {
    if (!a.valid) return b;
    if (!b.valid) return a;
    Aabb r = a;
    for (int i = 0; i < 3; ++i) {
        if (b.lo[i] < r.lo[i]) r.lo[i] = b.lo[i];
        if (b.hi[i] > r.hi[i]) r.hi[i] = b.hi[i];
    }
    return r;
}

inline Aabb intersectAabb(const Aabb& a, const Aabb& b) {
    if (!a.valid || !b.valid) {
        return emptyAabb();
    }
    Aabb r{};
    r.valid = true;
    for (int i = 0; i < 3; ++i) {
        r.lo[i] = a.lo[i] > b.lo[i] ? a.lo[i] : b.lo[i];
        r.hi[i] = a.hi[i] < b.hi[i] ? a.hi[i] : b.hi[i];
        if (r.lo[i] > r.hi[i]) {
            // Disjoint: the intersection really is empty, which is information worth keeping
            // rather than collapsing to a degenerate point.
            return emptyAabb();
        }
    }
    return r;
}

/// Local AABB corners of a primitive, before any transform. Returns 0 for anything unbounded or
/// without geometry: Plane is infinite, Unsupported has no shape.
inline int localCorners(const CsgNode& n, double out[8][3]) {
    const float* q = n.params;
    double x1, y1, z1, x2, y2, z2;

    switch (static_cast<Op>(n.op)) {
        case Op::Box:
            x1 = q[0]; y1 = q[1]; z1 = q[2];
            x2 = q[3]; y2 = q[4]; z2 = q[5];
            break;
        case Op::Sphere:
            x1 = y1 = z1 = -q[0];
            x2 = y2 = z2 = q[0];
            break;
        case Op::Cylinder:
            // params are capPoint, basePoint, radius.
            x1 = -q[2]; y1 = q[1]; z1 = -q[2];
            x2 = q[2];  y2 = q[0]; z2 = q[2];
            break;
        case Op::Cone:
            x1 = -q[0]; y1 = 0.0;  z1 = -q[0];
            x2 = q[0];  y2 = q[1]; z2 = q[0];
            break;
        case Op::Torus: {
            const double o = static_cast<double>(q[0]) + q[1];
            x1 = -o; y1 = -q[1]; z1 = -o;
            x2 = o;  y2 = q[1];  z2 = o;
            break;
        }
        case Op::Disc:
            // The hole does not extend the box.
            x1 = -q[0]; y1 = 0.0; z1 = -q[0];
            x2 = q[0];  y2 = 0.0; z2 = q[0];
            break;
        // Blob components: the box of the support, outside which the density is exactly zero.
        // The blended surface can bulge past one component but never past its support.
        case Op::BlobSphere:
            x1 = q[0] - q[3]; y1 = q[1] - q[3]; z1 = q[2] - q[3];
            x2 = q[0] + q[3]; y2 = q[1] + q[3]; z2 = q[2] + q[3];
            break;
        case Op::BlobCylinder: {
            const double r = q[6];
            x1 = (q[0] < q[3] ? q[0] : q[3]) - r;
            y1 = (q[1] < q[4] ? q[1] : q[4]) - r;
            z1 = (q[2] < q[5] ? q[2] : q[5]) - r;
            x2 = (q[0] > q[3] ? q[0] : q[3]) + r;
            y2 = (q[1] > q[4] ? q[1] : q[4]) + r;
            z2 = (q[2] > q[5] ? q[2] : q[5]) + r;
            break;
        }
        case Op::Triangle:
            out[0][0] = q[0]; out[0][1] = q[1]; out[0][2] = q[2];
            out[1][0] = q[3]; out[1][1] = q[4]; out[1][2] = q[5];
            out[2][0] = q[6]; out[2][1] = q[7]; out[2][2] = q[8];
            return 3;
        default:
            return 0;
    }

    const double ax = x1 < x2 ? x1 : x2, bx = x1 < x2 ? x2 : x1;
    const double ay = y1 < y2 ? y1 : y2, by = y1 < y2 ? y2 : y1;
    const double az = z1 < z2 ? z1 : z2, bz = z1 < z2 ? z2 : z1;

    const double xs[2] = {ax, bx};
    const double ys[2] = {ay, by};
    const double zs[2] = {az, bz};
    int k = 0;
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            for (int l = 0; l < 2; ++l) {
                out[k][0] = xs[i];
                out[k][1] = ys[j];
                out[k][2] = zs[l];
                ++k;
            }
        }
    }
    return 8;
}

Aabb subtreeBounds(const Scene& s, std::uint16_t index, const Mat4& m, int& primitiveCount,
                   Fidelity f);

inline Aabb childrenBounds(const Scene& s, std::uint16_t index, const Mat4& m, int& count,
                           Fidelity f) {
    const CsgNode& n = s.nodes[index];
    Aabb acc = emptyAabb();
    for (std::uint16_t i = 0; i < n.childCount; ++i) {
        acc = uniteAabb(acc, subtreeBounds(s, static_cast<std::uint16_t>(n.firstChild + i), m,
                                           count, f));
    }
    return acc;
}

inline Aabb subtreeBounds(const Scene& s, std::uint16_t index, const Mat4& m, int& primitiveCount,
                          Fidelity f) {
    const CsgNode& n = s.nodes[index];
    const Op op = static_cast<Op>(n.op);

    if (isTransform(op)) {
        const Mat4 mc = mulMat(m, matrixOf(n));
        return childrenBounds(s, index, mc, primitiveCount, f);
    }

    if (f.tightBounds && op == Op::Difference) {
        // Subtracting cannot grow the result, so only the body counts. Labels are not the body.
        for (std::uint16_t i = 0; i < n.childCount; ++i) {
            const std::uint16_t child = static_cast<std::uint16_t>(n.firstChild + i);
            if (static_cast<Op>(s.nodes[child].op) == Op::Label) {
                continue;
            }
            return subtreeBounds(s, child, m, primitiveCount, f);
        }
        return emptyAabb();
    }

    if (f.tightBounds && op == Op::Intersection) {
        Aabb acc = emptyAabb();
        bool any = false;
        for (std::uint16_t i = 0; i < n.childCount; ++i) {
            const std::uint16_t child = static_cast<std::uint16_t>(n.firstChild + i);
            if (static_cast<Op>(s.nodes[child].op) == Op::Label) {
                continue;
            }
            const Aabb b = subtreeBounds(s, child, m, primitiveCount, f);
            if (!b.valid) {
                // An unbounded or empty operand cannot narrow the result; skip it, matching how
                // the evaluator skips empty children of an Intersection.
                continue;
            }
            acc = any ? intersectAabb(acc, b) : b;
            any = true;
        }
        return any ? acc : emptyAabb();
    }

    if (op == Op::Sor) {
        // The spline in r-squared can swing past its control points, so the box comes from the
        // Hermite bound per segment: max endpoint value plus 4/27 * dh * (|m0| + |m1|), which no
        // point of a cubic Hermite exceeds. Heights cannot overshoot -- the curve is a function
        // of h -- so Y spans the interior points exactly.
        double r2[kMaxSorPoints];
        double h[kMaxSorPoints];
        const int count = sorControls(s, index, r2, h);
        if (count < 4) {
            return emptyAabb();
        }
        double maxR2 = 0.0;
        for (int i = 1; i + 2 < count; ++i) {
            double dh, m0, m1;
            sorSegment(r2, h, i, dh, m0, m1);
            const double top = (r2[i] > r2[i + 1] ? r2[i] : r2[i + 1]) +
                               (4.0 / 27.0) * std::fabs(dh) * (std::fabs(m0) + std::fabs(m1));
            if (top > maxR2) {
                maxR2 = top;
            }
        }
        const double r = std::sqrt(maxR2 > 0.0 ? maxR2 : 0.0);
        const double corners[8][3] = {
            {-r, h[1], -r}, {r, h[1], -r}, {-r, h[1], r}, {r, h[1], r},
            {-r, h[count - 2], -r}, {r, h[count - 2], -r},
            {-r, h[count - 2], r}, {r, h[count - 2], r}};
        Aabb box = emptyAabb();
        for (const double* c : corners) {
            double w[3];
            applyMat(m, c, w);
            expand(box, w);
        }
        ++primitiveCount;
        return box;
    }

    if (op == Op::SphereSweep) {
        // A B-spline stays inside its control hull (and linear trivially does), radii included,
        // so the hull box grown by the largest radius contains the whole envelope.
        double pts[kMaxSweepPoints][4];
        const int count = sweepControls(s, index, pts);
        if (count == 0) {
            return emptyAabb();
        }
        double lo[3] = {pts[0][0], pts[0][1], pts[0][2]};
        double hi[3] = {lo[0], lo[1], lo[2]};
        double maxR = 0.0;
        for (int i = 0; i < count; ++i) {
            for (int k = 0; k < 3; ++k) {
                if (pts[i][k] < lo[k]) lo[k] = pts[i][k];
                if (pts[i][k] > hi[k]) hi[k] = pts[i][k];
            }
            if (pts[i][3] > maxR) {
                maxR = pts[i][3];
            }
        }
        Aabb box = emptyAabb();
        for (int c = 0; c < 8; ++c) {
            const double corner[3] = {(c & 1 ? hi[0] : lo[0]) + (c & 1 ? maxR : -maxR),
                                      (c & 2 ? hi[1] : lo[1]) + (c & 2 ? maxR : -maxR),
                                      (c & 4 ? hi[2] : lo[2]) + (c & 4 ? maxR : -maxR)};
            double w[3];
            applyMat(m, corner, w);
            expand(box, w);
        }
        ++primitiveCount;
        return box;
    }

    Aabb own = emptyAabb();
    if (isPrimitive(op) || isBlobComponent(op)) {
        double pts[8][3];
        const int count = localCorners(n, pts);
        if (count > 0) {
            ++primitiveCount;
            for (int i = 0; i < count; ++i) {
                double w[3];
                applyMat(m, pts[i], w);
                expand(own, w);
            }
        }
    }

    return uniteAabb(own, childrenBounds(s, index, m, primitiveCount, f));
}

}  // namespace detail

struct BoundsResult {
    Aabb box;
    /// Primitives that actually contributed to the box, which under the default Fidelity is fewer
    /// than the scene holds: a subtracted blade never widens the result, so it is never visited.
    /// Grasp3D's equivalent counts every primitive it walks past. Use Scene::nodeCount if what
    /// you want is "how big is this model".
    int  primitiveCount;
};

/// World bounds of the subtree at index, ancestor transforms included.
///
/// Pass kGrasp3D for the reference's answers; see Fidelity.
inline BoundsResult worldBounds(const Scene& s, std::uint16_t index, Fidelity f = {}) {
    // Compose the ancestor transforms root first; the node itself is excluded.
    std::uint16_t chain[64];
    int depth = 0;
    for (std::uint16_t a = s.nodes[index].parent;
         a != kNoParent && depth < 64;
         a = s.nodes[a].parent) {
        chain[depth++] = a;
    }

    detail::Mat4 m = detail::identityMat();
    for (int i = depth - 1; i >= 0; --i) {
        const CsgNode& n = s.nodes[chain[i]];
        if (isTransform(static_cast<Op>(n.op))) {
            m = detail::mulMat(m, detail::matrixOf(n));
        }
    }

    BoundsResult r{};
    r.primitiveCount = 0;
    r.box = detail::subtreeBounds(s, index, m, r.primitiveCount, f);
    return r;
}

inline BoundsResult worldBounds(const Scene& s, Fidelity f = {}) {
    if (s.nodes.count == 0) {
        BoundsResult r{};
        r.box = emptyAabb();
        r.primitiveCount = 0;
        return r;
    }
    return worldBounds(s, 0, f);
}

}  // namespace makina
