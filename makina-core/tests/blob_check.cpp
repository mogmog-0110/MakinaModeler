// Blob field evaluation against closed-form answers.
//
// A blob has no B-rep to compare against, so the oracle here is algebra: for one component the
// density s*(1-(r/R)^2)^2 crosses the threshold t at r* = R*sqrt(1 - sqrt(t/s)), and for the
// symmetric cases below the nearest surface point lies on the sample ray, making the true
// distance |r - r*| exactly. The estimate must keep the sign and never exceed that distance --
// overshoot is the one failure sphere tracing cannot survive.
//
// The comparison against POV's own renders of the same blob lives in the pov_compare axis, not
// here; this file proves the field is the one the formulas describe.

#include <makina/Bounds.hpp>
#include <makina/Eval.hpp>
#include <makina/Flatten.hpp>
#include <makina/SceneJson.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
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

/// Wraps node JSON into the smallest scene the parser accepts.
makina::Scene scene(const std::string& nodes) {
    const std::string text =
        "{\"format\":\"makina-scene\",\"version\":1,"
        "\"root\":{\"id\":1,\"op\":\"SceneRoot\",\"name\":\"Scene\",\"children\":[" +
        nodes + "]}}";
    return makina::parseScene(text);
}

std::string blobSphere(int id, double x, double y, double z, double r, double s) {
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "{\"id\":%d,\"op\":\"BlobSphere\",\"name\":\"B\",\"x\":%g,\"y\":%g,\"z\":%g,"
                  "\"radius\":%g,\"strength\":%g}",
                  id, x, y, z, r, s);
    return buf;
}

std::string blob(const std::string& components, double threshold) {
    char head[128];
    std::snprintf(head, sizeof head,
                  "{\"id\":2,\"op\":\"Blob\",\"name\":\"Blob\",\"threshold\":%g,\"children\":[",
                  threshold);
    return head + components + "]}";
}

double evalAt(const makina::Scene& s, double x, double y, double z) {
    const double p[3] = {x, y, z};
    return makina::eval(s, p);
}

/// One sphere component: the surface sits at r*, the sign flips there, and the estimate stays
/// within the true radial distance everywhere it is sampled.
void singleSphere() {
    std::printf("one component, against the closed form\n");
    const double t = 0.5625;
    const double rStar = std::sqrt(1.0 - std::sqrt(t));
    const makina::Scene s = scene(blob(blobSphere(3, 0, 0, 0, 1, 1), t));

    check(std::fabs(evalAt(s, rStar, 0, 0)) < 1e-9, "the estimate at r* should be zero");
    check(evalAt(s, rStar - 0.1, 0, 0) < 0.0, "inside r* should be negative");
    check(evalAt(s, rStar + 0.1, 0, 0) > 0.0, "outside r* should be positive");

    for (double r = 0.05; r < 1.6; r += 0.05) {
        const double est = evalAt(s, r, 0, 0);
        const double truth = r - rStar;
        check((est < 0.0) == (truth < 0.0) || std::fabs(truth) < 1e-9,
              "sign disagrees with the closed form at r=" + std::to_string(r));
        check(std::fabs(est) <= std::fabs(truth) + 1e-12,
              "estimate overshoots the true distance at r=" + std::to_string(r));
    }
}

/// Sphere tracing must land on r* from outside without ever stepping through the surface.
void sphereTrace() {
    std::printf("marching lands on the surface\n");
    const double t = 0.5625;
    const double rStar = std::sqrt(1.0 - std::sqrt(t));
    const makina::Scene s = scene(blob(blobSphere(3, 0, 0, 0, 1, 1), t));

    double x = 3.0;
    bool crossed = false;
    int i = 0;
    for (; i < 200; ++i) {
        const double d = evalAt(s, x, 0, 0);
        if (d < -1e-7) {
            crossed = true;
            break;
        }
        if (d < 1e-9) {
            break;
        }
        x -= d;
    }
    check(!crossed, "the march stepped through the surface");
    check(std::fabs(x - rStar) < 1e-6,
          "the march stopped at " + std::to_string(x) + ", not at r*");
}

/// Two components each too weak to reach the midpoint alone must still enclose it together --
/// the summing is what makes a blob a blob rather than a union.
void blending() {
    std::printf("densities sum\n");
    const double t = 0.5625;
    const makina::Scene pair = scene(
        blob(blobSphere(3, -0.6, 0, 0, 1, 1) + "," + blobSphere(4, 0.6, 0, 0, 1, 1), t));
    const makina::Scene lone = scene(blob(blobSphere(3, -0.6, 0, 0, 1, 1), t));

    check(evalAt(pair, 0, 0, 0) < 0.0, "the midpoint should be inside the blended pair");
    check(evalAt(lone, 0, 0, 0) > 0.0, "the midpoint should be outside a single component");
}

/// A negative-strength component carves: the origin is inside without it, outside with it.
void negativeStrength() {
    std::printf("negative strength carves\n");
    const double t = 0.5625;
    const makina::Scene dented = scene(
        blob(blobSphere(3, 0, 0, 0, 1, 1) + "," + blobSphere(4, 0, 0, 0, 0.5, -1), t));
    const makina::Scene plain = scene(blob(blobSphere(3, 0, 0, 0, 1, 1), t));

    check(evalAt(plain, 0, 0, 0) < 0.0, "the origin should be inside the plain blob");
    check(evalAt(dented, 0, 0, 0) > 0.0, "the origin should be outside the dented blob");
}

/// A cylinder component's support is a capsule: the surface sits at r* radially from the axis
/// and also at r* beyond an end point.
void cylinderComponent() {
    std::printf("cylinder component, radially and past the cap\n");
    const double t = 0.5625;
    const double rStar = std::sqrt(1.0 - std::sqrt(t));
    const makina::Scene s = scene(blob(
        "{\"id\":3,\"op\":\"BlobCylinder\",\"name\":\"C\",\"x1\":-1,\"y1\":0,\"z1\":0,"
        "\"x2\":1,\"y2\":0,\"z2\":0,\"radius\":1,\"strength\":1}",
        t));

    check(std::fabs(evalAt(s, 0, rStar, 0)) < 1e-9, "radial surface should sit at r*");
    check(std::fabs(evalAt(s, 1.0 + rStar, 0, 0)) < 1e-9, "the cap should sit r* past the end");
    check(evalAt(s, 0, 0, 0) < 0.0, "the axis should be inside");
}

/// A Scale between the blob and a component stretches the field with the space.
void scaledComponent() {
    std::printf("a component under a transform\n");
    const double t = 0.5625;
    const double rStar = std::sqrt(1.0 - std::sqrt(t));
    const makina::Scene s = scene(blob(
        "{\"id\":3,\"op\":\"Scale\",\"name\":\"S\",\"x\":2,\"y\":1,\"z\":1,\"children\":[" +
            blobSphere(4, 0, 0, 0, 1, 1) + "]}",
        t));

    check(std::fabs(evalAt(s, 2.0 * rStar, 0, 0)) < 1e-9,
          "the surface along the stretched axis should sit at 2*r*");
    check(std::fabs(evalAt(s, 0, rStar, 0)) < 1e-9,
          "the unstretched axis should keep its surface at r*");
    check(evalAt(s, 2.0 * rStar + 0.1, 0.1, 0) > 0.0, "just outside should stay positive");
}

/// Bounds must cover the support, and a component outside any blob must contribute nothing.
void boundsAndStrays() {
    std::printf("bounds and stray components\n");
    const makina::Scene moved = scene(
        "{\"id\":9,\"op\":\"Translate\",\"name\":\"T\",\"x\":10,\"y\":0,\"z\":0,"
        "\"children\":[" +
        blob(blobSphere(4, 0, 0, 0, 1, 1), 0.5625) + "]}");
    const makina::Aabb box = makina::worldBounds(moved).box;
    check(box.valid && std::fabs(box.lo[0] - 9.0) < 1e-6 && std::fabs(box.hi[0] - 11.0) < 1e-6,
          "a translated blob's box should cover the support at 10 +- 1");

    const makina::Scene stray = scene(blobSphere(2, 0, 0, 0, 1, 1));
    check(makina::isEmpty(evalAt(stray, 0, 0, 0)),
          "a component with no blob above it should evaluate to empty");
}

/// The flattened program must draw the same blob the tree evaluates.
///
/// Signs must agree everywhere; magnitudes only where the field is live, because the program
/// deliberately drops the distance-to-support sharpening the tree has -- outside every support
/// the two are different conservative bounds on the same surface, not the same number.
void flattenAgreesWithEval() {
    std::printf("the program draws the tree's blob\n");
    const std::string scenes[3] = {
        blob(blobSphere(3, 0.2, -0.1, 0.3, 1, 1) + "," + blobSphere(4, -0.6, 0.2, 0, 0.8, 0.7),
             0.5625),
        blob("{\"id\":3,\"op\":\"Scale\",\"name\":\"S\",\"x\":2,\"y\":1,\"z\":1,\"children\":[" +
                 blobSphere(4, 0, 0, 0, 1, 1) + "]}",
             0.5625),
        blob("{\"id\":3,\"op\":\"BlobCylinder\",\"name\":\"C\",\"x1\":-0.7,\"y1\":-0.4,\"z1\":0,"
             "\"x2\":0.7,\"y2\":0.6,\"z2\":0.2,\"radius\":0.9,\"strength\":1}",
             0.5625),
    };
    for (const std::string& body : scenes) {
        const makina::Scene s = scene(
            "{\"id\":9,\"op\":\"Translate\",\"name\":\"T\",\"x\":0.5,\"y\":0,\"z\":0,"
            "\"children\":[" + body + "]}");
        const makina::EvalProgram prog = makina::flatten(s);
        check(prog.report.skippedUnsupported == 0, "the flatten should take the whole blob");

        int disagree = 0;
        double worstLive = 0.0;
        for (double x = -2.0; x <= 2.6; x += 0.35) {
            for (double y = -2.0; y <= 2.0; y += 0.35) {
                for (double z = -2.0; z <= 2.0; z += 0.35) {
                    const double p[3] = {x, y, z};
                    const double a = makina::eval(s, p);
                    const double b = makina::evalProgram(prog, p);
                    if ((a < 0.0) != (b < 0.0)) {
                        ++disagree;
                    }
                    // Inside the support union the two share the (threshold-field)/L formula
                    // and may differ only by float32 storage of the baked transforms. 0.1 stays
                    // below threshold/L for every scene here, so a value under it can only come
                    // from inside -- outside, both bounds sit at threshold/L or above.
                    if (a < 0.1) {
                        const double d = std::fabs(a - b);
                        if (d > worstLive) {
                            worstLive = d;
                        }
                    }
                }
            }
        }
        check(disagree == 0,
              std::to_string(disagree) + " grid point(s) are inside one evaluator and outside "
                                         "the other");
        check(worstLive < 1e-4,
              "near the field the two evaluators differ by " + std::to_string(worstLive));
    }
}

/// The scene format holds a blob byte for byte, same property roundtrip proves for the rest.
void jsonRoundTrip() {
    std::printf("the format holds a blob\n");
    const makina::Scene a = scene(blob(
        blobSphere(3, 0.1, 0.2, 0.3, 1.5, 0.9) + ","
        "{\"id\":4,\"op\":\"BlobCylinder\",\"name\":\"C\",\"x1\":-1,\"y1\":0,\"z1\":0,"
        "\"x2\":1,\"y2\":0.5,\"z2\":0,\"radius\":0.92,\"strength\":0.9}",
        0.55));
    const makina::Scene b = makina::parseScene(makina::writeScene(a));
    check(std::memcmp(&a, &b, sizeof(makina::Scene)) == 0,
          "write + parse should reproduce the scene byte for byte");
}

}  // namespace

int main() {
    std::printf("makina-core blob field\n\n");
    try {
        singleSphere();
        sphereTrace();
        blending();
        negativeStrength();
        cylinderComponent();
        scaledComponent();
        boundsAndStrays();
        flattenAgreesWithEval();
        jsonRoundTrip();
    } catch (const std::exception& e) {
        std::printf("    FAIL  %s\n", e.what());
        ++failures;
    }

    if (failures == 0) {
        std::printf("\nthe field matches the closed forms (%d checks)\n", checks);
        return 0;
    }
    std::printf("\n%d of %d checks FAILED\n", failures, checks);
    return 1;
}
