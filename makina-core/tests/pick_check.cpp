// Checks that clicking selects what the user meant.
//
// Two separate things can be wrong here and they fail differently:
//
//   the ray misses      nothing gets selected, which is obvious and gets fixed
//   the ray hits but    something gets selected -- the wrong something. On a bore that is the
//   the rule is wrong   cylinder that cut it, and the user sees a selection appear around a hole
//                       they clicked. This is the failure worth testing for.
//
// So the checks are mostly about the hierarchy rule, not about the intersection.

#include <makina/Pick.hpp>
#include <vector>
#include <makina/SceneJson.hpp>

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

/// A plate with a bore through it, plus a ball off to one side.
///
/// The bore is what makes this worth testing: a ray down the middle lands on the wall of the
/// subtracted cylinder, so the nearest primitive is the blade.
constexpr const char* kScene = R"({
  "format": "makina-scene",
  "version": 1,
  "nextId": 20,
  "materials": [],
  "root": { "op": "SceneRoot", "id": 1, "name": "Scene", "children": [
    { "op": "Difference", "id": 2, "name": "plate", "children": [
      { "op": "Cylinder", "id": 3, "name": "body", "capPoint": 0.5, "basePoint": -0.5, "radius": 2.0 },
      { "op": "Cylinder", "id": 4, "name": "bore", "capPoint": 2.0, "basePoint": -2.0, "radius": 0.6 }
    ]},
    { "op": "Translate", "id": 5, "name": "offset", "x": 5.0, "y": 0.0, "z": 0.0, "children": [
      { "op": "Sphere", "id": 6, "name": "ball", "radius": 0.8 }
    ]}
  ]}
})";

makina::PickResult down(const makina::Scene& s, double x, double z, int depth = 0) {
    const double origin[3] = {x, 10.0, z};
    const double dir[3] = {0.0, -1.0, 0.0};
    return makina::pick(s, origin, dir, 100.0, depth);
}

}  // namespace

int main() {
    std::printf("makina-core picking\n\n");

    const makina::Scene s = makina::parseScene(kScene);

    // --- the plain cases -------------------------------------------------------------------
    {
        const makina::PickResult onPlate = down(s, 1.4, 0.0);
        check(onPlate.hit, "the ray missed the plate");
        check(onPlate.id == 2, "clicking the plate did not select the difference, got id " +
                                   std::to_string(onPlate.id));

        const makina::PickResult onBall = down(s, 5.0, 0.0);
        check(onBall.hit, "the ray missed the ball");
        check(onBall.id == 5, "clicking the ball did not select its transform, got id " +
                                  std::to_string(onBall.id));

        const makina::PickResult nothing = down(s, 30.0, 30.0);
        check(!nothing.hit, "the ray hit empty space");
    }

    // --- the case the rule exists for ------------------------------------------------------
    {
        // A vertical ray anywhere inside the bore goes straight through -- the wall is vertical
        // and the ray is parallel to it, so it never meets. (Written the other way first, which
        // is a good reminder that "click the hole" is not "aim down the hole".)
        check(!down(s, 0.3, 0.0).hit, "a ray down the middle of the bore hit something");

        // The wall is what you see when you look into the hole from an angle. Here: from the
        // centre of the bore, outward.
        const double origin[3] = {0.0, 0.0, 0.0};
        const double dir[3] = {1.0, 0.0, 0.0};
        const makina::PickResult onBoreWall = makina::pick(s, origin, dir, 10.0);
        check(onBoreWall.hit, "the ray missed the bore wall");
        check(std::fabs(onBoreWall.point[0] - 0.6) < 0.01,
              "the ray stopped somewhere other than the bore wall");

        // The primitive there really is the blade -- that is why the rule is needed.
        check(onBoreWall.primitiveId == 4,
              "the nearest primitive on the bore wall was not the blade, got id " +
                  std::to_string(onBoreWall.primitiveId));

        // And what gets selected is the plate, not the cylinder that was subtracted from it.
        check(onBoreWall.id == 2,
              "clicking the bore selected the blade instead of the plate, got id " +
                  std::to_string(onBoreWall.id));
    }

    // --- descending, one level at a time ---------------------------------------------------
    {
        const makina::PickResult outer = down(s, 1.4, 0.0, 0);
        check(outer.id == 2 && outer.remainingDepth == 1,
              "the ladder from the plate body is not two rungs deep");

        const makina::PickResult inner = down(s, 1.4, 0.0, 1);
        check(inner.id == 3, "descending once did not reach the body, got id " +
                                 std::to_string(inner.id));
        check(inner.remainingDepth == 0, "the ladder did not end at the primitive");

        // Holding the modifier past the bottom settles rather than losing the selection.
        const makina::PickResult tooFar = down(s, 1.4, 0.0, 9);
        check(tooFar.id == 3, "descending past the bottom lost the selection");

        // And a negative depth is treated as the top rather than wrapping.
        check(down(s, 1.4, 0.0, -3).id == 2, "a negative depth did not clamp to the top");
    }

    // --- through a camera --------------------------------------------------------------------
    {
        // The camera path has to agree with the ray path, or a click lands somewhere other than
        // where the cursor is. Framing the scene puts the plate at the centre of the screen.
        makina::Camera c;
        const makina::BoundsResult b = makina::worldBounds(s);
        c = makina::frameBox(c, b.box, 16.0 / 9.0);

        const makina::PickResult centre = makina::pickThroughCamera(s, c, 0.0, 0.0, 16.0 / 9.0);
        check(centre.hit, "picking through the camera missed the framed scene");

        // Far outside the model, in a corner of the screen, has to miss.
        const makina::PickResult corner =
            makina::pickThroughCamera(s, c, 0.49, 0.49, 16.0 / 9.0);
        check(!corner.hit, "a click in the corner hit something");
    }

    // ---------------------------------------------------------------- projection
    //
    // Tested as the inverse of cameraRay rather than against numbers written out by hand. A
    // projection and a ray generator that were each "obviously right" separately are exactly how a
    // click comes to land somewhere the picture does not agree with.
    {
        const makina::Scene s = makina::parseScene(kScene);
        const double aspect = 16.0 / 9.0;
        makina::Camera c = makina::frameBox(makina::Camera{}, makina::worldBounds(s).box, aspect);

        for (int mode = 0; mode < 2; ++mode) {
            c = makina::setOrthographic(c, mode == 1);
            const double points[3][3] = {{0.0, 0.0, 0.0}, {1.3, 0.4, -0.7}, {-2.0, 1.0, 0.5}};
            for (const double(&p)[3] : points) {
                double u = 0.0, v = 0.0;
                check(makina::projectToScreen(c, p, aspect, u, v),
                      "a point in front of the camera has no screen position");

                double origin[3], dir[3];
                makina::cameraRay(c, u, v, aspect, origin, dir);
                // The ray through the projected point has to pass through the point: the vector
                // from its origin to the point must be parallel to its direction.
                const double to[3] = {p[0] - origin[0], p[1] - origin[1], p[2] - origin[2]};
                const double len = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
                double cross = 0.0;
                if (len > 1e-9) {
                    const double n[3] = {to[0] / len, to[1] / len, to[2] / len};
                    const double cx = n[1] * dir[2] - n[2] * dir[1];
                    const double cy = n[2] * dir[0] - n[0] * dir[2];
                    const double cz = n[0] * dir[1] - n[1] * dir[0];
                    cross = std::sqrt(cx * cx + cy * cy + cz * cz);
                }
                check(cross < 1e-9, std::string(mode == 1 ? "orthographic" : "perspective") +
                                        ": the ray through a projected point misses it");
            }
        }

        // Behind a perspective eye there is no screen position. The naive formula produces one,
        // mirrored through the centre, and anything built on it selects things behind the camera.
        c = makina::setOrthographic(c, false);
        double eye[3], fwd[3];
        makina::cameraEye(c, eye);
        makina::cameraForward(c, fwd);
        const double back[3] = {eye[0] - fwd[0], eye[1] - fwd[1], eye[2] - fwd[2]};
        double u = 0.0, v = 0.0;
        check(!makina::projectToScreen(c, back, aspect, u, v),
              "a point behind the eye was given a screen position");
    }

    // ---------------------------------------------------------------- rectangle
    {
        const makina::Scene s = makina::parseScene(kScene);
        const double aspect = 16.0 / 9.0;
        const makina::Camera c =
            makina::frameBox(makina::Camera{}, makina::worldBounds(s).box, aspect);

        const std::vector<std::uint32_t> all =
            makina::pickInRect(s, c, -0.5, -0.5, 0.5, 0.5, aspect);
        check(all.size() >= 2, "a rectangle over the whole frame caught fewer than both boxes");

        const std::vector<std::uint32_t> none =
            makina::pickInRect(s, c, 0.48, 0.48, 0.49, 0.49, aspect);
        check(none.empty(), "a rectangle in the corner caught something");

        // What the rectangle selects has to be what clicking selects, or the two tools disagree
        // about what an object is.
        const makina::PickResult clicked = makina::pickThroughCamera(s, c, 0.0, 0.0, aspect);
        const std::vector<std::uint32_t> centre =
            makina::pickInRect(s, c, -0.02, -0.02, 0.02, 0.02, aspect);
        bool agrees = false;
        for (const std::uint32_t id : centre) {
            if (id == clicked.id) {
                agrees = true;
            }
        }
        check(clicked.hit && agrees, "a rectangle over the centre missed what a click there picks");

        // The corners may be given in any order: a drag runs in whichever direction the hand went.
        const std::vector<std::uint32_t> backwards =
            makina::pickInRect(s, c, 0.5, 0.5, -0.5, -0.5, aspect);
        check(backwards == all, "dragging the rectangle the other way selected something else");
    }

    if (failures == 0) {
        std::printf("\npicking selects what was meant (%d checks)\n", checks);
        return 0;
    }
    std::printf("\n%d of %d checks FAILED\n", failures, checks);
    return 1;
}
