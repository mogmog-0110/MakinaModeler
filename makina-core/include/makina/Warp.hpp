// Domain warps (PLAN.md D-14): Twist, Bend and Taper as maps on the evaluation point.
//
// A warp bends everything under it by moving the point instead of the shape:
//
//     d'(p) = d(w^-1(p)) / L
//
// The inverse map sends a world point back into the unwarped space of the children, and the
// children's field is read there. L is a Lipschitz bound of w^-1 over the subtree, so that d'
// stays a lower bound on the true distance and sphere tracing does not step through the surface.
// Both halves live here, written once for C++ and HLSL in the style of Sdf.hpp, so the CPU
// evaluator, the generated shader and the interpreter cannot disagree about what a Twist is.
//
// Every warp is expressed about one axis, called `a` below; `u` and `v` are the other two, in the
// right-handed order (a, u, v) = (Y, Z, X) for the Y axis. mkWarpInv permutes the coordinates in
// and out so the maps themselves are written once.
//
// Rates are per unit of length along the axis, in radians here; the node stores degrees because
// that is what a person types, and the conversion happens at the call site.

#ifndef MAKINA_WARP_INCLUDED
#define MAKINA_WARP_INCLUDED

#include "Sdf.hpp"

// MK_OUT: an output parameter. HLSL spells it `out float x`, C++ `double& x`.
#ifdef __cplusplus
    #define MK_SIN(x)   std::sin(x)
    #define MK_COS(x)   std::cos(x)
    #define MK_ATAN2(y, x) std::atan2(y, x)
    #define MK_OUT MK_FLOAT&
    namespace makina {
#else
    #define MK_SIN(x)   sin(x)
    #define MK_COS(x)   cos(x)
    #define MK_ATAN2(y, x) atan2(y, x)
    #define MK_OUT out MK_FLOAT
#endif

#define MK_WARP_TWIST 0
#define MK_WARP_BEND  1
#define MK_WARP_TAPER 2

/// Inverse twist: undo a rotation of angle rate*a about the axis. Length-preserving along the
/// axis, so its only stretch is tangential: a point at radius r moves by rate*r per unit of a,
/// which is where the Lipschitz bound 1 + |rate|*R comes from.
MK_FN void mkTwistInv(MK_FLOAT rate, MK_FLOAT a, MK_FLOAT u, MK_FLOAT v,
                      MK_OUT outA, MK_OUT outU, MK_OUT outV) {
    MK_FLOAT th = -rate * a;
    MK_FLOAT c = MK_COS(th);
    MK_FLOAT s = MK_SIN(th);
    outA = a;
    outU = c * u - s * v;
    outV = s * u + c * v;
}

/// Inverse bend: the axis has been wrapped onto a circle of radius 1/rate whose centre sits at
/// u = 1/rate; undo it by reading the point's angle about that centre as the unbent axis
/// coordinate and its distance from the centre as the unbent u. rate == 0 is the identity and
/// is short-circuited: the centre would be at infinity.
///
/// Beyond the children's reach the arc stops: the angle is clamped to +-rate*H and the overshoot
/// is measured as straight distance from the end section's plane, so the space past each end is
/// the end section extruded along its own tangent. That is the freeze mkWarpInv describes, done
/// here because in a bend "along the axis" means "around the arc".
MK_FN void mkBendInv(MK_FLOAT rate, MK_FLOAT H, MK_FLOAT a, MK_FLOAT u, MK_FLOAT v,
                     MK_OUT outA, MK_OUT outU, MK_OUT outV) {
    if (MK_ABS(rate) < 1.0e-9) {
        outA = a;
        outU = u;
        outV = v;
        return;
    }
    MK_FLOAT c = 1.0 / rate;
    MK_FLOAT du = c - u;
    MK_FLOAT phi = MK_ATAN2(a, du);
    if (c < 0.0) {
        // Centre on the -u side: the angle is measured from the -u direction, so it flips.
        phi = MK_ATAN2(a, -du);
    }
    MK_FLOAT phiMax = MK_ABS(rate) * H;
    MK_FLOAT phic = mkClamp(phi, -phiMax, phiMax);
    // Distance from the centre, and its overshoot past the arc's end measured along the
    // end section's tangent (the direction the arc was heading when it stopped).
    MK_FLOAT rr = MK_SQRT(du * du + a * a);
    MK_FLOAT sn = MK_SIN(phic);
    MK_FLOAT cs = MK_COS(phic);
    // Position of the point relative to the centre, in the (a, along-du) frame, and the end
    // section's tangent there. The overshoot is the component along that tangent.
    MK_FLOAT ea = rr * MK_SIN(phi);
    MK_FLOAT ed = rr * MK_COS(phi);
    MK_FLOAT over = (ea * cs - ed * sn);
    MK_FLOAT rad = ea * sn + ed * cs;
    outA = phic * MK_ABS(c) + over;
    outU = c - rad * (c < 0.0 ? -1.0 : 1.0);
    outV = v;
}

/// Inverse taper: the cross-section has been scaled by s = 1 + rate*a; divide it back out.
/// s is clamped away from zero so a taper past the point where the section vanishes stays
/// finite; the reader refuses such a node before it gets here.
MK_FN void mkTaperInv(MK_FLOAT rate, MK_FLOAT a, MK_FLOAT u, MK_FLOAT v,
                      MK_OUT outA, MK_OUT outU, MK_OUT outV) {
    MK_FLOAT s = 1.0 + rate * a;
    if (MK_ABS(s) < 1.0e-6) {
        s = s < 0.0 ? -1.0e-6 : 1.0e-6;
    }
    outA = a;
    outU = u / s;
    outV = v / s;
}

/// The forward map: where a point of the unwarped children ends up. Only the mesh writer needs
/// it (D-14: the B-rep is built unwarped and its vertices are carried through). Frozen beyond
/// H exactly as the inverse is, so the two are inverses of each other everywhere.
MK_FN void mkWarpFwd(int kind, int axis, MK_FLOAT rate, MK_FLOAT H,
                     MK_FLOAT x, MK_FLOAT y, MK_FLOAT z,
                     MK_OUT ox, MK_OUT oy, MK_OUT oz) {
    MK_FLOAT a, u, v;
    if (axis == 0)      { a = x; u = y; v = z; }
    else if (axis == 1) { a = y; u = z; v = x; }
    else                { a = z; u = x; v = y; }
    MK_FLOAT ac = mkClamp(a, -H, H);
    MK_FLOAT over = a - ac;
    MK_FLOAT ra, ru, rv;
    if (kind == MK_WARP_TWIST) {
        MK_FLOAT th = rate * ac;
        MK_FLOAT c = MK_COS(th);
        MK_FLOAT s = MK_SIN(th);
        ra = a;
        ru = c * u - s * v;
        rv = s * u + c * v;
    } else if (kind == MK_WARP_TAPER) {
        MK_FLOAT sc = 1.0 + rate * ac;
        ra = a;
        ru = u * sc;
        rv = v * sc;
    } else if (MK_ABS(rate) < 1.0e-9) {
        ra = a; ru = u; rv = v;
    } else {
        // Bend: the axis coordinate becomes arc length about the centre at u = c; past the
        // frozen end the overshoot continues along the end's tangent.
        MK_FLOAT c = 1.0 / rate;
        MK_FLOAT phi = ac * rate;
        MK_FLOAT rr = c - u;
        MK_FLOAT sn = MK_SIN(phi);
        MK_FLOAT cs = MK_COS(phi);
        ra = rr * sn + over * cs;
        ru = c - rr * cs + over * sn;
        rv = v;
    }
    if (axis == 0)      { ox = ra; oy = ru; oz = rv; }
    else if (axis == 1) { oy = ra; oz = ru; ox = rv; }
    else                { oz = ra; ox = ru; oy = rv; }
}

/// Dispatch by kind. `axis` is 0/1/2 for X/Y/Z, the same encoding as flags::kAxis*. `H` is how
/// far along the axis the children reach; beyond it the warp is frozen at the end section.
///
/// Freezing is what makes the Lipschitz bound finite. Every one of these maps has a place where
/// its inverse stretches without limit -- taper where the section shrinks to nothing, bend at
/// the centre of its circle, twist as the angle runs away -- and a march samples the field far
/// outside the children too. Clamping the axis coordinate to the children's extent turns the
/// space beyond the ends into a straight extension of the end section, whose stretch is exactly
/// the end's, so mkWarpLipschitz(R, H) holds everywhere the ray can be.
MK_FN void mkWarpInv(int kind, int axis, MK_FLOAT rate, MK_FLOAT H,
                     MK_FLOAT x, MK_FLOAT y, MK_FLOAT z,
                     MK_OUT ox, MK_OUT oy, MK_OUT oz) {
    // Into the (a, u, v) frame: a is the chosen axis, (u, v) the other two in right-handed order.
    MK_FLOAT a, u, v;
    if (axis == 0)      { a = x; u = y; v = z; }
    else if (axis == 1) { a = y; u = z; v = x; }
    else                { a = z; u = x; v = y; }

    // The part of a beyond the children's reach rides through untouched: the map is applied to
    // the clamped coordinate and the overshoot is added back along the axis afterwards.
    MK_FLOAT ac = mkClamp(a, -H, H);
    MK_FLOAT over = a - ac;

    MK_FLOAT ra, ru, rv;
    if (kind == MK_WARP_BEND) {
        // A bend freezes inside its own map (see mkBendInv): the axis is an arc there.
        mkBendInv(rate, H, a, u, v, ra, ru, rv);
    } else {
        if (kind == MK_WARP_TWIST) { mkTwistInv(rate, ac, u, v, ra, ru, rv); }
        else                       { mkTaperInv(rate, ac, u, v, ra, ru, rv); }
        ra += over;
    }

    if (axis == 0)      { ox = ra; oy = ru; oz = rv; }
    else if (axis == 1) { oy = ra; oz = ru; ox = rv; }
    else                { oz = ra; ox = ru; oy = rv; }
}

/// Distance from a point to the guard cylinder about the warp's axis -- radius R, half height
/// H -- or 0 inside it. The guard is the region the Lipschitz bound is sized for; outside it the
/// leaf answers with this distance instead of its field, and every evaluator (Eval, the CPU
/// program, the generated shader) uses this one function so they cannot disagree about where
/// that boundary is.
MK_FN MK_FLOAT mkWarpGuardDistance(int axis, MK_FLOAT R, MK_FLOAT H,
                                   MK_FLOAT x, MK_FLOAT y, MK_FLOAT z) {
    MK_FLOAT a, u, v;
    if (axis == 0)      { a = x; u = y; v = z; }
    else if (axis == 1) { a = y; u = z; v = x; }
    else                { a = z; u = x; v = y; }
    MK_FLOAT dr = MK_SQRT(u * u + v * v) - R;
    MK_FLOAT da = MK_ABS(a) - H;
    MK_FLOAT ox = dr > 0.0 ? dr : 0.0;
    MK_FLOAT oy = da > 0.0 ? da : 0.0;
    return MK_SQRT(ox * ox + oy * oy);
}

/// A Lipschitz bound of the inverse map over a subtree of radius R about the axis and half
/// extent H along it, so d(w^-1(p)) / L never overstates the distance. Conservative on purpose:
/// the numbers here are proved rather than fitted, and lattice_gradient_check measures that the
/// numeric gradient of the warped field never exceeds 1.
///
///   Twist   1 + |rate| * R            tangential stretch grows with radius
///   Bend    1 / (1 - |rate| * R)       the inverse map's angular stretch is c / (distance from
///                                       the centre), largest on the side nearest the centre,
///                                       where that distance is c - R; a subtree that reaches
///                                       the centre (|rate| * R >= 1) folds onto itself and has
///                                       no Lipschitz bound, so the rate is clamped there
///   Taper   (1 + |rate| * R / smin) / smin   dividing by s shrinks distances by 1/s and adds a
///                                       shear of rate*u/s^2 along the axis; smin is s at
///                                       the far end, and s reaching zero is the same fold
MK_FN MK_FLOAT mkWarpLipschitz(int kind, MK_FLOAT rate, MK_FLOAT R, MK_FLOAT H) {
    MK_FLOAT ar = MK_ABS(rate);
    if (kind == MK_WARP_TWIST) {
        return 1.0 + ar * R;
    }
    if (kind == MK_WARP_BEND) {
        MK_FLOAT room = 1.0 - ar * R;
        if (room < 1.0e-3) {
            room = 1.0e-3;
        }
        return 1.0 / room;
    }
    MK_FLOAT smin = 1.0 - ar * H;
    if (smin < 1.0e-3) {
        smin = 1.0e-3;
    }
    return (1.0 + ar * R / smin) / smin;
}

#ifdef __cplusplus
    }  // namespace makina
#endif

#endif  // MAKINA_WARP_INCLUDED
