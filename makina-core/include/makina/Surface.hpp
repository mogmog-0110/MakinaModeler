// Surface point sampling: a port of SceneSdf's localSurface / surfaceSamplesBudget.
//
// The measurement commands all work the same way -- take points on one subtree's surface, evaluate
// the other subtree's distance field at them -- so this is the input to every one of them.
//
// Two properties matter and both are inherited from the reference:
//
//   Deterministic. No random numbers anywhere. The same scene yields the same points in the same
//   order, which is what lets the port be compared against Java point by point rather than
//   statistically.
//
//   Area weighted. Splitting a budget evenly per primitive leaves a large flat face sparse while a
//   small torus gets crowded, and a gap measured from sparse samples reads as larger than it is.

#pragma once

#include "Bounds.hpp"
#include "Eval.hpp"
#include "Scene.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

namespace makina {

/// A surface point and the primitive it came from.
struct SurfaceSample {
    double        p[3];
    std::uint16_t prim;   ///< index into Scene::nodes
};

namespace detail {

/// A point in a primitive's own space, wrapped so it can live in a vector: a bare `double[3]` is
/// not a valid element type.
struct LocalPoint {
    double p[3];
};

/// Golden angle, as the reference spells it.
inline double goldenAngle() {
    return 3.14159265358979323846 * (3.0 - std::sqrt(5.0));
}

/// Rough local surface area. Precision is not the point: this only apportions a sample budget.
inline double areaOf(const CsgNode& n) {
    const float* q = n.params;
    const double pi = 3.14159265358979323846;

    switch (static_cast<Op>(n.op)) {
        case Op::Box: {
            const double a = std::fabs(static_cast<double>(q[3]) - q[0]);
            const double b = std::fabs(static_cast<double>(q[4]) - q[1]);
            const double c = std::fabs(static_cast<double>(q[5]) - q[2]);
            return 2.0 * (a * b + b * c + c * a);
        }
        case Op::Sphere:
            return 4.0 * pi * q[0] * q[0];
        case Op::Cylinder: {
            // params are capPoint, basePoint, radius.
            const double r = q[2];
            const double h = std::fabs(static_cast<double>(q[0]) - q[1]);
            return 2.0 * pi * r * (h + r);
        }
        case Op::Cone: {
            const double r = q[0];
            const double h = std::fabs(static_cast<double>(q[1]));
            return pi * r * (r + std::sqrt(r * r + h * h));
        }
        case Op::Torus:
            return 4.0 * pi * pi * q[0] * q[1];
        case Op::Disc: {
            const double outer = static_cast<double>(q[0]) * q[0];
            const double inner = static_cast<double>(q[1]) * q[1];
            return pi * (outer > inner ? outer - inner : 0.0);
        }
        case Op::Triangle: {
            const double ux = q[3] - q[0], uy = q[4] - q[1], uz = q[5] - q[2];
            const double vx = q[6] - q[0], vy = q[7] - q[1], vz = q[8] - q[2];
            const double cx = uy * vz - uz * vy;
            const double cy = uz * vx - ux * vz;
            const double cz = ux * vy - uy * vx;
            return 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
        }
        default:
            // Plane is infinite and is not sampled; a tiny area keeps it out of the budget without
            // dividing by zero.
            return 1e-9;
    }
}

/// Area scale of a matrix, from the mean of paired column norms.
inline double areaScale(const Mat4& m) {
    const double c0 = std::sqrt(m.m[0] * m.m[0] + m.m[4] * m.m[4] + m.m[8] * m.m[8]);
    const double c1 = std::sqrt(m.m[1] * m.m[1] + m.m[5] * m.m[5] + m.m[9] * m.m[9]);
    const double c2 = std::sqrt(m.m[2] * m.m[2] + m.m[6] * m.m[6] + m.m[10] * m.m[10]);
    return (c0 * c1 + c1 * c2 + c2 * c0) / 3.0;
}

/// Points on a primitive's surface, in its own local space.
///
/// The integer arithmetic here is deliberately the reference's, truncation and all: `(int)(n*0.7)`
/// and `side/rings` decide how many points land where, so rounding them differently would move
/// every sample and make a point-by-point comparison against Java meaningless.
inline void localSurface(const CsgNode& n, int count, std::vector<LocalPoint>& out) {
    const float* q = n.params;
    const double pi = 3.14159265358979323846;
    const double ga = goldenAngle();

    auto push = [&out](double x, double y, double z) {
        out.push_back(LocalPoint{{x, y, z}});
    };

    switch (static_cast<Op>(n.op)) {
        case Op::Box: {
            const double ax = q[0] < q[3] ? q[0] : q[3], bx = q[0] < q[3] ? q[3] : q[0];
            const double ay = q[1] < q[4] ? q[1] : q[4], by = q[1] < q[4] ? q[4] : q[1];
            const double az = q[2] < q[5] ? q[2] : q[5], bz = q[2] < q[5] ? q[5] : q[2];
            const int g = std::max(2, static_cast<int>(std::lround(std::sqrt(count / 6.0))));
            for (int i = 0; i < g; ++i) {
                for (int j = 0; j < g; ++j) {
                    const double u = g == 1 ? 0.5 : static_cast<double>(i) / (g - 1);
                    const double v = g == 1 ? 0.5 : static_cast<double>(j) / (g - 1);
                    push(ax + u * (bx - ax), ay + v * (by - ay), az);
                    push(ax + u * (bx - ax), ay + v * (by - ay), bz);
                    push(ax + u * (bx - ax), ay, az + v * (bz - az));
                    push(ax + u * (bx - ax), by, az + v * (bz - az));
                    push(ax, ay + u * (by - ay), az + v * (bz - az));
                    push(bx, ay + u * (by - ay), az + v * (bz - az));
                }
            }
            return;
        }
        case Op::Sphere: {
            const double r = q[0];
            for (int i = 0; i < count; ++i) {
                const double y = 1.0 - 2.0 * (i + 0.5) / count;
                const double rad = std::sqrt(std::max(0.0, 1.0 - y * y));
                const double th = ga * i;
                push(r * rad * std::cos(th), r * y, r * rad * std::sin(th));
            }
            return;
        }
        case Op::Cylinder: {
            const double r = q[2];
            const double lo = q[1], hi = q[0];
            const int side = static_cast<int>(count * 0.7);
            const int cap = std::max(4, (count - side) / 2);
            const int rings = std::max(2, static_cast<int>(std::sqrt(side / 2.0)));
            const int segs = std::max(6, side / rings);
            for (int i = 0; i < rings; ++i) {
                for (int j = 0; j < segs; ++j) {
                    const double y =
                        lo + (hi - lo) * (rings == 1 ? 0.5 : static_cast<double>(i) / (rings - 1));
                    const double th = 2.0 * pi * j / segs;
                    push(r * std::cos(th), y, r * std::sin(th));
                }
            }
            for (int k = 0; k < 2; ++k) {
                const double y = (k == 0) ? lo : hi;
                for (int i = 0; i < cap; ++i) {
                    const double rr = r * std::sqrt((i + 0.5) / cap);
                    const double th = ga * i;
                    push(rr * std::cos(th), y, rr * std::sin(th));
                }
            }
            return;
        }
        case Op::Cone: {
            const double r1 = q[0];
            const double h = q[1];
            const int side = static_cast<int>(count * 0.7);
            const int cap = std::max(4, count - side);
            const int rings = std::max(2, static_cast<int>(std::sqrt(side / 2.0)));
            const int segs = std::max(6, side / rings);
            for (int i = 0; i < rings; ++i) {
                for (int j = 0; j < segs; ++j) {
                    const double t = rings == 1 ? 0.5 : static_cast<double>(i) / (rings - 1);
                    const double rr = r1 * (1.0 - t);
                    const double th = 2.0 * pi * j / segs;
                    push(rr * std::cos(th), h * t, rr * std::sin(th));
                }
            }
            for (int i = 0; i < cap; ++i) {
                const double rr = r1 * std::sqrt((i + 0.5) / cap);
                const double th = ga * i;
                push(rr * std::cos(th), 0.0, rr * std::sin(th));
            }
            return;
        }
        case Op::Torus: {
            const double maj = q[0], minr = q[1];
            const int u = std::max(8, static_cast<int>(std::sqrt(static_cast<double>(count) * 2.0)));
            const int v = std::max(4, count / u);
            for (int i = 0; i < u; ++i) {
                for (int j = 0; j < v; ++j) {
                    const double a = 2.0 * pi * i / u;
                    const double b = 2.0 * pi * j / v;
                    const double rr = maj + minr * std::cos(b);
                    push(rr * std::cos(a), minr * std::sin(b), rr * std::sin(a));
                }
            }
            return;
        }
        case Op::Disc: {
            const double r = q[0], hole = q[1];
            for (int i = 0; i < count; ++i) {
                const double t = (i + 0.5) / count;
                const double rr = std::sqrt(hole * hole + t * (r * r - hole * hole));
                const double th = ga * i;
                push(rr * std::cos(th), 0.0, rr * std::sin(th));
            }
            return;
        }
        case Op::Triangle: {
            const int g = std::max(2, static_cast<int>(std::sqrt(count * 2.0)));
            for (int i = 0; i <= g; ++i) {
                for (int j = 0; j <= g - i; ++j) {
                    const double u = static_cast<double>(i) / g;
                    const double v = static_cast<double>(j) / g;
                    const double w = 1.0 - u - v;
                    push(u * q[0] + v * q[3] + w * q[6],
                         u * q[1] + v * q[4] + w * q[7],
                         u * q[2] + v * q[5] + w * q[8]);
                }
            }
            return;
        }
        default:
            // Plane is infinite; there is no bounded surface to sample.
            return;
    }
}

struct PrimRef {
    std::uint16_t index;
    Mat4          world;
};

inline void collectPrims(const Scene& s, std::uint16_t index, const Mat4& m,
                         std::vector<PrimRef>& out, Fidelity f) {
    const CsgNode& n = s.nodes[index];
    const Op op = static_cast<Op>(n.op);

    Mat4 mc = m;
    if (isTransform(op)) {
        mc = mulMat(m, matrixOf(n));
    } else if (isPrimitive(op) && op != Op::Plane) {
        // Plane is in the reference's supported set but has no bounded surface, so sampling it
        // would contribute nothing while taking a share of the budget.
        out.push_back(PrimRef{index, m});
    }

    for (std::uint16_t i = 0; i < n.childCount; ++i) {
        collectPrims(s, static_cast<std::uint16_t>(n.firstChild + i), mc, out, f);
    }
}

/// Ancestor transforms of a node, composed root first. The node itself is excluded.
inline Mat4 ancestorMatrix(const Scene& s, std::uint16_t index) {
    std::uint16_t chain[64];
    int depth = 0;
    for (std::uint16_t a = s.nodes[index].parent;
         a != kNoParent && depth < 64;
         a = s.nodes[a].parent) {
        chain[depth++] = a;
    }

    Mat4 m = identityMat();
    for (int i = depth - 1; i >= 0; --i) {
        const CsgNode& n = s.nodes[chain[i]];
        if (isTransform(static_cast<Op>(n.op))) {
            m = mulMat(m, matrixOf(n));
        }
    }
    return m;
}

/// Tolerance of the CSG filter: a thousandth of the subtree diagonal, at least 1e-6.
///
/// This is why Fidelity has to reach all the way down here rather than staying inside Bounds.hpp:
/// a tighter box has a shorter diagonal, so it keeps fewer samples, so every count downstream moves.
inline double filterEps(const Scene& s, std::uint16_t index, Fidelity f) {
    const BoundsResult b = worldBounds(s, index, f);
    if (!b.box.valid) {
        return 1e-6;
    }
    const double dx = b.box.hi[0] - b.box.lo[0];
    const double dy = b.box.hi[1] - b.box.lo[1];
    const double dz = b.box.hi[2] - b.box.lo[2];
    return std::max(1e-6, std::sqrt(dx * dx + dy * dy + dz * dz) * 1e-3);
}

}  // namespace detail

/// Number of primitives in the subtree this build cannot evaluate.
inline int countUnsupported(const Scene& s, std::uint16_t index) {
    const CsgNode& n = s.nodes[index];
    int total = (static_cast<Op>(n.op) == Op::Unsupported) ? 1 : 0;
    for (std::uint16_t i = 0; i < n.childCount; ++i) {
        total += countUnsupported(s, static_cast<std::uint16_t>(n.firstChild + i));
    }
    return total;
}

/// Roughly `budget` points over the subtree, apportioned by area, in world coordinates.
///
/// Points that the CSG cuts away are dropped: a sample on a face that a Difference removed is not
/// on the surface of the result, and measuring a gap from it reports a distance to something that
/// is not there. If the filter removes everything -- which happens when the tolerance is tighter
/// than the model's own numerical noise -- the unfiltered points are returned rather than nothing,
/// matching the reference.
///
/// Pass kGrasp3D to reproduce the reference's bounds and Label handling, and with them its sample
/// counts; see Fidelity. Nothing but the comparison test should ask for that.
inline std::vector<SurfaceSample> surfaceSamples(const Scene& s, std::uint16_t index, int budget,
                                                 Fidelity f = {}) {
    std::vector<detail::PrimRef> prims;
    detail::collectPrims(s, index, detail::ancestorMatrix(s, index), prims, f);

    std::vector<SurfaceSample> raw;
    if (prims.empty()) {
        return raw;
    }

    std::vector<double> areas(prims.size());
    double total = 0.0;
    for (std::size_t i = 0; i < prims.size(); ++i) {
        areas[i] = std::max(1e-9, detail::areaOf(s.nodes[prims[i].index]) *
                                      detail::areaScale(prims[i].world));
        total += areas[i];
    }

    std::vector<detail::LocalPoint> local;
    for (std::size_t i = 0; i < prims.size(); ++i) {
        const int n = std::max(8, static_cast<int>(std::lround(budget * areas[i] / total)));
        local.clear();
        detail::localSurface(s.nodes[prims[i].index], n, local);
        for (const detail::LocalPoint& q : local) {
            SurfaceSample sample{};
            detail::applyMat(prims[i].world, q.p, sample.p);
            sample.prim = prims[i].index;
            raw.push_back(sample);
        }
    }

    const double eps = detail::filterEps(s, index, f);
    std::vector<SurfaceSample> kept;
    kept.reserve(raw.size());
    for (const SurfaceSample& sample : raw) {
        if (std::fabs(eval(s, index, sample.p, f)) <= eps) {
            kept.push_back(sample);
        }
    }
    return kept.empty() ? raw : kept;
}

}  // namespace makina
