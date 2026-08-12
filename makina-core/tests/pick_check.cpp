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

    if (failures == 0) {
        std::printf("\npicking selects what was meant (%d checks)\n", checks);
        return 0;
    }
    std::printf("\n%d of %d checks FAILED\n", failures, checks);
    return 1;
}
