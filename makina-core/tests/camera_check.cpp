// Checks the two camera behaviours that decide whether a viewport feels right.
//
// Both are the kind of thing normally found by using the tool and saying "this is wrong" -- which
// is how Grasp3D found them (dca118a). Once found, they are ordinary arithmetic and can be pinned:
//
//   orbit turns around the pivot     the model stays where you are looking
//   pan holds the point under the    a drag moves the world by exactly what the cursor moved,
//   cursor                           at any distance and any aspect
//
// The pan check is written as "the point that was under the cursor is now at the centre", not as
// "the pivot moved by k times the distance". The second version passes for a camera that pans at
// the right rate in the wrong direction, and for one that ignores the aspect. Verified by taking
// the aspect back out of pan: this file then fails on six of its nine distance-by-aspect cases,
// by a tenth of the model's size, while a rate-only check would still have passed.

#include <makina/Camera.hpp>

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        std::printf("    FAIL  %s\n", what.c_str());
        ++failures;
    }
}

void checkNear(double got, double want, double tol, const std::string& what) {
    ++checks;
    if (std::fabs(got - want) > tol) {
        std::printf("    FAIL  %s: got %.9g want %.9g\n", what.c_str(), got, want);
        ++failures;
    }
}

double dist(const double a[3], const double b[3]) {
    double d = 0.0;
    for (int i = 0; i < 3; ++i) {
        d += (a[i] - b[i]) * (a[i] - b[i]);
    }
    return std::sqrt(d);
}

/// Where the ray through screen (u,v) crosses the plane through the pivot facing the camera.
///
/// That plane is what a drag is understood to move things on: the depth the user is working at.
void pointUnderCursor(const makina::Camera& c, double u, double v, double aspect, double out[3]) {
    double origin[3], dir[3], fwd[3];
    makina::cameraRay(c, u, v, aspect, origin, dir);
    makina::cameraForward(c, fwd);

    // Distance along the ray to the pivot plane. dot(dir, fwd) is never zero for a ray inside the
    // frustum, so no guard is needed beyond a sanity floor.
    double toPivot[3];
    for (int i = 0; i < 3; ++i) {
        toPivot[i] = c.pivot[i] - origin[i];
    }
    const double num = toPivot[0] * fwd[0] + toPivot[1] * fwd[1] + toPivot[2] * fwd[2];
    const double den = dir[0] * fwd[0] + dir[1] * fwd[1] + dir[2] * fwd[2];
    const double t = num / (std::fabs(den) > 1e-12 ? den : 1e-12);
    for (int i = 0; i < 3; ++i) {
        out[i] = origin[i] + dir[i] * t;
    }
}

void orbitTurnsAroundThePivot() {
    std::printf("orbit turns around the pivot\n");

    makina::Camera c;
    c.pivot[0] = 3.0;
    c.pivot[1] = 1.0;
    c.pivot[2] = -2.0;
    c.distance = 7.0;

    double eyeBefore[3];
    makina::cameraEye(c, eyeBefore);

    const makina::Camera turned = makina::orbit(c, 0.25, 0.1);

    for (int i = 0; i < 3; ++i) {
        checkNear(turned.pivot[i], c.pivot[i], 1e-12, "the pivot moved");
    }
    checkNear(turned.distance, c.distance, 1e-12, "the distance changed");

    // The eye has to have moved, and to have stayed on the sphere around the pivot. A camera that
    // orbits the world origin passes the first and fails the second whenever the pivot is not at
    // the origin -- which is the whole bug.
    double eyeAfter[3];
    makina::cameraEye(turned, eyeAfter);
    check(dist(eyeBefore, eyeAfter) > 0.1, "the eye did not move");
    checkNear(dist(eyeAfter, turned.pivot), c.distance, 1e-9, "the eye left the sphere");

    // Straight up and straight down are clamped short of the pole, where the frame is undefined.
    const makina::Camera up = makina::orbit(c, 0.0, 10.0);
    check(std::fabs(up.pitch) < 1.5708, "pitch reached the pole");
    const makina::Camera down = makina::orbit(c, 0.0, -10.0);
    check(std::fabs(down.pitch) < 1.5708, "pitch reached the pole downward");

    std::printf("    pivot fixed, eye on the sphere, pitch clamped\n");
}

void panHoldsThePointUnderTheCursor() {
    std::printf("pan holds the point under the cursor\n");

    // Deliberately awkward: a distance far from 1, an aspect far from 1, and an oblique view.
    // Each of those hides a different mistake -- a rate that ignores distance, one that ignores
    // aspect, and one that uses world axes instead of the screen basis.
    for (const double distance : {0.4, 5.0, 120.0}) {
        for (const double aspect : {1.0, 16.0 / 9.0, 0.5}) {
            makina::Camera c;
            c.pivot[0] = -2.0;
            c.pivot[1] = 0.5;
            c.pivot[2] = 4.0;
            c.yaw = 0.9;
            c.pitch = 0.4;
            c.distance = distance;

            const double u = 0.17;
            const double v = -0.23;

            double target[3];
            pointUnderCursor(c, u, v, aspect, target);

            // Drag by exactly the cursor offset. Afterwards that world point has to sit at the
            // centre of the screen, which is the pivot.
            const makina::Camera panned = makina::pan(c, -u, v, aspect);

            const double moved = dist(panned.pivot, target);
            const double scale = distance;
            checkNear(moved / scale, 0.0, 1e-9,
                      "the dragged point did not land at the centre (distance " +
                          std::to_string(distance) + ", aspect " + std::to_string(aspect) + ")");
        }
    }

    // And the rate really is proportional: the same drag at ten times the distance moves ten
    // times as far. This is the part that was wrong in Grasp3D.
    makina::Camera near;
    near.distance = 1.0;
    makina::Camera far = near;
    far.distance = 10.0;
    const double a = dist(makina::pan(near, 0.3, 0.0, 1.0).pivot, near.pivot);
    const double b = dist(makina::pan(far, 0.3, 0.0, 1.0).pivot, far.pivot);
    checkNear(b / a, 10.0, 1e-9, "pan does not scale with distance");

    std::printf("    the point under the cursor stays under it, at 3 distances x 3 aspects\n");
}

void zoomGoesTowardTheCursor() {
    std::printf("zoom goes toward the cursor\n");

    makina::Camera c;
    c.distance = 10.0;
    const double aspect = 16.0 / 9.0;
    const double u = 0.3, v = 0.2;

    double target[3];
    pointUnderCursor(c, u, v, aspect, target);
    const double before = dist(c.pivot, target);

    const makina::Camera zoomed = makina::dollyToCursor(c, 1.0, u, v, aspect);
    const double after = dist(zoomed.pivot, target);

    check(zoomed.distance < c.distance, "zooming in did not shorten the distance");
    check(after < before, "the pivot did not move toward the cursor");

    // Zooming out has to be the mirror image, or repeated in-and-out drifts the model away.
    const makina::Camera out = makina::dollyToCursor(c, -1.0, u, v, aspect);
    check(out.distance > c.distance, "zooming out did not lengthen the distance");
    check(dist(out.pivot, target) > before, "zooming out moved toward the cursor");

    // A notch in and a notch out returns to where it started, near enough. Any asymmetry here
    // shows up as the view creeping while the user scrolls back and forth.
    const makina::Camera roundTrip = makina::dollyToCursor(zoomed, -1.0, u, v, aspect);
    checkNear(roundTrip.distance, c.distance, 1e-9, "in then out changed the distance");

    std::printf("    in shortens, out lengthens, and one of each returns\n");
}

void fitFramesTheBox() {
    std::printf("fit frames the box\n");

    makina::Aabb box{};
    box.valid = true;
    box.lo[0] = -1.0;  box.lo[1] = 0.0;  box.lo[2] = -3.0;
    box.hi[0] = 5.0;   box.hi[1] = 2.0;  box.hi[2] = 1.0;

    makina::Camera c;
    const makina::Camera fitted = makina::frameBox(c, box, 16.0 / 9.0);

    for (int i = 0; i < 3; ++i) {
        checkNear(fitted.pivot[i], (box.lo[i] + box.hi[i]) * 0.5, 1e-12,
                  "fit did not centre on the box");
    }

    // Every corner has to be inside the frustum, or "fit" clipped something. Checked against the
    // narrower half-angle, which is the one that decides.
    double right[3], up[3], fwd[3], eye[3];
    makina::cameraBasis(fitted, right, up);
    makina::cameraForward(fitted, fwd);
    makina::cameraEye(fitted, eye);
    const double tanHalfV = std::tan(fitted.fovY * 3.14159265358979323846 / 360.0);
    const double tanHalfH = tanHalfV * 16.0 / 9.0;

    int outside = 0;
    for (int corner = 0; corner < 8; ++corner) {
        const double p[3] = {(corner & 1) ? box.hi[0] : box.lo[0],
                             (corner & 2) ? box.hi[1] : box.lo[1],
                             (corner & 4) ? box.hi[2] : box.lo[2]};
        double d[3];
        for (int i = 0; i < 3; ++i) {
            d[i] = p[i] - eye[i];
        }
        const double z = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
        const double x = d[0] * right[0] + d[1] * right[1] + d[2] * right[2];
        const double y = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
        if (z <= 0.0 || std::fabs(x) > z * tanHalfH || std::fabs(y) > z * tanHalfV) {
            ++outside;
        }
    }
    check(outside == 0, "fit left " + std::to_string(outside) + " corner(s) outside the frustum");

    // A flat plate seen edge on still has to produce a usable camera rather than one sitting on
    // top of the pivot.
    makina::Aabb flat{};
    flat.valid = true;
    flat.lo[0] = -1.0;  flat.lo[1] = 0.0;  flat.lo[2] = -1.0;
    flat.hi[0] = 1.0;   flat.hi[1] = 0.0;  flat.hi[2] = 1.0;
    check(makina::frameBox(c, flat, 1.0).distance > 1e-3, "a flat box gave a degenerate distance");

    std::printf("    centred, every corner inside, degenerate boxes survive\n");
}

void projectionSwitchKeepsTheFraming() {
    std::printf("the projection switch keeps the framing\n");

    makina::Camera c;
    c.distance = 12.0;
    const makina::Camera ortho = makina::setOrthographic(c, true);

    check(ortho.orthographic, "the switch did not take");
    // The orthographic half-height has to be what the perspective camera covered at the pivot,
    // or the model jumps in size the moment the key is pressed.
    const double want = c.distance * std::tan(c.fovY * 3.14159265358979323846 / 360.0);
    checkNear(ortho.orthoHeight, want, 1e-9, "the model would change size on switching");

    const makina::Camera back = makina::setOrthographic(ortho, false);
    checkNear(back.distance, c.distance, 1e-9, "switching back moved the camera");

    std::printf("    apparent size preserved, and the round trip returns\n");
}

void axisViewsLookAlongTheAxes() {
    std::printf("axis views look along the axes\n");

    makina::Camera c;

    double fwd[3];
    makina::cameraForward(makina::lookAlong(c, makina::ViewAxis::Front), fwd);
    checkNear(fwd[2], -1.0, 1e-9, "front does not look down -Z");

    makina::cameraForward(makina::lookAlong(c, makina::ViewAxis::Front, true), fwd);
    checkNear(fwd[2], 1.0, 1e-9, "back does not look down +Z");

    makina::cameraForward(makina::lookAlong(c, makina::ViewAxis::Right), fwd);
    checkNear(fwd[0], -1.0, 1e-9, "right does not look down -X");

    makina::cameraForward(makina::lookAlong(c, makina::ViewAxis::Top), fwd);
    checkNear(fwd[1], -1.0, 2e-3, "top does not look down");

    // Top stops a hair short of straight down on purpose; at the pole the frame is undefined and
    // the view rolls. The tolerance above is that hair.
    double right[3], up[3];
    makina::cameraBasis(makina::lookAlong(c, makina::ViewAxis::Top), right, up);
    const double len = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    checkNear(len, 1.0, 1e-9, "the top view has no usable right vector");

    std::printf("    front / back / right / top, and top keeps a frame\n");
}

}  // namespace

int main() {
    std::printf("makina-core viewport camera\n\n");

    orbitTurnsAroundThePivot();
    panHoldsThePointUnderTheCursor();
    zoomGoesTowardTheCursor();
    fitFramesTheBox();
    projectionSwitchKeepsTheFraming();
    axisViewsLookAlongTheAxes();

    if (failures == 0) {
        std::printf("\nthe camera behaves (%d checks)\n", checks);
        return 0;
    }
    std::printf("\n%d of %d checks FAILED\n", failures, checks);
    return 1;
}
