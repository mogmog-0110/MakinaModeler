// A warped subtree as a POV-Ray isosurface (D-14).
//
// POV has no twist, bend or taper. It does have `isosurface { function { ... } }`, which draws the
// zero set of any expression in x, y, z its function language can spell -- and a domain warp is
// exactly an expression: the children's field read at the inverse-mapped point. So a warp goes
// out as one isosurface whose function is the whole subtree written as arithmetic, and POV
// ray-traces the warped solid from a description that shares no code with the march. That is what
// makes the silhouette comparison mean something for a warp: the only thing the two sides share
// is Warp.hpp's algebra, spelled once in HLSL/C++ and once, here, in POV's function syntax.
//
// Only the zero set has to agree, which is a large simplification: no Lipschitz division, no
// guard, no distance correction -- POV's isosurface solver wants a bound on the gradient
// (max_gradient) and nothing else, so a Box may be written as a Chebyshev distance and a Scale
// as a plain substitution.
//
// The subset: Box, Sphere, Cylinder (Y-axis), Cone, Torus, the three booleans, Translate,
// Rotate, Scale, and nested warps. Anything else under a warp is reported and the isosurface
// is not written; the caller names the scene rather than drawing something else.
//
// The function is built by substitution -- each node returns an expression in whatever strings
// stand for x, y and z at that point in the tree -- because POV's function language has no local
// variables. The strings can grow (a twist under a bend under a rotate substitutes an atan2 into
// a sin into a rotate), and POV parses that fine; the fixtures stay small.

#pragma once

#include "Bounds.hpp"
#include "Eval.hpp"
#include "PovShape.hpp"
#include "Scene.hpp"

#include <cmath>
#include <string>

namespace makina {
namespace detail {

/// True when everything under `index` has an isosurface spelling.
inline bool isoWritable(const Scene& s, std::uint16_t index, std::string& why) {
    const CsgNode& n = s.nodes[index];
    const Op op = static_cast<Op>(n.op);
    switch (op) {
        case Op::Box: case Op::Sphere: case Op::Cylinder: case Op::Cone: case Op::Torus:
            return true;
        case Op::Merge: case Op::Difference: case Op::Intersection:
        case Op::Translate: case Op::Rotate: case Op::Scale:
        case Op::Twist: case Op::Bend: case Op::Taper:
        case Op::Label:
            for (std::uint16_t i = 0; i < n.childCount; ++i) {
                if (!isoWritable(s, static_cast<std::uint16_t>(n.firstChild + i), why)) {
                    return false;
                }
            }
            return true;
        default:
            why = std::string(findOp(op) ? findOp(op)->name : "?") + " under a warp";
            return false;
    }
}

struct IsoPoint {
    std::string x, y, z;
};

/// A number in POV function syntax: always with a decimal point so it is never read as an
/// integer, and parenthesised when negative so "a - -b" cannot happen.
inline std::string isoNum(double v) {
    std::string t = num(v);
    if (t.find('.') == std::string::npos && t.find('e') == std::string::npos) {
        t += ".0";
    }
    return v < 0.0 ? "(" + t + ")" : t;
}

inline std::string isoSq(const std::string& e) { return "(" + e + ")*(" + e + ")"; }

/// The primitive's field in its own frame, as an expression of the point strings.
inline std::string isoPrimitive(const CsgNode& n, const IsoPoint& p) {
    const float* q = n.params;
    switch (static_cast<Op>(n.op)) {
        case Op::Sphere:
            return "(sqrt(" + isoSq(p.x) + "+" + isoSq(p.y) + "+" + isoSq(p.z) + ")-" +
                   isoNum(q[0]) + ")";
        case Op::Box: {
            // Chebyshev distance to the box: the same zero set as the exact one, and cheap.
            const double cx = (q[0] + q[3]) * 0.5, cy = (q[1] + q[4]) * 0.5, cz = (q[2] + q[5]) * 0.5;
            const double hx = std::fabs(q[3] - q[0]) * 0.5, hy = std::fabs(q[4] - q[1]) * 0.5,
                         hz = std::fabs(q[5] - q[2]) * 0.5;
            return "max(max(abs(" + p.x + "-" + isoNum(cx) + ")-" + isoNum(hx) + ",abs(" + p.y +
                   "-" + isoNum(cy) + ")-" + isoNum(hy) + "),abs(" + p.z + "-" + isoNum(cz) +
                   ")-" + isoNum(hz) + ")";
        }
        case Op::Cylinder: {
            // params: capPoint, basePoint, radius on the Y axis.
            const double top = q[0], bottom = q[1], r = q[2];
            const double cy = (top + bottom) * 0.5, hy = std::fabs(top - bottom) * 0.5;
            return "max(sqrt(" + isoSq(p.x) + "+" + isoSq(p.z) + ")-" + isoNum(r) + ",abs(" +
                   p.y + "-" + isoNum(cy) + ")-" + isoNum(hy) + ")";
        }
        case Op::Cone: {
            // Base radius r at y=0, apex at y=h (PovShape.hpp): radius shrinks linearly. Written
            // as the larger of the radial and the axial excess, whose zero set is the cone.
            const double r = q[0], h = q[1];
            return "max(sqrt(" + isoSq(p.x) + "+" + isoSq(p.z) + ")-" + isoNum(r) + "*(1.0-(" +
                   p.y + ")/" + isoNum(h) + "),max(-(" + p.y + ")," + "(" + p.y + ")-" +
                   isoNum(h) + "))";
        }
        case Op::Torus: {
            const double R = q[0], r = q[1];
            return "(sqrt(" + isoSq("sqrt(" + isoSq(p.x) + "+" + isoSq(p.z) + ")-" + isoNum(R)) +
                   "+" + isoSq(p.y) + ")-" + isoNum(r) + ")";
        }
        default:
            return "1.0";
    }
}

/// The inverse of one transform or warp applied to the point strings, giving the strings the
/// children see. Mirrors Eval.hpp's invApply and Warp.hpp's maps in POV syntax.
inline IsoPoint isoInverse(const Scene& s, std::uint16_t index, const IsoPoint& p) {
    const CsgNode& n = s.nodes[index];
    const Op op = static_cast<Op>(n.op);
    const int axis = n.flags & flags::kAxisMask;
    if (op == Op::Translate) {
        return {"(" + p.x + "-" + isoNum(n.params[0]) + ")", "(" + p.y + "-" + isoNum(n.params[1]) + ")",
                "(" + p.z + "-" + isoNum(n.params[2]) + ")"};
    }
    if (op == Op::Scale) {
        return {"(" + p.x + "/" + isoNum(n.params[0]) + ")", "(" + p.y + "/" + isoNum(n.params[1]) + ")",
                "(" + p.z + "/" + isoNum(n.params[2]) + ")"};
    }
    if (op == Op::Rotate) {
        const double a = -n.params[0] * 3.14159265358979323846 / 180.0;
        const std::string c = isoNum(std::cos(a)), sn = isoNum(std::sin(a));
        if (axis == flags::kAxisY) {
            return {"(" + c + "*" + p.x + "+" + sn + "*" + p.z + ")", p.y,
                    "(-" + sn + "*" + p.x + "+" + c + "*" + p.z + ")"};
        }
        if (axis == flags::kAxisZ) {
            return {"(" + c + "*" + p.x + "-" + sn + "*" + p.y + ")",
                    "(" + sn + "*" + p.x + "+" + c + "*" + p.y + ")", p.z};
        }
        return {p.x, "(" + c + "*" + p.y + "-" + sn + "*" + p.z + ")",
                "(" + sn + "*" + p.y + "+" + c + "*" + p.z + ")"};
    }
    // Warps: into the (a, u, v) frame, apply the inverse map on the clamped axis coordinate,
    // back out. Same freeze as Warp.hpp: beyond H the end section extends straight.
    const WarpExtent e = warpExtent(s, index, Fidelity{});
    const double H = e.valid ? e.H / kWarpGuardMargin : 0.0;
    const double rate = warpRateOf(n);
    std::string a, u, v;
    if (axis == 0)      { a = p.x; u = p.y; v = p.z; }
    else if (axis == 1) { a = p.y; u = p.z; v = p.x; }
    else                { a = p.z; u = p.x; v = p.y; }
    const std::string ac = "min(max(" + a + "," + isoNum(-H) + ")," + isoNum(H) + ")";
    const std::string over = "(" + a + "-" + ac + ")";
    std::string ra, ru, rv;
    if (op == Op::Twist) {
        const std::string th = "(" + isoNum(-rate) + "*" + ac + ")";
        ra = "(" + ac + "+" + over + ")";
        ru = "(cos(" + th + ")*" + u + "-sin(" + th + ")*" + v + ")";
        rv = "(sin(" + th + ")*" + u + "+cos(" + th + ")*" + v + ")";
    } else if (op == Op::Taper) {
        const std::string sc = "(1.0+" + isoNum(rate) + "*" + ac + ")";
        ra = "(" + ac + "+" + over + ")";
        ru = "(" + u + "/" + sc + ")";
        rv = "(" + v + "/" + sc + ")";
    } else {
        // Bend, mkBendInv spelled out. c = 1/rate; the angle is clamped to +-rate*H and the
        // overshoot is the tangential distance past the arc's end.
        const double c = 1.0 / rate;
        const std::string cs = isoNum(c);
        const std::string du = "(" + cs + "-" + u + ")";
        const std::string phi = c < 0.0 ? "atan2(" + a + ",-" + du + ")" : "atan2(" + a + "," + du + ")";
        const double phiMax = std::fabs(rate) * H;
        const std::string phic = "min(max(" + phi + "," + isoNum(-phiMax) + ")," + isoNum(phiMax) + ")";
        const std::string rr = "sqrt(" + isoSq(du) + "+" + isoSq(a) + ")";
        const std::string ea = "(" + rr + "*sin(" + phi + "))";
        const std::string ed = "(" + rr + "*cos(" + phi + "))";
        const std::string overB = "(" + ea + "*cos(" + phic + ")-" + ed + "*sin(" + phic + "))";
        const std::string rad = "(" + ea + "*sin(" + phic + ")+" + ed + "*cos(" + phic + "))";
        ra = "(" + phic + "*" + isoNum(std::fabs(c)) + "+" + overB + ")";
        ru = c < 0.0 ? "(" + cs + "+" + rad + ")" : "(" + cs + "-" + rad + ")";
        rv = v;
    }
    IsoPoint out;
    if (axis == 0)      { out.x = ra; out.y = ru; out.z = rv; }
    else if (axis == 1) { out.y = ra; out.z = ru; out.x = rv; }
    else                { out.z = ra; out.x = ru; out.y = rv; }
    return out;
}

/// The subtree's field as one expression.
inline std::string isoExpr(const Scene& s, std::uint16_t index, const IsoPoint& p) {
    const CsgNode& n = s.nodes[index];
    const Op op = static_cast<Op>(n.op);
    if (isPrimitive(op)) {
        return isoPrimitive(n, p);
    }
    if (isTransform(op)) {
        const IsoPoint q = isoInverse(s, index, p);
        std::string acc;
        for (std::uint16_t i = 0; i < n.childCount; ++i) {
            const std::string e = isoExpr(s, static_cast<std::uint16_t>(n.firstChild + i), q);
            acc = acc.empty() ? e : "min(" + acc + "," + e + ")";
        }
        return acc.empty() ? "1.0" : acc;
    }
    // Booleans and Label: children in order, folded left. Left-leaning is fine here -- POV is
    // parsing text, not marching a stack.
    std::string acc;
    for (std::uint16_t i = 0; i < n.childCount; ++i) {
        const std::string e = isoExpr(s, static_cast<std::uint16_t>(n.firstChild + i), p);
        if (acc.empty()) {
            acc = e;
        } else if (op == Op::Difference) {
            acc = "max(" + acc + ",-" + e + ")";
        } else if (op == Op::Intersection) {
            acc = "max(" + acc + "," + e + ")";
        } else {
            acc = "min(" + acc + "," + e + ")";
        }
    }
    return acc.empty() ? "1.0" : acc;
}

}  // namespace detail

/// A warp node and everything under it as one `isosurface{...}` block, dressed by the caller.
/// Empty, with `why` set, when something under the warp has no isosurface spelling.
///
/// max_gradient is generous: the function is a Chebyshev-or-exact distance through an inverse
/// map whose stretch mkWarpLipschitz bounds, so its gradient stays under that bound; 8 covers
/// every fixture with room, and POV warns rather than fails when it is too small.
inline std::string povIsosurface(const Scene& s, std::uint16_t index, std::string& why) {
    if (!detail::isoWritable(s, index, why)) {
        return std::string();
    }
    const detail::IsoPoint p{"x", "y", "z"};
    const std::string f = detail::isoExpr(s, index, p);
    // POV wants a container. The warp's own box, in its own space, is what Bounds gives.
    int count = 0;
    const Aabb box = detail::warpBounds(s, index, detail::identityMat(), count, Fidelity{});
    std::string out = "isosurface{\n\tfunction{ " + f + " }\n";
    out += "\tcontained_by{ box{ " + detail::vec3(box.lo[0], box.lo[1], box.lo[2]) + ", " +
           detail::vec3(box.hi[0], box.hi[1], box.hi[2]) + " } }\n";
    out += "\tthreshold 0\n\tmax_gradient 8\n\taccuracy 0.0005\n";
    return out;
}

}  // namespace makina
