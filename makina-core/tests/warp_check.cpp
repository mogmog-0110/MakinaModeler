// Domain warps against what can be said about them without a second implementation (D-14).
//
// The two independent-implementation axes this project leans on do not reach a warped field:
// the BSP is affine-only, and POV has no twist. So the checks here are the invariants the maths
// gives for free, plus the one property everything downstream depends on -- that the field is
// still a lower bound on distance, which the numeric gradient measures directly. A gradient
// above one anywhere means a march can step through the surface, and it is exactly the failure
// the Lipschitz division in Warp.hpp exists to prevent.
//
// The picture is checked elsewhere: pov_compare draws the warped solid through POV's isosurface
// once the exporter writes one.

#include <makina/Bounds.hpp>
#include <makina/Eval.hpp>
#include <makina/Flatten.hpp>
#include <makina/SceneJson.hpp>

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int checks = 0;
int failures = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        std::printf("    FAIL  %s\n", what.c_str());
        ++failures;
    }
}

/// A tall thin box, so a twist about Y visibly turns it and a bend visibly curves it.
std::string sceneWith(const std::string& warp) {
    return "{\"format\":\"makina-scene\",\"version\":1,\"nextId\":4,\"materials\":[],"
           "\"root\":{\"id\":1,\"op\":\"SceneRoot\",\"children\":[" + warp +
           "]}}";
}

std::string boxUnder(const char* op, const char* key, double rate, const char* axis) {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "{\"id\":2,\"op\":\"%s\",\"%s\":%g,\"axis\":\"%s\",\"children\":["
                  "{\"id\":3,\"op\":\"Box\",\"x1\":-0.3,\"y1\":-1,\"z1\":-0.08,"
                  "\"x2\":0.3,\"y2\":1,\"z2\":0.08}]}",
                  op, key, rate, axis);
    return buf;
}

double at(const makina::Scene& s, double x, double y, double z) {
    const double p[3] = {x, y, z};
    return makina::eval(s, p);
}

/// Central-difference gradient magnitude at p.
double gradAt(const makina::Scene& s, double x, double y, double z) {
    const double h = 1e-4;
    const double gx = (at(s, x + h, y, z) - at(s, x - h, y, z)) / (2 * h);
    const double gy = (at(s, x, y + h, z) - at(s, x, y - h, z)) / (2 * h);
    const double gz = (at(s, x, y, z + h) - at(s, x, y, z - h)) / (2 * h);
    return std::sqrt(gx * gx + gy * gy + gz * gz);
}

/// The Lipschitz property, sampled on a lattice around the solid: the field must never climb
/// faster than distance. Reported as the worst gradient found so a regression shows as a number.
double worstGradient(const makina::Scene& s, double reach) {
    double worst = 0.0;
    constexpr int kSteps = 13;
    for (int i = 0; i < kSteps; ++i) {
        for (int j = 0; j < kSteps; ++j) {
            for (int k = 0; k < kSteps; ++k) {
                const double x = -reach + 2 * reach * i / (kSteps - 1);
                const double y = -reach + 2 * reach * j / (kSteps - 1);
                const double z = -reach + 2 * reach * k / (kSteps - 1);
                const double g = gradAt(s, x, y, z);
                if (g > worst) {
                    worst = g;
                }
            }
        }
    }
    return worst;
}

void twist() {
    std::printf("twist about Y\n");
    const makina::Scene s = makina::parseScene(sceneWith(boxUnder("Twist", "degreesPerUnit", 90, "Y")));
    check(s.nodes.count == 3, "three nodes expected");

    // The axis is the fixed line of every rotation, so points on it read the untwisted box.
    const makina::Scene plain = makina::parseScene(sceneWith(boxUnder("Twist", "degreesPerUnit", 0, "Y")));
    // Only the Lipschitz division separates the two values there, so the ratio is the bound.
    check(at(s, 0, 0.5, 0) < 0 && at(plain, 0, 0.5, 0) < 0, "the axis is inside both");
    check(at(s, 0, 0.5, 0) > at(plain, 0, 0.5, 0),
          "the twisted field is the plain one divided by L > 1 (less negative inside)");

    // At y = 1 the box has turned 90 degrees about Y, right-handed like Rotate: +x has gone to
    // -z. So a point out along -z at y=1 is inside, and the same point out along x is not.
    // Sampled just under the top face: y=1 is the face itself, where the field is exactly 0.
    check(at(s, 0, 0.98, -0.25) < 0, "near y=1 the box should have turned onto -z (inside)");
    check(at(s, 0.25, 0.98, 0) > 0, "near y=1 the old x side should be gone (outside)");
    // And at y = 0 nothing has turned.
    check(at(s, 0.25, 0, 0) < 0 && at(s, 0, 0, 0.25) > 0, "at y=0 the box is untwisted");

    // The Lipschitz division must hold: the field never climbs faster than distance.
    const double g = worstGradient(s, 1.4);
    std::printf("    worst gradient %.4f\n", g);
    check(g <= 1.0 + 1e-3, "twisted field gradient exceeds 1: the march could step through");

    // Rate zero is the identity, to the bit.
    const makina::Scene box = makina::parseScene(
        "{\"format\":\"makina-scene\",\"version\":1,\"nextId\":3,\"materials\":[],"
        "\"root\":{\"id\":1,\"op\":\"SceneRoot\",\"children\":[{\"id\":2,\"op\":\"Box\","
        "\"x1\":-0.3,\"y1\":-1,\"z1\":-0.08,\"x2\":0.3,\"y2\":1,\"z2\":0.08}]}}");
    check(at(plain, 0.4, 0.3, 0.1) == at(box, 0.4, 0.3, 0.1), "rate 0 should be the identity");
}

void bend() {
    std::printf("bend along Y\n");
    // A bend wraps its axis onto a circle, so to curve the tall box the bend axis is Y (the box's
    // length). Rate 60 deg/unit: the circle's radius is c = 1/rate ~ 0.955, its centre at z = c.
    const makina::Scene s = makina::parseScene(sceneWith(boxUnder("Bend", "degreesPerUnit", 60, "Y")));
    check(s.nodes.count == 3, "three nodes expected");

    // The origin is a fixed point of a bend.
    check(at(s, 0, 0, 0) < 0, "the origin stays inside");
    // The top of the box (y=1) has swung 60 degrees about the centre, toward +z, so the point
    // straight up at (0, 1, 0) is now outside, and the swung top is inside.
    check(at(s, 0, 1, 0) > 0, "the straight-up point should have left the bent box");
    const double rad = 60.0 * 3.14159265358979323846 / 180.0;
    const double c = 1.0 / rad;
    const double topY = c * std::sin(rad);
    const double topZ = c - c * std::cos(rad);
    check(at(s, 0, topY * 0.98, topZ * 0.98) < 0, "the swung top of the box should be inside");

    const double g = worstGradient(s, 1.4);
    std::printf("    worst gradient %.4f\n", g);
    check(g <= 1.0 + 1e-3, "bent field gradient exceeds 1: the march could step through");
}

void taper() {
    std::printf("taper along Y\n");
    // rate 0.5: at y=1 the section is 1.5x, at y=-1 it is 0.5x.
    const makina::Scene s = makina::parseScene(sceneWith(boxUnder("Taper", "ratePerUnit", 0.5, "Y")));
    check(s.nodes.count == 3, "three nodes expected");
    // Sampled just inside the end faces; on y=+-1 itself the field is exactly 0.
    check(at(s, 0.4, 0.98, 0) < 0, "near y=1 the widened box should reach x=0.4");
    check(at(s, 0.4, -0.98, 0) > 0, "near y=-1 the narrowed box should not reach x=0.4");
    check(at(s, 0.1, -0.98, 0) < 0, "near y=-1 the narrowed box still holds x=0.1");

    const double g = worstGradient(s, 1.4);
    std::printf("    worst gradient %.4f\n", g);
    check(g <= 1.0 + 1e-3, "tapered field gradient exceeds 1: the march could step through");
}

/// The flattened program must read the same field as the tree evaluator through the warp: the
/// tree applies the inverse map on the way down, the program carries it in a side table and
/// applies it in front of each leaf. Two routes, one number, on a lattice around each warp.
void programAgrees() {
    std::printf("the flattened program agrees with the tree through every warp\n");
    const char* const cases[3][3] = {{"Twist", "degreesPerUnit", "Y"},
                                     {"Bend", "degreesPerUnit", "Y"},
                                     {"Taper", "ratePerUnit", "Y"}};
    const double rates[3] = {90.0, 60.0, 0.5};
    for (int c = 0; c < 3; ++c) {
        // Wrapped in a Translate so the warp's world->warp matrix is not the identity, and
        // with a boolean below so both leaf kinds and the correction ride the chain.
        const std::string inner = boxUnder(cases[c][0], cases[c][1], rates[c], cases[c][2]);
        const makina::Scene s = makina::parseScene(
            "{\"format\":\"makina-scene\",\"version\":1,\"nextId\":9,\"materials\":[],"
            "\"root\":{\"id\":1,\"op\":\"SceneRoot\",\"children\":[{\"id\":8,\"op\":"
            "\"Translate\",\"x\":0.2,\"y\":0.1,\"z\":-0.3,\"children\":[" + inner + "]}]}}");
        const makina::EvalProgram prog = makina::flatten(s);
        check(!prog.warps.empty(), std::string(cases[c][0]) + ": the program should carry a warp");
        double worst = 0.0;
        constexpr int kSteps = 9;
        for (int i = 0; i < kSteps; ++i) {
            for (int j = 0; j < kSteps; ++j) {
                for (int k = 0; k < kSteps; ++k) {
                    const double p[3] = {-1.5 + 3.0 * i / (kSteps - 1), -1.5 + 3.0 * j / (kSteps - 1),
                                         -1.5 + 3.0 * k / (kSteps - 1)};
                    const double a = makina::eval(s, p);
                    const double b = makina::evalProgram(prog, p);
                    const double d = std::fabs(a - b);
                    if (d > worst) {
                        worst = d;
                    }
                }
            }
        }
        std::printf("    %s: worst tree/program difference %.2e\n", cases[c][0], worst);
        check(worst < 1e-5, std::string(cases[c][0]) + ": the program disagrees with the tree");
    }
}

/// The forward map undoes the inverse everywhere the mesh writer will use it, frozen ends
/// included -- a vertex sent out and read back has to land where it started.
void forwardUndoesInverse() {
    std::printf("the forward map undoes the inverse, frozen ends included\n");
    const int kinds[3] = {MK_WARP_TWIST, MK_WARP_BEND, MK_WARP_TAPER};
    const double rates[3] = {1.2, 0.9, 0.5};
    double worst = 0.0;
    for (int k = 0; k < 3; ++k) {
        for (int axis = 0; axis < 3; ++axis) {
            for (int i = 0; i < 27; ++i) {
                // Inside the reach and past it, so the frozen band is exercised.
                const double p[3] = {-1.6 + 1.6 * (i % 3), -1.6 + 1.6 * ((i / 3) % 3),
                                     -1.6 + 1.6 * (i / 9)};
                double q[3], back[3];
                makina::mkWarpInv(kinds[k], axis, rates[k], 1.0, p[0], p[1], p[2], q[0], q[1], q[2]);
                makina::mkWarpFwd(kinds[k], axis, rates[k], 1.0, q[0], q[1], q[2], back[0], back[1],
                                  back[2]);
                for (int c = 0; c < 3; ++c) {
                    const double d = std::fabs(back[c] - p[c]);
                    if (d > worst) {
                        worst = d;
                    }
                }
            }
        }
    }
    std::printf("    worst fwd(inv(p)) - p: %.2e\n", worst);
    check(worst < 1e-9, "the forward map does not undo the inverse");
}

void roundTrip() {
    std::printf("json round trip keeps the axis and the rate\n");
    const makina::Scene s = makina::parseScene(sceneWith(boxUnder("Twist", "degreesPerUnit", 45, "Z")));
    const makina::Scene back = makina::parseScene(makina::writeScene(s));
    check(back.nodes.count == 3, "three nodes after the round trip");
    check(static_cast<makina::Op>(back.nodes[1].op) == makina::Op::Twist, "op survives");
    check((back.nodes[1].flags & makina::flags::kAxisMask) == makina::flags::kAxisZ, "axis survives");
    check(back.nodes[1].params[0] == 45.0f, "rate survives");
}

}  // namespace

int main() {
    std::printf("makina-core warp check\n\n");
    twist();
    bend();
    taper();
    programAgrees();
    forwardUndoesInverse();
    roundTrip();
    std::printf("\n%d checks", checks);
    if (failures > 0) {
        std::printf(", %d FAILED\n", failures);
        return 1;
    }
    std::printf(", all passed\n");
    return 0;
}
