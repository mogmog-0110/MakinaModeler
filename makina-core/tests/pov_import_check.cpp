// Reading POV-Ray back, and the two things that make it worth having.
//
// **Round trip.** A scene exported to POV and read back has to be the same solid. Not the same
// file -- the exporter writes transforms as text and the reader turns them into nodes, so the tree
// differs by construction. What must survive is the geometry, and the way to ask that is to sample
// the distance field of both and compare.
//
// **Refusal.** The other half is that anything not represented stops the read or is named. A
// reader that silently drops what it cannot do produces a scene that loaded, rendered, and is not
// the one in the file, and there is no later check that catches it.
//
// The sample scenes under tests/scenes/pov are not read here. They are hand-written files full of
// macros and radiosity, so what they exercise is the refusal path, and a test whose expected
// result is "refuses for one of eleven possible reasons" asserts nothing. tools/povimport reads
// them and prints what it could not take.

#include <makina/Bounds.hpp>
#include <makina/Eval.hpp>
#include <makina/Pov.hpp>
#include <makina/PovImport.hpp>
#include <makina/SceneJson.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("could not open '" + path + "'");
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// Samples the field on a lattice through the scene's own bounds.
std::vector<double> sample(const makina::Scene& s) {
    const makina::Aabb box = makina::worldBounds(s).box;
    std::vector<double> out;
    if (!box.valid) {
        return out;
    }
    // Padded, so points outside the solid are sampled too: a reader that produced an empty scene
    // would otherwise agree perfectly with one that produced a solid, on no points at all.
    double lo[3], hi[3];
    for (int i = 0; i < 3; ++i) {
        const double pad = (box.hi[i] - box.lo[i]) * 0.25 + 0.1;
        lo[i] = box.lo[i] - pad;
        hi[i] = box.hi[i] + pad;
    }
    constexpr int kSteps = 11;
    for (int i = 0; i < kSteps; ++i) {
        for (int j = 0; j < kSteps; ++j) {
            for (int k = 0; k < kSteps; ++k) {
                const double p[3] = {
                    lo[0] + (hi[0] - lo[0]) * i / (kSteps - 1),
                    lo[1] + (hi[1] - lo[1]) * j / (kSteps - 1),
                    lo[2] + (hi[2] - lo[2]) * k / (kSteps - 1)};
                out.push_back(makina::eval(s, p));
            }
        }
    }
    return out;
}

/// The largest radius any bound reaches, for scaling a tolerance to the model.
double scaleOf(const makina::Scene& s) {
    const makina::Aabb box = makina::worldBounds(s).box;
    if (!box.valid) {
        return 1.0;
    }
    double r = 0.0;
    for (int i = 0; i < 3; ++i) {
        r = std::fmax(r, box.hi[i] - box.lo[i]);
    }
    return r > 1e-9 ? r : 1.0;
}

void roundTrip(const std::string& path) {
    std::printf("%s\n", path.c_str());
    const makina::Scene original = makina::parseScene(readFile(path));

    makina::PovOptions opt;
    opt.silhouette = false;
    const std::string pov = makina::writePov(original, opt);

    makina::PovImportResult back;
    try {
        back = makina::importPov(pov);
    } catch (const makina::PovParseError& e) {
        // A refusal here is a finding, not a pass. The exporter wrote this file, so anything it
        // writes that the reader will not take is a gap between two halves of one format.
        check(false, std::string("the reader refused what the exporter wrote: ") + e.what());
        return;
    }

    const std::vector<double> a = sample(original);
    const std::vector<double> b = sample(back.scene);
    check(!a.empty(), "the original scene has no bounds to sample");
    check(a.size() == b.size(), "the two scenes sample to different numbers of points");
    if (a.empty() || a.size() != b.size()) {
        return;
    }

    // Distances, not just signs. Matching signs would pass a scene scaled by a half.
    const double tolerance = scaleOf(original) * 1e-4;
    double worst = 0.0;
    int wrongSide = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (makina::isEmpty(a[i]) || makina::isEmpty(b[i])) {
            continue;
        }
        worst = std::fmax(worst, std::fabs(a[i] - b[i]));
        if ((a[i] < 0.0) != (b[i] < 0.0)) {
            ++wrongSide;
        }
    }
    check(wrongSide == 0, std::to_string(wrongSide) + " sample point(s) are inside one scene and "
                          "outside the other");
    check(worst <= tolerance,
          "the field differs by " + std::to_string(worst) + ", past the " +
              std::to_string(tolerance) + " that float storage accounts for");

    std::printf("    %u nodes -> %u after the round trip, worst field difference %.2e\n",
                original.nodes.count, back.scene.nodes.count, worst);
}

/// Reads a fragment and expects it to be refused, with the reason mentioning `because`.
void refuses(const std::string& source, const std::string& because) {
    try {
        (void)makina::importPov(source);
        check(false, "this was read without complaint and should not have been: " + because);
    } catch (const makina::PovParseError& e) {
        const std::string msg = e.what();
        check(msg.find(because) != std::string::npos,
              "refused, but the reason does not mention '" + because + "': " + msg);
    }
}

/// Reads a fragment that must succeed while naming something it could not represent.
void notes(const std::string& source, const std::string& what) {
    try {
        const makina::PovImportResult r = makina::importPov(source);
        bool found = false;
        for (const std::string& s : r.unsupported) {
            if (s.find(what) != std::string::npos) {
                found = true;
            }
        }
        check(found, "'" + what + "' was not reported as unread");
    } catch (const makina::PovParseError& e) {
        check(false, std::string("refused instead of reporting: ") + e.what());
    }
}

void boundaries() {
    std::printf("what the reader will not take\n");

    // Geometry this model cannot hold. Each of these would move or reshape the solid if guessed.
    refuses("cone{<0,0,0>,1,<0,2,0>,0.5}", "truncated");
    refuses("cylinder{<1,1,1>,<1,1,1>,0.5}", "distinct end points");
    refuses("plane{<1,0,0>,0}", "+Y");
    refuses("sphere{<0,0,0>,-1}", "positive radius");
    refuses("box{<0,0,0>,<1,1,1> scale <0,1,1>}", "collapses");
    refuses("object{Nope}", "not a declared object");
    refuses("union{sphere{<0,0,0>,1}", "not closed");

    // Recognised, representable geometry, with something on it that is not.
    notes("sphere{<0,0,0>,1 normal{bumps 0.1}}", "normal");
    notes("sphere{<0,0,0>,1 no_shadow}", "no_shadow");
    notes("global_settings{ radiosity{} } sphere{<0,0,0>,1}", "global_settings");

    // A cylinder that is not along an axis, checked against where the solid has to be rather than
    // against the transform the reader built. Stating the expected geometry independently is the
    // point: the reader and a test that recomputed its own rotation would agree with each other
    // and both be wrong.
    try {
        const makina::PovImportResult r =
            makina::importPov("cylinder{<-1,0,-1>,<1,2,1>,0.3}");
        const double from[3] = {-1.0, 0.0, -1.0};
        const double to[3]   = {1.0, 2.0, 1.0};

        int inside = 0;
        for (int i = 1; i < 8; ++i) {
            const double t = i / 8.0;
            const double p[3] = {from[0] + (to[0] - from[0]) * t,
                                 from[1] + (to[1] - from[1]) * t,
                                 from[2] + (to[2] - from[2]) * t};
            if (makina::eval(r.scene, p) < 0.0) {
                ++inside;
            }
        }
        check(inside == 7, std::to_string(inside) + " of 7 points on the axis are inside the "
                           "cylinder; a mis-turned one would miss most of them");

        // Beyond either end, and off to the side: a cylinder that came out longer or fatter than
        // asked would swallow these.
        const double past[3] = {1.6, 2.6, 1.6};
        check(makina::eval(r.scene, past) > 0.0, "a point past the far cap reads as inside");
        // Perpendicular to the axis, well outside the radius. The axis runs along (2,2,2), so
        // (1,-1,0) is at right angles to it.
        const double side[3] = {0.6, -0.6, 0.0};
        check(makina::eval(r.scene, side) > 0.0,
              "a point beside the axis, further out than the radius, reads as inside");
    } catch (const makina::PovParseError& e) {
        check(false, std::string("a slanted cylinder was refused: ") + e.what());
    }

    // The whole point of the intermediate tree: an instance carries a transform of its own, and
    // the wrapper is created after the subtree it wraps.
    try {
        const makina::PovImportResult r = makina::importPov(
            "#declare Ball = sphere{<0,0,0>,1}\n"
            "union{ object{Ball translate <2,0,0>} object{Ball translate <-2,0,0>} }");
        check(r.scene.nodes.count == 6,
              "an instanced sphere under two translates should be 6 nodes, not " +
                  std::to_string(r.scene.nodes.count));
        // Every node's children must be contiguous, which is the invariant building directly into
        // the flat scene could not hold.
        for (std::uint32_t i = 0; i < r.scene.nodes.count; ++i) {
            const makina::CsgNode& n = r.scene.nodes[i];
            for (std::uint16_t c = 0; c < n.childCount; ++c) {
                const std::uint16_t at = static_cast<std::uint16_t>(n.firstChild + c);
                check(r.scene.nodes[at].parent == i,
                      "node " + std::to_string(at) + " does not name its parent");
            }
        }
        const makina::Aabb box = makina::worldBounds(r.scene).box;
        check(box.valid && std::fabs(box.lo[0] + 3.0) < 1e-5 && std::fabs(box.hi[0] - 3.0) < 1e-5,
              "the two instances should span -3..3 in x");
    } catch (const makina::PovParseError& e) {
        check(false, std::string("instancing was refused: ") + e.what());
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("makina-core POV-Ray reader\n\n");

    try {
        boundaries();
    } catch (const std::exception& e) {
        std::printf("    FAIL  %s\n", e.what());
        ++failures;
    }
    std::printf("\n");

    for (int i = 1; i < argc; ++i) {
        try {
            roundTrip(argv[i]);
        } catch (const std::exception& e) {
            std::printf("    FAIL  %s\n", e.what());
            ++failures;
        }
    }

    if (failures == 0) {
        std::printf("\nwhat the exporter writes, the reader takes back (%d checks)\n", checks);
        return 0;
    }
    std::printf("\n%d of %d checks FAILED\n", failures, checks);
    return 1;
}
