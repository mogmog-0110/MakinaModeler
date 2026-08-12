// Boolean operations on closed polygon solids, through a BSP tree. A port of Grasp3D's CsgBsp,
// itself a port of csg.js.
//
// This is the second independent representation of the same geometry. The SDF answers "how far to
// the surface" from a formula; this answers "inside or outside" from a boundary. They share no
// code and fail in different ways -- an SDF gets a boolean wrong by combining distances that were
// never distances, a B-rep gets it wrong by losing a polygon at a coplanar face -- so where they
// agree, the model is very probably right. Tessellate.hpp drives it and bsp_compare checks it.
//
// A BSP cuts space recursively along the planes its own polygons lie on. Pushing one solid's
// polygons through the other's tree sorts them into inside and outside, and that sort is what
// makes subtract and intersect expressible at all.
//
// Precision is preview grade, and deliberately so: exactly coplanar faces and polygons smaller
// than EPS can still break up. That is a property of the algorithm, not a bug to chase here --
// the SDF is the authority on the surface, this is the cross-check.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace makina {

/// Position plus an analytic outward normal. Both are interpolated when a polygon is split, which
/// is why the normal travels with the vertex rather than being derived from the plane: after a cut
/// the plane is flat but the surface it came from was not.
struct BspVertex {
    double p[3];
    double n[3];
};

/// A convex polygon, its plane, and the primitive it came from.
///
/// `shared` is a node index into the Scene, which is what carries the material through a boolean
/// and lets a cut surface be repainted with the material of the side that was cut.
struct BspPoly {
    std::vector<BspVertex> v;
    double                 nx = 0.0, ny = 1.0, nz = 0.0, w = 0.0;
    std::uint16_t          shared = 0;

    void computePlane() {
        // Newell's method: steadier than a cross product of the first three points, which
        // degenerates whenever those three happen to be nearly collinear.
        double x = 0.0, y = 0.0, z = 0.0;
        const std::size_t n = v.size();
        for (std::size_t i = 0; i < n; ++i) {
            const BspVertex& a = v[i];
            const BspVertex& b = v[(i + 1) % n];
            x += (a.p[1] - b.p[1]) * (a.p[2] + b.p[2]);
            y += (a.p[2] - b.p[2]) * (a.p[0] + b.p[0]);
            z += (a.p[0] - b.p[0]) * (a.p[1] + b.p[1]);
        }
        const double len = std::sqrt(x * x + y * y + z * z);
        if (len < 1e-12) {
            nx = 0.0;
            ny = 1.0;
            nz = 0.0;
            w = 0.0;
            return;
        }
        nx = x / len;
        ny = y / len;
        nz = z / len;
        w = nx * v[0].p[0] + ny * v[0].p[1] + nz * v[0].p[2];
    }

    void flip() {
        std::reverse(v.begin(), v.end());
        for (BspVertex& q : v) {
            q.n[0] = -q.n[0];
            q.n[1] = -q.n[1];
            q.n[2] = -q.n[2];
        }
        nx = -nx;
        ny = -ny;
        nz = -nz;
        w = -w;
    }

    /// Matches the winding to the vertex normals, which are analytic and point outward.
    ///
    /// Absorbs two different mistakes at once: a face written with its corners in the wrong order,
    /// and a mirroring transform, which reverses every winding in the solid without touching a
    /// single coordinate. Without this a mirrored solid is inside out, and every boolean using it
    /// returns the complement of what was asked for.
    void alignToVertexNormals() {
        double a[3] = {0.0, 0.0, 0.0};
        for (const BspVertex& q : v) {
            a[0] += q.n[0];
            a[1] += q.n[1];
            a[2] += q.n[2];
        }
        if (nx * a[0] + ny * a[1] + nz * a[2] < 0.0) {
            std::reverse(v.begin(), v.end());
            computePlane();
        }
    }
};

namespace detail {

constexpr double kBspEps = 1e-5;

inline BspVertex lerpVertex(const BspVertex& a, const BspVertex& b, double t) {
    BspVertex r{};
    for (int i = 0; i < 3; ++i) {
        r.p[i] = a.p[i] + (b.p[i] - a.p[i]) * t;
        r.n[i] = a.n[i] + (b.n[i] - a.n[i]) * t;
    }
    const double len = std::sqrt(r.n[0] * r.n[0] + r.n[1] * r.n[1] + r.n[2] * r.n[2]);
    if (len > 1e-12) {
        r.n[0] /= len;
        r.n[1] /= len;
        r.n[2] /= len;
    }
    return r;
}

class BspNode {
public:
    BspNode() = default;

    explicit BspNode(std::vector<BspPoly> ps) { build(std::move(ps)); }

    void build(std::vector<BspPoly> ps) {
        if (ps.empty()) {
            return;
        }
        if (!m_hasPlane) {
            m_nx = ps[0].nx;
            m_ny = ps[0].ny;
            m_nz = ps[0].nz;
            m_w = ps[0].w;
            m_hasPlane = true;
        }
        std::vector<BspPoly> f, b;
        for (BspPoly& p : ps) {
            split(p, m_polys, m_polys, f, b);
        }
        if (!f.empty()) {
            if (!m_front) {
                m_front = std::make_unique<BspNode>();
            }
            m_front->build(std::move(f));
        }
        if (!b.empty()) {
            if (!m_back) {
                m_back = std::make_unique<BspNode>();
            }
            m_back->build(std::move(b));
        }
    }

    void invert() {
        for (BspPoly& p : m_polys) {
            p.flip();
        }
        m_nx = -m_nx;
        m_ny = -m_ny;
        m_nz = -m_nz;
        m_w = -m_w;
        if (m_front) {
            m_front->invert();
        }
        if (m_back) {
            m_back->invert();
        }
        m_front.swap(m_back);
    }

    /// Keeps only the parts of `ps` that lie outside this solid.
    std::vector<BspPoly> clipPolygons(const std::vector<BspPoly>& ps) const {
        if (!m_hasPlane) {
            return ps;
        }
        std::vector<BspPoly> f, b;
        for (const BspPoly& p : ps) {
            BspPoly copy = p;
            split(copy, f, b, f, b);
        }
        if (m_front) {
            f = m_front->clipPolygons(f);
        }
        if (m_back) {
            b = m_back->clipPolygons(b);
        } else {
            // No back child means everything behind this plane is solid, so it is dropped. This
            // one line is where "remove what is inside the other solid" actually happens.
            b.clear();
        }
        f.insert(f.end(), b.begin(), b.end());
        return f;
    }

    void clipTo(const BspNode& other) {
        m_polys = other.clipPolygons(m_polys);
        if (m_front) {
            m_front->clipTo(other);
        }
        if (m_back) {
            m_back->clipTo(other);
        }
    }

    std::vector<BspPoly> allPolygons() const {
        std::vector<BspPoly> out = m_polys;
        if (m_front) {
            const std::vector<BspPoly> f = m_front->allPolygons();
            out.insert(out.end(), f.begin(), f.end());
        }
        if (m_back) {
            const std::vector<BspPoly> b = m_back->allPolygons();
            out.insert(out.end(), b.begin(), b.end());
        }
        return out;
    }

private:
    /// Sorts a polygon against this node's plane, splitting it when it straddles.
    ///
    /// coFront / coBack take the polygons lying *in* the plane, sorted by which way they face;
    /// keeping those apart is what stops two touching solids from merging their shared face.
    void split(BspPoly& p, std::vector<BspPoly>& coFront, std::vector<BspPoly>& coBack,
               std::vector<BspPoly>& f, std::vector<BspPoly>& b) const {
        enum { kCoplanar = 0, kFront = 1, kBack = 2, kSpanning = 3 };

        int type = 0;
        std::vector<int> types(p.v.size());
        for (std::size_t i = 0; i < p.v.size(); ++i) {
            const double t =
                m_nx * p.v[i].p[0] + m_ny * p.v[i].p[1] + m_nz * p.v[i].p[2] - m_w;
            const int ty = t < -kBspEps ? kBack : (t > kBspEps ? kFront : kCoplanar);
            types[i] = ty;
            type |= ty;
        }

        switch (type) {
            case kCoplanar: {
                const double dot = m_nx * p.nx + m_ny * p.ny + m_nz * p.nz;
                (dot > 0.0 ? coFront : coBack).push_back(p);
                break;
            }
            case kFront:
                f.push_back(p);
                break;
            case kBack:
                b.push_back(p);
                break;
            default: {
                std::vector<BspVertex> fv, bv;
                for (std::size_t i = 0; i < p.v.size(); ++i) {
                    const std::size_t j = (i + 1) % p.v.size();
                    const int ti = types[i], tj = types[j];
                    if (ti != kBack) {
                        fv.push_back(p.v[i]);
                    }
                    if (ti != kFront) {
                        bv.push_back(p.v[i]);
                    }
                    if ((ti | tj) == kSpanning) {
                        const double denom = m_nx * (p.v[j].p[0] - p.v[i].p[0]) +
                                             m_ny * (p.v[j].p[1] - p.v[i].p[1]) +
                                             m_nz * (p.v[j].p[2] - p.v[i].p[2]);
                        const double t = (m_w - (m_nx * p.v[i].p[0] + m_ny * p.v[i].p[1] +
                                                 m_nz * p.v[i].p[2])) /
                                         denom;
                        const BspVertex mid = lerpVertex(p.v[i], p.v[j], t);
                        fv.push_back(mid);
                        bv.push_back(mid);
                    }
                }
                if (fv.size() >= 3) {
                    BspPoly q;
                    q.v = std::move(fv);
                    q.shared = p.shared;
                    q.computePlane();
                    f.push_back(std::move(q));
                }
                if (bv.size() >= 3) {
                    BspPoly q;
                    q.v = std::move(bv);
                    q.shared = p.shared;
                    q.computePlane();
                    b.push_back(std::move(q));
                }
                break;
            }
        }
    }

    double m_nx = 0.0, m_ny = 0.0, m_nz = 0.0, m_w = 0.0;
    bool   m_hasPlane = false;

    std::vector<BspPoly>     m_polys;
    std::unique_ptr<BspNode> m_front;
    std::unique_ptr<BspNode> m_back;
};

}  // namespace detail

/// A - B. The cut surface is B's polygons turned inside out, so it arrives carrying the blade's
/// `shared` index; Tessellate.hpp repaints it with the body's material.
inline std::vector<BspPoly> bspSubtract(const std::vector<BspPoly>& aPolys,
                                        const std::vector<BspPoly>& bPolys) {
    detail::BspNode a(aPolys);
    detail::BspNode b(bPolys);
    a.invert();
    a.clipTo(b);
    b.clipTo(a);
    b.invert();
    b.clipTo(a);
    b.invert();
    a.build(b.allPolygons());
    a.invert();
    return a.allPolygons();
}

/// A and B.
inline std::vector<BspPoly> bspIntersect(const std::vector<BspPoly>& aPolys,
                                         const std::vector<BspPoly>& bPolys) {
    detail::BspNode a(aPolys);
    detail::BspNode b(bPolys);
    a.invert();
    b.clipTo(a);
    b.invert();
    a.clipTo(b);
    b.clipTo(a);
    a.build(b.allPolygons());
    a.invert();
    return a.allPolygons();
}

}  // namespace makina
