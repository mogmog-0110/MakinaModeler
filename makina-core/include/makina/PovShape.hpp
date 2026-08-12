// One CSG primitive as a POV-Ray object, in its own local space.
//
// Local space is the whole trick: Grasp3D never bakes a transform into a primitive's numbers.
// The transforms above a node are collected into a text block on the way down the tree and
// written inside the object's braces, where POV applies them. Pov.hpp owns that block; this file
// only writes the shape and stops before the closing brace so the caller can append material and
// transform.
//
// Two of the eight primitives have no thickness. POV-Ray's CSG cannot use a surface as an operand
// -- a difference against a Disc removes nothing, because the blade has no inside -- so inside a
// Difference or an Intersection the solid form from PatchSolid is written instead: the disc becomes
// a thin cylinder, the triangle an intersection of five half-spaces.

#pragma once

#include "Op.hpp"
#include "Scene.hpp"

#include <charconv>
#include <cmath>
#include <string>

namespace makina {

namespace detail {

/// Shortest text that reads back as the same double.
///
/// POV parses what it is given, so a rounded literal is a different scene, not a differently
/// spelled one. Java's Double.toString makes the same promise, which is what keeps a POV file
/// written here comparable with one written by the reference.
inline std::string num(double v) {
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);
    return std::string(buf, r.ptr);
}

inline std::string vec3(double x, double y, double z) {
    return "<" + num(x) + "," + num(y) + "," + num(z) + ">";
}

// ------------------------------------------------------------------ PatchSolid

/// Thickness given to a zero-thickness face so it can act as a CSG operand.
///
/// Proportional to the primitive's own size rather than fixed: a 0.001-radius disc given a 0.02
/// slab would be a cylinder, and a 100-unit triangle given one would still be a surface as far as
/// the ray marcher's epsilon is concerned. The floor stops a degenerate primitive from producing a
/// zero-volume solid, which POV renders as nothing.
inline double patchThickness(const CsgNode& n) {
    constexpr double kRatio = 0.02;
    constexpr double kMin = 1e-4;

    double size = 1.0;
    if (static_cast<Op>(n.op) == Op::Disc) {
        size = n.params[0];
    } else if (static_cast<Op>(n.op) == Op::Triangle) {
        double best = 0.0;
        double v[3][3];
        for (int i = 0; i < 3; ++i) {
            for (int k = 0; k < 3; ++k) {
                v[i][k] = n.params[i * 3 + k];
            }
        }
        for (int i = 0; i < 3; ++i) {
            const int j = (i + 1) % 3;
            const double dx = v[j][0] - v[i][0];
            const double dy = v[j][1] - v[i][1];
            const double dz = v[j][2] - v[i][2];
            const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
            best = d > best ? d : best;
        }
        size = best;
    }
    const double scaled = std::fabs(size) * kRatio;
    return scaled > kMin ? scaled : kMin;
}

inline std::string povPlane(double nx, double ny, double nz, double d) {
    return "\tplane{" + vec3(nx, ny, nz) + "," + num(d) + "}\n";
}

/// One side face of a triangle solid: the half-space through edge p->q whose inside contains
/// `other`, the third corner. Returns "" for a degenerate edge, which contributes no constraint.
inline std::string triangleSide(const double p[3], const double q[3], const double other[3],
                                const double n[3]) {
    const double e[3] = {q[0] - p[0], q[1] - p[1], q[2] - p[2]};
    double s[3] = {e[1] * n[2] - e[2] * n[1], e[2] * n[0] - e[0] * n[2], e[0] * n[1] - e[1] * n[0]};
    const double len = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (len < 1e-12) {
        return std::string();
    }
    for (int i = 0; i < 3; ++i) {
        s[i] /= len;
    }
    double d = s[0] * p[0] + s[1] * p[1] + s[2] * p[2];
    if (s[0] * other[0] + s[1] * other[1] + s[2] * other[2] > d) {
        for (int i = 0; i < 3; ++i) {
            s[i] = -s[i];
        }
        d = -d;
    }
    return povPlane(s[0], s[1], s[2], d);
}

}  // namespace detail

/// The opening of a primitive's POV block, up to but not including its material.
///
/// `asSolid` asks for the thickened form of a Disc or a Triangle, which is what a POV CSG operand
/// has to be. It changes nothing for the other six.
///
/// Returns "" for a primitive POV has no object for -- an unsupported node, or a triangle with no
/// area asked for as a solid -- and the caller then writes nothing at all rather than an empty
/// block, which POV rejects.
inline std::string povShape(const CsgNode& n, bool asSolid) {
    const float* q = n.params;
    const Op op = static_cast<Op>(n.op);

    switch (op) {
        case Op::Box:
            return "box{\n\t" + detail::vec3(q[0], q[1], q[2]) + "," +
                   detail::vec3(q[3], q[4], q[5]) + "\n";

        case Op::Sphere:
            return "sphere{\n\t<0,0,0>," + detail::num(q[0]) + "\n";

        case Op::Cylinder:
            // params are capPoint, basePoint, radius: two heights on the Y axis, not two points.
            return "cylinder{\n\t<0," + detail::num(q[1]) + ",0>,<0," + detail::num(q[0]) +
                   ",0>," + detail::num(q[2]) + "\n";

        case Op::Cone:
            // The apex radius is 0: Grasp3D's Cone.render never reads Radius 2 (PORT_STATUS 3.1).
            return "cone{\n\t<0,0,0>," + detail::num(q[0]) + ",<0," + detail::num(q[1]) + ",0>,0\n";

        case Op::Torus:
            return "torus{\n\t" + detail::num(q[0]) + "," + detail::num(q[1]) + "\n";

        case Op::Plane:
            return "plane{\n\ty," + detail::num(q[0]) + "\n";

        case Op::Disc: {
            if (!asSolid) {
                // The hole radius is written even when zero. POV reads a two-argument disc as
                // solid and a three-argument one as an annulus, so dropping a zero would be fine
                // -- but keeping it means the token stream is the same shape for every disc, and
                // a disc that gains a bore later does not change the shape of what is written.
                return "disc{\n\t<0,0,0>,<0,1,0>," + detail::num(q[0]) + "," +
                       detail::num(q[1]) + "\n";
            }
            const double h = detail::patchThickness(n) / 2.0;
            if (q[1] > 0.0f) {
                // The bore is cut by a taller cylinder so its ends stick out; a blade flush with
                // the face leaves POV deciding between two coincident surfaces.
                return "difference{\n\tcylinder{<0," + detail::num(-h) + ",0>,<0," +
                       detail::num(h) + ",0>," + detail::num(q[0]) + "}\n\tcylinder{<0," +
                       detail::num(-h * 3.0) + ",0>,<0," + detail::num(h * 3.0) + ",0>," +
                       detail::num(q[1]) + "}\n";
            }
            return "cylinder{\n\t<0," + detail::num(-h) + ",0>,<0," + detail::num(h) + ",0>," +
                   detail::num(q[0]) + "\n";
        }

        case Op::Triangle: {
            double v[3][3];
            for (int i = 0; i < 3; ++i) {
                for (int k = 0; k < 3; ++k) {
                    v[i][k] = q[i * 3 + k];
                }
            }
            if (!asSolid) {
                return "triangle{\n\t" + detail::vec3(v[0][0], v[0][1], v[0][2]) + "," +
                       detail::vec3(v[1][0], v[1][1], v[1][2]) + "," +
                       detail::vec3(v[2][0], v[2][1], v[2][2]) + "\n";
            }
            const double u[3] = {v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2]};
            const double w[3] = {v[2][0] - v[0][0], v[2][1] - v[0][1], v[2][2] - v[0][2]};
            double nrm[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2],
                             u[0] * w[1] - u[1] * w[0]};
            const double len =
                std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
            if (len < 1e-12) {
                // A zero-area triangle cannot become a solid; the surface form is all there is.
                return povShape(n, false);
            }
            for (int i = 0; i < 3; ++i) {
                nrm[i] /= len;
            }
            const double h = detail::patchThickness(n) / 2.0;
            const double d = nrm[0] * v[0][0] + nrm[1] * v[0][1] + nrm[2] * v[0][2];

            std::string s = "intersection{\n";
            s += detail::povPlane(nrm[0], nrm[1], nrm[2], d + h);
            s += detail::povPlane(-nrm[0], -nrm[1], -nrm[2], -(d - h));
            s += detail::triangleSide(v[0], v[1], v[2], nrm);
            s += detail::triangleSide(v[1], v[2], v[0], nrm);
            s += detail::triangleSide(v[2], v[0], v[1], nrm);
            return s;
        }

        default:
            return std::string();
    }
}

}  // namespace makina
