// Primitives as closed polygon solids, and the CSG tree as boolean operations between them.
// A port of Grasp3D's CsgTess.
//
// Facet counts are fixed (kSegments around a circle, kLatitudes down a sphere), so this is an
// approximation of the surface the SDF describes exactly. That is the point: the two are meant to
// agree away from the surface, and where they disagree the tessellation error bound says whether
// the disagreement is real. bsp_compare works exactly that way.
//
// Two structural rules make the booleans behave, and both come from the same fact: a BSP's
// inside test is only valid for one closed solid at a time. Merging two overlapping solids into
// one tree destroys it. So the work is always decomposed into operations between closed solids:
//
//   (A|B) - C = (A-C) | (B-C)     a body of several solids is subtracted from one at a time
//   p - (C|D) = (p-C) - D         several blades are subtracted one after another
//   (A|B) & C = (A&C) | (B&C)     intersection distributes the same way
//
// Plane is unbounded and cannot be a solid, so a subtree containing one has no tessellation. The
// caller gets `false` rather than a silently wrong mesh.

#pragma once

#include "Bsp.hpp"
#include "Bounds.hpp"
#include "Op.hpp"
#include "PovShape.hpp"
#include "Scene.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

namespace makina {

/// Segments around a full circle. Raising it costs BSP time superlinearly -- every facet is a
/// splitting plane -- so it is a preview-grade number, matching the reference.
constexpr int kSegments = 24;
/// Latitude bands from pole to pole on a sphere.
constexpr int kLatitudes = 14;
/// Bands around the tube of a torus.
constexpr int kTubeBands = 12;

/// One closed solid.
using BspSolid = std::vector<BspPoly>;

namespace detail {

constexpr double kPi = 3.14159265358979323846;

inline BspVertex vert(double x, double y, double z, double nx, double ny, double nz) {
    BspVertex v{};
    v.p[0] = x;
    v.p[1] = y;
    v.p[2] = z;
    v.n[0] = nx;
    v.n[1] = ny;
    v.n[2] = nz;
    return v;
}

inline void addPoly(BspSolid& out, std::vector<BspVertex> vs, std::uint16_t shared) {
    if (vs.size() < 3) {
        return;
    }
    BspPoly p;
    p.v = std::move(vs);
    p.shared = shared;
    p.computePlane();
    out.push_back(std::move(p));
}

/// A flat polygon whose vertices all carry the same normal.
inline void addFlat(BspSolid& out, const double (*pts)[3], int count, double nx, double ny,
                    double nz, std::uint16_t shared) {
    std::vector<BspVertex> vs;
    vs.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        vs.push_back(vert(pts[i][0], pts[i][1], pts[i][2], nx, ny, nz));
    }
    addPoly(out, std::move(vs), shared);
}

inline BspVertex spherePoint(double r, double t, double p) {
    const double x = std::sin(t) * std::cos(p);
    const double y = std::cos(t);
    const double z = std::sin(t) * std::sin(p);
    return vert(r * x, r * y, r * z, x, y, z);
}

inline BspVertex torusPoint(double R, double r, double a, double b) {
    const double cx = std::cos(a), cz = std::sin(a);
    const double nx = std::cos(b) * cx, ny = std::sin(b), nz = std::cos(b) * cz;
    return vert((R + r * std::cos(b)) * cx, r * std::sin(b), (R + r * std::cos(b)) * cz, nx, ny,
                nz);
}

/// Bakes the transform into every vertex and normalises the winding afterwards.
///
/// The normal is transformed by the upper-left 3x3 and renormalised, which is right for the rigid
/// and uniform-scale transforms Grasp3D offers. A non-uniform scale would need the inverse
/// transpose; `alignToVertexNormals` is what keeps the result usable anyway, because it only needs
/// the normal's *side*, not its exact direction.
inline BspSolid finishSolid(const BspSolid& polys, const Mat4& m) {
    BspSolid out;
    out.reserve(polys.size());
    for (const BspPoly& p : polys) {
        BspPoly q;
        q.shared = p.shared;
        q.v.reserve(p.v.size());
        for (const BspVertex& s : p.v) {
            double w[3];
            applyMat(m, s.p, w);
            double n[3] = {m.m[0] * s.n[0] + m.m[1] * s.n[1] + m.m[2] * s.n[2],
                           m.m[4] * s.n[0] + m.m[5] * s.n[1] + m.m[6] * s.n[2],
                           m.m[8] * s.n[0] + m.m[9] * s.n[1] + m.m[10] * s.n[2]};
            double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len < 1e-12) {
                len = 1.0;
            }
            q.v.push_back(vert(w[0], w[1], w[2], n[0] / len, n[1] / len, n[2] / len));
        }
        q.computePlane();
        q.alignToVertexNormals();
        out.push_back(std::move(q));
    }
    return out;
}

}  // namespace detail

/// One primitive as a closed solid, in world space.
///
/// Returns false for a primitive that has no solid form: Plane, which is unbounded, an unsupported
/// node, or a degenerate cone or triangle. A caller that gets false must not treat the empty result
/// as "an empty solid" -- it means "this subtree cannot be tessellated", and the two differ.
inline bool tessellatePrimitive(const CsgNode& n, std::uint16_t index, const detail::Mat4& m,
                                BspSolid& out) {
    using detail::addFlat;
    using detail::addPoly;
    using detail::kPi;
    using detail::spherePoint;
    using detail::torusPoint;
    using detail::vert;

    const float* q = n.params;
    BspSolid local;

    switch (static_cast<Op>(n.op)) {
        case Op::Box: {
            const double ax = std::min<double>(q[0], q[3]), bx = std::max<double>(q[0], q[3]);
            const double ay = std::min<double>(q[1], q[4]), by = std::max<double>(q[1], q[4]);
            const double az = std::min<double>(q[2], q[5]), bz = std::max<double>(q[2], q[5]);
            const double faces[6][4][3] = {
                {{ax, ay, az}, {ax, by, az}, {bx, by, az}, {bx, ay, az}},
                {{ax, ay, bz}, {bx, ay, bz}, {bx, by, bz}, {ax, by, bz}},
                {{ax, ay, az}, {bx, ay, az}, {bx, ay, bz}, {ax, ay, bz}},
                {{ax, by, az}, {ax, by, bz}, {bx, by, bz}, {bx, by, az}},
                {{ax, ay, az}, {ax, ay, bz}, {ax, by, bz}, {ax, by, az}},
                {{bx, ay, az}, {bx, by, az}, {bx, by, bz}, {bx, ay, bz}}};
            const double normals[6][3] = {{0, 0, -1}, {0, 0, 1},  {0, -1, 0},
                                          {0, 1, 0},  {-1, 0, 0}, {1, 0, 0}};
            for (int i = 0; i < 6; ++i) {
                addFlat(local, faces[i], 4, normals[i][0], normals[i][1], normals[i][2], index);
            }
            break;
        }

        case Op::Sphere: {
            const double r = q[0];
            // Triangles, not quads: a quad on a sphere patch is not planar, and a BSP plane
            // derived from a non-planar polygon puts some of its own vertices on the wrong side.
            for (int i = 0; i < kLatitudes; ++i) {
                const double t0 = kPi * i / kLatitudes, t1 = kPi * (i + 1) / kLatitudes;
                for (int j = 0; j < kSegments; ++j) {
                    const double p0 = 2 * kPi * j / kSegments, p1 = 2 * kPi * (j + 1) / kSegments;
                    if (i > 0) {
                        addPoly(local,
                                {spherePoint(r, t0, p0), spherePoint(r, t0, p1),
                                 spherePoint(r, t1, p1)},
                                index);
                    }
                    if (i < kLatitudes - 1) {
                        addPoly(local,
                                {spherePoint(r, t0, p0), spherePoint(r, t1, p1),
                                 spherePoint(r, t1, p0)},
                                index);
                    }
                }
            }
            break;
        }

        case Op::Cylinder: {
            const double r = q[2];
            const double a = std::min<double>(q[0], q[1]), b = std::max<double>(q[0], q[1]);
            std::vector<BspVertex> capTop, capBottom;
            for (int j = 0; j < kSegments; ++j) {
                const double p0 = 2 * kPi * j / kSegments, p1 = 2 * kPi * (j + 1) / kSegments;
                const double c0 = std::cos(p0), s0 = std::sin(p0);
                const double c1 = std::cos(p1), s1 = std::sin(p1);
                addPoly(local,
                        {vert(r * c0, a, r * s0, c0, 0, s0), vert(r * c0, b, r * s0, c0, 0, s0),
                         vert(r * c1, b, r * s1, c1, 0, s1), vert(r * c1, a, r * s1, c1, 0, s1)},
                        index);
                capTop.push_back(vert(r * c0, b, r * s0, 0, 1, 0));
                capBottom.push_back(vert(r * c0, a, r * s0, 0, -1, 0));
            }
            // The bottom cap is walked in the same direction as the top, so its winding faces the
            // wrong way until it is reversed.
            std::reverse(capBottom.begin(), capBottom.end());
            addPoly(local, std::move(capTop), index);
            addPoly(local, std::move(capBottom), index);
            break;
        }

        case Op::Cone: {
            const double r1 = q[0], h = q[1];
            if (h == 0.0 || r1 == 0.0) {
                return false;
            }
            const double nl = std::sqrt(h * h + r1 * r1);
            std::vector<BspVertex> base;
            for (int j = 0; j < kSegments; ++j) {
                const double p0 = 2 * kPi * j / kSegments, p1 = 2 * kPi * (j + 1) / kSegments;
                const double c0 = std::cos(p0), s0 = std::sin(p0);
                const double c1 = std::cos(p1), s1 = std::sin(p1);
                const double pm = (p0 + p1) / 2;
                addPoly(local,
                        {vert(r1 * c0, 0, r1 * s0, h * c0 / nl, r1 / nl, h * s0 / nl),
                         vert(0, h, 0, h * std::cos(pm) / nl, r1 / nl, h * std::sin(pm) / nl),
                         vert(r1 * c1, 0, r1 * s1, h * c1 / nl, r1 / nl, h * s1 / nl)},
                        index);
                // With h < 0 the apex points down, so the base faces up instead.
                base.push_back(vert(r1 * c1, 0, r1 * s1, 0, h > 0 ? -1 : 1, 0));
            }
            addPoly(local, std::move(base), index);
            break;
        }

        case Op::Torus: {
            const double R = q[0], r = q[1];
            for (int i = 0; i < kSegments; ++i) {
                const double a0 = 2 * kPi * i / kSegments, a1 = 2 * kPi * (i + 1) / kSegments;
                for (int j = 0; j < kTubeBands; ++j) {
                    const double b0 = 2 * kPi * j / kTubeBands, b1 = 2 * kPi * (j + 1) / kTubeBands;
                    addPoly(local,
                            {torusPoint(R, r, a0, b0), torusPoint(R, r, a0, b1),
                             torusPoint(R, r, a1, b1)},
                            index);
                    addPoly(local,
                            {torusPoint(R, r, a0, b0), torusPoint(R, r, a1, b1),
                             torusPoint(R, r, a1, b0)},
                            index);
                }
            }
            break;
        }

        case Op::Disc: {
            const double r = q[0], hole = q[1];
            if (r <= 0.0) {
                return false;
            }
            // The same thickness the POV export uses, so the two agree on what a face is as a
            // solid. The face sits at the middle of the slab, so nothing appears to move.
            const double h = detail::patchThickness(n) / 2.0;
            std::vector<BspVertex> capTop, capBottom;
            for (int j = 0; j < kSegments; ++j) {
                const double p0 = 2 * kPi * j / kSegments, p1 = 2 * kPi * (j + 1) / kSegments;
                const double c0 = std::cos(p0), s0 = std::sin(p0);
                const double c1 = std::cos(p1), s1 = std::sin(p1);
                addPoly(local,
                        {vert(r * c0, -h, r * s0, c0, 0, s0), vert(r * c0, h, r * s0, c0, 0, s0),
                         vert(r * c1, h, r * s1, c1, 0, s1), vert(r * c1, -h, r * s1, c1, 0, s1)},
                        index);
                if (hole > 0.0) {
                    addPoly(local,
                            {vert(hole * c1, -h, hole * s1, -c1, 0, -s1),
                             vert(hole * c1, h, hole * s1, -c1, 0, -s1),
                             vert(hole * c0, h, hole * s0, -c0, 0, -s0),
                             vert(hole * c0, -h, hole * s0, -c0, 0, -s0)},
                            index);
                    const double top[4][3] = {{hole * c0, h, hole * s0},
                                              {r * c0, h, r * s0},
                                              {r * c1, h, r * s1},
                                              {hole * c1, h, hole * s1}};
                    const double bottom[4][3] = {{hole * c0, -h, hole * s0},
                                                 {hole * c1, -h, hole * s1},
                                                 {r * c1, -h, r * s1},
                                                 {r * c0, -h, r * s0}};
                    addFlat(local, top, 4, 0, 1, 0, index);
                    addFlat(local, bottom, 4, 0, -1, 0, index);
                } else {
                    capTop.push_back(vert(r * c0, h, r * s0, 0, 1, 0));
                    capBottom.push_back(vert(r * c0, -h, r * s0, 0, -1, 0));
                }
            }
            if (hole <= 0.0) {
                std::reverse(capBottom.begin(), capBottom.end());
                addPoly(local, std::move(capTop), index);
                addPoly(local, std::move(capBottom), index);
            }
            break;
        }

        case Op::Triangle: {
            double src[3][3];
            for (int i = 0; i < 3; ++i) {
                for (int k = 0; k < 3; ++k) {
                    src[i][k] = q[i * 3 + k];
                }
            }
            const double u[3] = {src[1][0] - src[0][0], src[1][1] - src[0][1],
                                 src[1][2] - src[0][2]};
            const double w2[3] = {src[2][0] - src[0][0], src[2][1] - src[0][1],
                                  src[2][2] - src[0][2]};
            double nrm[3] = {u[1] * w2[2] - u[2] * w2[1], u[2] * w2[0] - u[0] * w2[2],
                             u[0] * w2[1] - u[1] * w2[0]};
            const double len = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
            if (len < 1e-12) {
                return false;   // a zero-area triangle cannot become a solid
            }
            for (int i = 0; i < 3; ++i) {
                nrm[i] /= len;
            }
            const double h = detail::patchThickness(n) / 2.0;
            double top[3][3], bot[3][3];
            for (int i = 0; i < 3; ++i) {
                for (int k = 0; k < 3; ++k) {
                    top[i][k] = src[i][k] + nrm[k] * h;
                    bot[i][k] = src[i][k] - nrm[k] * h;
                }
            }
            const double topFace[3][3] = {{top[0][0], top[0][1], top[0][2]},
                                          {top[1][0], top[1][1], top[1][2]},
                                          {top[2][0], top[2][1], top[2][2]}};
            const double botFace[3][3] = {{bot[2][0], bot[2][1], bot[2][2]},
                                          {bot[1][0], bot[1][1], bot[1][2]},
                                          {bot[0][0], bot[0][1], bot[0][2]}};
            addFlat(local, topFace, 3, nrm[0], nrm[1], nrm[2], index);
            addFlat(local, botFace, 3, -nrm[0], -nrm[1], -nrm[2], index);
            for (int i = 0; i < 3; ++i) {
                const int j = (i + 1) % 3;
                const int k = (i + 2) % 3;
                const double e[3] = {src[j][0] - src[i][0], src[j][1] - src[i][1],
                                     src[j][2] - src[i][2]};
                double sN[3] = {e[1] * nrm[2] - e[2] * nrm[1], e[2] * nrm[0] - e[0] * nrm[2],
                                e[0] * nrm[1] - e[1] * nrm[0]};
                const double sl = std::sqrt(sN[0] * sN[0] + sN[1] * sN[1] + sN[2] * sN[2]);
                if (sl < 1e-12) {
                    continue;
                }
                for (int c = 0; c < 3; ++c) {
                    sN[c] /= sl;
                }
                const double d = sN[0] * src[i][0] + sN[1] * src[i][1] + sN[2] * src[i][2];
                if (sN[0] * src[k][0] + sN[1] * src[k][1] + sN[2] * src[k][2] > d) {
                    for (int c = 0; c < 3; ++c) {
                        sN[c] = -sN[c];
                    }
                }
                const double side[4][3] = {{bot[i][0], bot[i][1], bot[i][2]},
                                           {bot[j][0], bot[j][1], bot[j][2]},
                                           {top[j][0], top[j][1], top[j][2]},
                                           {top[i][0], top[i][1], top[i][2]}};
                addFlat(local, side, 4, sN[0], sN[1], sN[2], index);
            }
            break;
        }

        default:
            return false;   // Plane is unbounded; Unsupported has no shape at all
    }

    out = detail::finishSolid(local, m);
    return true;
}

}  // namespace makina
