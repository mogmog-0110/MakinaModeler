// The CSG tree as a set of closed polygon solids. The tree-walking half of Grasp3D's CsgTess.
//
// The result is a *list* of solids, not one mesh. That is not a convenience: a BSP's inside test
// is only defined for a single closed solid, so two overlapping solids merged into one tree give
// wrong answers for every boolean afterwards. Keeping them apart and distributing the operation
// over them is what makes the result correct:
//
//   (A|B) - C = (A-C) | (B-C)
//   p - (C|D) = (p-C) - D
//   (A|B) & C = (A&C) | (B&C)
//
// A point is inside the scene when it is inside any solid in the list, which is the same union the
// SDF evaluator takes.

#pragma once

#include "Bounds.hpp"
#include "Bsp.hpp"
#include "Fidelity.hpp"
#include "Op.hpp"
#include "Scene.hpp"
#include "Surface.hpp"
#include "Tessellate.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace makina {

/// What a tessellation run produced, and what it could not.
///
/// `complete` false means some subtree has no solid form -- a Plane, an unsupported node, a
/// degenerate cone. The solids that were produced are still valid; what is not valid is treating
/// the list as the whole scene. A caller checking the SDF against this has to stop, not carry on
/// with a mesh that is missing a part.
struct TessellationResult {
    std::vector<BspSolid> solids;
    bool                  complete = true;
    std::string           missing;   ///< op name of the first thing that had no solid form
};

namespace detail {

void collectSolids(const Scene& s, std::uint16_t index, const Mat4& m, TessellationResult& r,
                   Fidelity f);

/// Evaluates a Difference or an Intersection down to the solids it produces.
inline std::vector<BspSolid> booleanSolids(const Scene& s, std::uint16_t index, const Mat4& m,
                                           TessellationResult& r, Fidelity f) {
    const CsgNode& n = s.nodes[index];
    const Op op = static_cast<Op>(n.op);

    // One direct child is one operand; a child holding several solids is their union.
    std::vector<std::vector<BspSolid>> operands;
    for (std::uint16_t i = 0; i < n.childCount; ++i) {
        const std::uint16_t c = static_cast<std::uint16_t>(n.firstChild + i);
        // A Label is an annotation, never an operand -- see Fidelity.hpp. Its children are
        // geometry, but they belong to whatever encloses the boolean, not to the boolean.
        if (static_cast<Op>(s.nodes[c].op) == Op::Label) {
            continue;
        }
        TessellationResult sub;
        collectSolids(s, c, m, sub, f);
        if (!sub.complete) {
            r.complete = false;
            if (r.missing.empty()) {
                r.missing = sub.missing;
            }
        }
        if (!sub.solids.empty()) {
            operands.push_back(std::move(sub.solids));
        }
    }
    if (operands.empty()) {
        return {};
    }

    if (op == Op::Difference) {
        std::vector<BspSolid> out;
        for (const BspSolid& body : operands[0]) {
            BspSolid acc = body;
            for (std::size_t i = 1; i < operands.size(); ++i) {
                for (const BspSolid& blade : operands[i]) {
                    acc = bspSubtract(acc, blade);
                }
            }
            // The cut surface arrives wearing the blade's material. Repaint it with the body's,
            // matching what POV is asked to do with cutaway_textures (Pov.hpp).
            const std::uint16_t bodyPrim = body.empty() ? 0 : body[0].shared;
            for (BspPoly& p : acc) {
                bool ownFace = false;
                for (const BspPoly& b : body) {
                    if (b.shared == p.shared) {
                        ownFace = true;
                        break;
                    }
                }
                if (!ownFace) {
                    p.shared = bodyPrim;
                }
            }
            if (!acc.empty()) {
                out.push_back(std::move(acc));
            }
        }
        return out;
    }

    if (op == Op::Intersection) {
        std::vector<BspSolid> acc = operands[0];
        for (std::size_t i = 1; i < operands.size(); ++i) {
            std::vector<BspSolid> next;
            for (const BspSolid& a : acc) {
                for (const BspSolid& b : operands[i]) {
                    BspSolid piece = bspIntersect(a, b);
                    if (!piece.empty()) {
                        next.push_back(std::move(piece));
                    }
                }
            }
            acc = std::move(next);
            if (acc.empty()) {
                return {};
            }
        }
        return acc;
    }

    return {};
}

inline void collectSolids(const Scene& s, std::uint16_t index, const Mat4& m,
                          TessellationResult& r, Fidelity f) {
    const CsgNode& n = s.nodes[index];
    const Op op = static_cast<Op>(n.op);

    if (op == Op::Difference || op == Op::Intersection) {
        std::vector<BspSolid> made = booleanSolids(s, index, m, r, f);
        for (BspSolid& solid : made) {
            r.solids.push_back(std::move(solid));
        }
        return;
    }

    if (!f.labelsAreGeometry && op == Op::Label) {
        return;
    }

    Mat4 mc = m;
    if (isTransform(op)) {
        mc = mulMat(m, matrixOf(n));
    } else if (isPrimitive(op) || op == Op::Unsupported) {
        BspSolid solid;
        if (tessellatePrimitive(n, index, m, solid)) {
            r.solids.push_back(std::move(solid));
        } else {
            r.complete = false;
            if (r.missing.empty()) {
                r.missing = opName(static_cast<Op>(n.op));
            }
        }
    }

    for (std::uint16_t i = 0; i < n.childCount; ++i) {
        collectSolids(s, static_cast<std::uint16_t>(n.firstChild + i), mc, r, f);
    }
}

}  // namespace detail

/// Every closed solid the subtree at `index` produces, in world space.
inline TessellationResult tessellate(const Scene& s, std::uint16_t index, Fidelity f = {}) {
    TessellationResult r;
    if (s.nodes.count == 0) {
        return r;
    }
    detail::collectSolids(s, index, detail::ancestorMatrix(s, index), r, f);
    return r;
}

inline TessellationResult tessellate(const Scene& s, Fidelity f = {}) {
    return tessellate(s, 0, f);
}

namespace detail {

/// Crossings of a ray from `p` along `dir` with one convex polygon: 1 or 0.
///
/// Returns -1 when the answer cannot be trusted -- the ray runs in the polygon's plane, or passes
/// within `kGrazeEps` of an edge. A grazing hit is the one case parity counting gets wrong, by
/// counting a shared edge twice or not at all, so it is reported rather than guessed at.
inline int rayCrossesPoly(const BspPoly& poly, const double p[3], const double dir[3]) {
    constexpr double kGrazeEps = 1e-9;

    const double denom = poly.nx * dir[0] + poly.ny * dir[1] + poly.nz * dir[2];
    if (std::fabs(denom) < 1e-12) {
        const double d = poly.nx * p[0] + poly.ny * p[1] + poly.nz * p[2] - poly.w;
        return std::fabs(d) < 1e-9 ? -1 : 0;   // in the plane: no crossing can be defined
    }
    const double t = (poly.w - (poly.nx * p[0] + poly.ny * p[1] + poly.nz * p[2])) / denom;
    if (t <= 0.0) {
        return 0;
    }

    const double hit[3] = {p[0] + dir[0] * t, p[1] + dir[1] * t, p[2] + dir[2] * t};

    // Inside a convex polygon means the same side of every edge, measured against the plane
    // normal. Working in 3D avoids picking a projection axis and getting it wrong on a face that
    // is nearly parallel to it.
    const std::size_t n = poly.v.size();
    for (std::size_t i = 0; i < n; ++i) {
        const double* a = poly.v[i].p;
        const double* b = poly.v[(i + 1) % n].p;
        const double e[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const double r[3] = {hit[0] - a[0], hit[1] - a[1], hit[2] - a[2]};
        const double c[3] = {e[1] * r[2] - e[2] * r[1], e[2] * r[0] - e[0] * r[2],
                             e[0] * r[1] - e[1] * r[0]};
        const double side = c[0] * poly.nx + c[1] * poly.ny + c[2] * poly.nz;
        if (side < -kGrazeEps) {
            return 0;
        }
        if (side < kGrazeEps) {
            return -1;   // on an edge
        }
    }
    return 1;
}

}  // namespace detail

/// Where a point sits relative to a boundary representation.
///
/// `Grazing` is a real answer, not a failure: a ray that clips an edge cannot be counted by
/// parity, and a caller comparing this against an SDF must drop that sample rather than record a
/// disagreement that says nothing.
enum class Containment { Outside, Inside, Grazing };

/// Parity of a ray cast from `p`.
///
/// The direction is fixed rather than random, so a disagreement is reproducible from the sample
/// coordinates alone. It is deliberately not axis-aligned: these scenes are full of boxes whose
/// faces are exactly parallel to the axes, and a ray running along one of those grazes every
/// polygon it meets.
inline Containment containedBySolids(const std::vector<BspSolid>& solids, const double p[3]) {
    constexpr double kDir[3] = {0.7302967433402214, 0.5477225575051661, 0.4082482904638631};

    for (const BspSolid& solid : solids) {
        int crossings = 0;
        for (const BspPoly& poly : solid) {
            const int c = detail::rayCrossesPoly(poly, p, kDir);
            if (c < 0) {
                return Containment::Grazing;
            }
            crossings += c;
        }
        if ((crossings & 1) != 0) {
            return Containment::Inside;   // inside any solid is inside the union
        }
    }
    return Containment::Outside;
}

/// How far the tessellation can be from the surface the SDF describes.
///
/// A circle of radius r drawn with kSegments chords bulges inward by r(1 - cos(pi/kSegments)) at
/// the midpoint of each chord, and that sagitta is the dominant error -- flat faces are exact and
/// the sphere's latitude bands are the same construction. Scaled by the scene's own size, since a
/// millimetre matters on a bottle and not on a building.
inline double tessellationError(double sceneRadius) {
    const double sagitta = 1.0 - std::cos(detail::kPi / kSegments);
    return sceneRadius * sagitta;
}

}  // namespace makina
