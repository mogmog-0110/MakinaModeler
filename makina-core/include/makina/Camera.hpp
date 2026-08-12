// The viewport camera, as arithmetic.
//
// No window, no device, no event loop -- this is here rather than in the app because the two
// things that decide whether a modeller feels right are both pure functions of the camera state,
// and both are easy to get subtly wrong in a way that is only findable by feel unless it is
// tested:
//
//   orbit turns around what you are looking at, not around the world origin. With the pivot off
//   screen, the first drag throws the model out of frame and the user learns not to orbit.
//
//   pan moves in proportion to how far away you are. A fixed rate flies across the model when
//   you are close and does nothing when you are pulled back.
//
// Grasp3D's dca118a was exactly these two, found by using it. Having them here means they are
// checked by numbers before anyone has to feel for them, and they stay checked.
//
// Convention: right-handed, Y up, looking down -Z, angles in degrees at the boundary and radians
// inside (COORDINATES.md).

#pragma once

#include "Bounds.hpp"

#include <algorithm>
#include <cmath>

namespace makina {

/// Orbit camera: a pivot, a direction to it, and a distance.
///
/// Stored as pivot + yaw/pitch/distance rather than eye + target, because every operation the
/// user has is naturally expressed in those terms and converting back and forth loses the pivot.
/// An eye/target pair cannot say "orbit around the selection" without being told the pivot again.
struct Camera {
    double pivot[3]{0.0, 0.0, 0.0};
    double yaw = 0.6;      ///< radians, around +Y
    double pitch = 0.5;    ///< radians, positive looks down
    double distance = 5.0;

    double fovY = 35.0;    ///< degrees, vertical
    bool   orthographic = false;
    /// Half-height of the orthographic view, in world units. Kept in step with `distance` and the
    /// field of view when switching, so the switch does not change the apparent size.
    double orthoHeight = 2.0;

    double nearClip = 0.01;
    double farClip = 1000.0;
};

namespace detail {

constexpr double kCamPi = 3.14159265358979323846;

/// Pitch stops just short of straight up or down.
///
/// At exactly +-90 degrees the view direction is parallel to the world up vector and the right
/// vector is undefined -- the frame flips and the model appears to spin. The clamp is what stops
/// that, and it has to be a hair short of the pole rather than at it.
inline double clampPitch(double p) {
    constexpr double kLimit = kCamPi * 0.5 - 1.0e-3;
    return p < -kLimit ? -kLimit : (p > kLimit ? kLimit : p);
}

}  // namespace detail

/// Unit vector from the pivot toward the eye.
inline void cameraOffsetDir(const Camera& c, double out[3]) {
    const double cp = std::cos(c.pitch);
    out[0] = cp * std::sin(c.yaw);
    out[1] = std::sin(c.pitch);
    out[2] = cp * std::cos(c.yaw);
}

inline void cameraEye(const Camera& c, double out[3]) {
    double dir[3];
    cameraOffsetDir(c, dir);
    for (int i = 0; i < 3; ++i) {
        out[i] = c.pivot[i] + dir[i] * c.distance;
    }
}

/// Unit vector from the eye toward the pivot.
inline void cameraForward(const Camera& c, double out[3]) {
    double dir[3];
    cameraOffsetDir(c, dir);
    for (int i = 0; i < 3; ++i) {
        out[i] = -dir[i];
    }
}

/// Screen right and screen up, both unit length.
///
/// Derived from the forward vector and world up rather than stored, so they cannot drift out of
/// agreement with the angles. The pitch clamp is what keeps the cross product well conditioned.
inline void cameraBasis(const Camera& c, double right[3], double up[3]) {
    double fwd[3];
    cameraForward(c, fwd);
    const double worldUp[3] = {0.0, 1.0, 0.0};

    right[0] = fwd[1] * worldUp[2] - fwd[2] * worldUp[1];
    right[1] = fwd[2] * worldUp[0] - fwd[0] * worldUp[2];
    right[2] = fwd[0] * worldUp[1] - fwd[1] * worldUp[0];
    double len = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    if (len < 1e-12) {
        right[0] = 1.0;
        right[1] = 0.0;
        right[2] = 0.0;
        len = 1.0;
    }
    for (int i = 0; i < 3; ++i) {
        right[i] /= len;
    }

    up[0] = right[1] * fwd[2] - right[2] * fwd[1];
    up[1] = right[2] * fwd[0] - right[0] * fwd[2];
    up[2] = right[0] * fwd[1] - right[1] * fwd[0];
}

// ---------------------------------------------------------------- operations

/// Turns the camera around its pivot.
///
/// `dx`/`dy` are in screen fractions -- a drag across the full width is 1.0 -- so the same gesture
/// covers the same arc whatever the window size. Pixels would make the same drag turn further on
/// a small window.
inline Camera orbit(const Camera& c, double dx, double dy, double turnsPerScreen = 1.0) {
    Camera r = c;
    const double radians = 2.0 * detail::kCamPi * turnsPerScreen;
    r.yaw -= dx * radians;
    r.pitch = detail::clampPitch(c.pitch + dy * radians * 0.5);
    return r;
}

/// Slides the camera and its pivot across the view plane.
///
/// The rate is the world size of one screen at the pivot's depth, which is what makes a drag hold
/// the same point under the cursor whether the model fills the screen or sits far away. A fixed
/// rate is the single most common way a viewport comes out feeling wrong.
///
/// `aspect` is not optional and not decorative: the horizontal span is the vertical one times the
/// aspect, so leaving it out makes a sideways drag lag the cursor by that factor -- 1.78x on a
/// 16:9 window, which reads as the model sticking.
inline Camera pan(const Camera& c, double dx, double dy, double aspect) {
    Camera r = c;
    double right[3], up[3];
    cameraBasis(c, right, up);

    const double screenHeight = c.orthographic
                                    ? 2.0 * c.orthoHeight
                                    : 2.0 * c.distance * std::tan(c.fovY * detail::kCamPi / 360.0);
    const double screenWidth = screenHeight * (aspect > 0.0 ? aspect : 1.0);

    for (int i = 0; i < 3; ++i) {
        r.pivot[i] = c.pivot[i] - right[i] * dx * screenWidth + up[i] * dy * screenHeight;
    }
    return r;
}

/// Moves toward or away from the pivot.
///
/// Multiplicative, not additive: every notch covers the same *proportion* of the remaining
/// distance, so approaching a small detail never overshoots past it and pulling back from a large
/// model does not take fifty notches. The floor keeps the camera off the pivot, where the basis
/// stops being defined.
inline Camera dolly(const Camera& c, double amount, double sceneRadius = 1.0) {
    Camera r = c;
    const double factor = std::pow(1.1, -amount);
    const double floorDistance = sceneRadius * 1e-3;

    r.distance = c.distance * factor;
    if (r.distance < floorDistance) {
        r.distance = floorDistance;
    }
    r.orthoHeight = c.orthoHeight * factor;
    if (r.orthoHeight < floorDistance) {
        r.orthoHeight = floorDistance;
    }
    return r;
}

/// Zooms toward a point rather than toward the middle of the screen.
///
/// `u`/`v` are the cursor position in screen fractions from the centre, so -0.5..0.5. The pivot
/// slides toward the cursor by the fraction of the distance the zoom just removed, which is what
/// makes "put the cursor on it and scroll" work. Without it, zooming in on a corner of the model
/// pushes that corner off the screen.
inline Camera dollyToCursor(const Camera& c, double amount, double u, double v, double aspect,
                            double sceneRadius = 1.0) {
    const Camera zoomed = dolly(c, amount, sceneRadius);

    double right[3], up[3];
    cameraBasis(c, right, up);
    const double screenHeight = c.orthographic
                                    ? 2.0 * c.orthoHeight
                                    : 2.0 * c.distance * std::tan(c.fovY * detail::kCamPi / 360.0);
    const double screenWidth = screenHeight * (aspect > 0.0 ? aspect : 1.0);
    // How much of the way to the cursor the pivot travels: exactly the fraction of the view the
    // zoom removed, so the point under the cursor stays under it.
    const double closed = c.orthographic ? 1.0 - zoomed.orthoHeight / c.orthoHeight
                                         : 1.0 - zoomed.distance / c.distance;

    Camera r = zoomed;
    for (int i = 0; i < 3; ++i) {
        r.pivot[i] = c.pivot[i] + (right[i] * u * screenWidth + up[i] * v * screenHeight) * closed;
    }
    return r;
}

/// Frames a box: pivot at its centre, far enough back that it fits.
///
/// Fits the bounding *sphere*, not the box, so the framing does not change as the camera turns --
/// filling the frame exactly from one angle means clipping from another. `margin` is the slack,
/// as a multiplier.
///
/// A degenerate box (a single point, a flat plate seen edge on) still gets a usable distance,
/// because a radius of zero would put the camera inside the pivot.
inline Camera frameBox(const Camera& c, const Aabb& box, double aspect, double margin = 1.15) {
    if (!box.valid) {
        return c;
    }
    Camera r = c;

    double radius = 0.0;
    for (int i = 0; i < 3; ++i) {
        r.pivot[i] = (box.lo[i] + box.hi[i]) * 0.5;
        const double half = (box.hi[i] - box.lo[i]) * 0.5;
        radius += half * half;
    }
    radius = std::sqrt(radius);
    if (radius < 1e-6) {
        radius = 1e-6;
    }

    const double tanHalfV = std::tan(c.fovY * detail::kCamPi / 360.0);
    // The narrower of the two half-angles decides: a wide flat model has to fit vertically, a
    // tall one horizontally, and using only the vertical one clips the tall case.
    const double tanHalfH = tanHalfV * (aspect > 0.0 ? aspect : 1.0);
    const double tanHalf = tanHalfV < tanHalfH ? tanHalfV : tanHalfH;

    r.distance = radius * margin / tanHalf;
    r.orthoHeight = radius * margin;
    return r;
}

/// The six axis views. `back` gives the opposite side, which is what Ctrl does in Blender.
enum class ViewAxis { Front, Right, Top };

inline Camera lookAlong(const Camera& c, ViewAxis axis, bool back = false) {
    Camera r = c;
    const double flip = back ? detail::kCamPi : 0.0;
    switch (axis) {
        case ViewAxis::Front:
            r.yaw = 0.0 + flip;
            r.pitch = 0.0;
            break;
        case ViewAxis::Right:
            r.yaw = detail::kCamPi * 0.5 + flip;
            r.pitch = 0.0;
            break;
        case ViewAxis::Top:
            // Not exactly the pole: at 90 degrees the frame is undefined and the view rolls.
            r.yaw = c.yaw;
            r.pitch = detail::clampPitch(back ? -detail::kCamPi * 0.5 : detail::kCamPi * 0.5);
            break;
    }
    return r;
}

/// Switches projection without changing how large the model looks.
///
/// The half-height the perspective camera covers at the pivot is what the orthographic camera
/// takes, and the reverse on the way back. Switching should change the *kind* of projection, not
/// the framing; a switch that also rescales makes the two modes feel like two different scenes.
inline Camera setOrthographic(const Camera& c, bool ortho) {
    if (ortho == c.orthographic) {
        return c;
    }
    Camera r = c;
    r.orthographic = ortho;
    const double tanHalfV = std::tan(c.fovY * detail::kCamPi / 360.0);
    if (ortho) {
        r.orthoHeight = c.distance * tanHalfV;
    } else {
        r.distance = c.orthoHeight / (tanHalfV > 1e-9 ? tanHalfV : 1e-9);
    }
    return r;
}

/// Ray through a point on the screen, for picking.
///
/// `u`/`v` are fractions from the centre of the screen, -0.5..0.5, with +v up. Under an
/// orthographic camera the origin moves and the direction is constant; under a perspective one it
/// is the other way round, and a picker that assumed perspective would pick from the middle of
/// the screen in orthographic mode.
inline void cameraRay(const Camera& c, double u, double v, double aspect, double origin[3],
                      double dir[3]) {
    double right[3], up[3], fwd[3];
    cameraBasis(c, right, up);
    cameraForward(c, fwd);
    double eye[3];
    cameraEye(c, eye);

    if (c.orthographic) {
        const double h = c.orthoHeight;
        for (int i = 0; i < 3; ++i) {
            origin[i] = eye[i] + right[i] * (u * 2.0 * h * aspect) + up[i] * (v * 2.0 * h);
            dir[i] = fwd[i];
        }
        return;
    }

    const double tanHalfV = std::tan(c.fovY * detail::kCamPi / 360.0);
    double d[3];
    for (int i = 0; i < 3; ++i) {
        origin[i] = eye[i];
        d[i] = fwd[i] + right[i] * (u * 2.0 * tanHalfV * aspect) + up[i] * (v * 2.0 * tanHalfV);
    }
    const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int i = 0; i < 3; ++i) {
        dir[i] = len > 1e-12 ? d[i] / len : fwd[i];
    }
}

}  // namespace makina
