// Compares makina-core's measurement commands against Grasp3D's SceneMeasure.
//
// tools/measuredump writes the reference; this replays the same queries through the C++ port.
// Subtrees are addressed by the ordinal of the root's direct child, because the two
// implementations number their nodes differently and an index would not mean the same thing on
// both sides.
//
// Tolerances differ by quantity and the reasons are not the same:
//
//   gap / symmetry   deterministic on both sides, so the only slack needed is for CsgNode storing
//                    parameters as float where Grasp3D uses double
//   sample counts    must match exactly. A differing count means the two took different points,
//                    and then every distance they agree on agrees by luck
//   volume           Monte-Carlo, but with the same generator and the same seed, so it is exact
//                    too. Reproducing java.util.Random is what buys that (Measure.hpp)
//
// Every call passes makina::kGrasp3D -- see Fidelity.hpp for the two points Makina disagrees on.
// Both reach the sample counts: the boolean-aware AABB feeds `filterEps`, which sets how close to
// the surface a point has to be to survive, and the Label reading changes what `eval` reports at
// that point. Under Makina's own answers not one count in the corpus would line up, and the
// comparison would say nothing about the measurement code itself. The divergence is not skipped:
// `checkTighterBounds` asserts Makina's box is contained in the reference's, which is the property
// that makes the tightening safe.

#include <makina/Measure.hpp>
#include <makina/SceneJson.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

/// Carried between the `floating` line and the `float` lines that follow it: the reference emits
/// the list header first and then one line per item, so the port's result has to survive across
/// them.
std::vector<makina::FloatItem> g_floatItems;

constexpr double kRelTolerance = 1e-4;

/// Reproduce Grasp3D's answers, so the comparison isolates the measurement code.
constexpr makina::Fidelity kReference = makina::kGrasp3D;

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("could not open '" + path + "'");
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

double parseNum(const std::string& s) {
    if (s == "inf") return std::numeric_limits<double>::infinity();
    if (s == "-inf") return -std::numeric_limits<double>::infinity();
    if (s == "nan") return std::numeric_limits<double>::quiet_NaN();
    return std::stod(s);
}

/// The reference writes +infinity where the port writes its kEmpty sentinel; both mean "nothing
/// there", so they are compared as a category rather than as numbers.
bool bothEmpty(double expected, double got) {
    return std::isinf(expected) && makina::isEmpty(got);
}

void checkNear(double expected, double got, const std::string& what) {
    ++checks;
    if (bothEmpty(expected, got)) {
        return;
    }
    if (std::isinf(expected) != makina::isEmpty(got)) {
        std::printf("    FAIL  %s: java=%s cpp=%s\n", what.c_str(),
                    std::isinf(expected) ? "empty" : std::to_string(expected).c_str(),
                    makina::isEmpty(got) ? "empty" : std::to_string(got).c_str());
        ++failures;
        return;
    }
    const double scale = std::fabs(expected) > 1.0 ? std::fabs(expected) : 1.0;
    if (std::fabs(expected - got) > kRelTolerance * scale) {
        std::printf("    FAIL  %s: java=%.9g cpp=%.9g (gap %.3g)\n", what.c_str(), expected, got,
                    std::fabs(expected - got));
        ++failures;
    }
}

void checkExact(long expected, long got, const std::string& what) {
    ++checks;
    if (expected != got) {
        std::printf("    FAIL  %s: java=%ld cpp=%ld\n", what.c_str(), expected, got);
        ++failures;
    }
}

/// Makina's box has to shrink, never grow: a bound that no longer encloses the solid would make
/// every gap and overlap below it meaningless. Checked on each direct child, which is what the
/// measurements are addressed by.
void checkTighterBounds(const makina::Scene& scene) {
    const makina::CsgNode& root = scene.nodes[0];
    for (std::uint16_t i = 0; i < root.childCount; ++i) {
        const std::uint16_t c = static_cast<std::uint16_t>(root.firstChild + i);
        const makina::BoundsResult mine = makina::worldBounds(scene, c);
        const makina::BoundsResult ref = makina::worldBounds(scene, c, kReference);
        if (!mine.box.valid || !ref.box.valid) {
            continue;
        }
        for (int a = 0; a < 3; ++a) {
            ++checks;
            const double slack = 1e-6;
            if (mine.box.lo[a] < ref.box.lo[a] - slack ||
                mine.box.hi[a] > ref.box.hi[a] + slack) {
                std::printf("    FAIL  child %u axis %d: the box escapes the reference's\n", i, a);
                ++failures;
            }
        }
    }
}

/// Index of the root's n-th direct child.
std::uint16_t childOf(const makina::Scene& s, int ordinal) {
    return static_cast<std::uint16_t>(s.nodes[0].firstChild + ordinal);
}

void compareOne(const std::string& jsonPath, const std::string& dumpPath) {
    std::printf("%s\n", jsonPath.c_str());

    const makina::Scene scene = makina::parseScene(readFile(jsonPath));

    std::ifstream in(dumpPath);
    if (!in) {
        throw std::runtime_error("could not open '" + dumpPath + "'. Was tools/measuredump run?");
    }

    const int before = failures;
    checkTighterBounds(scene);
    int gaps = 0, overlaps = 0, floats = 0, syms = 0;
    std::string line;

    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream ls(line);
        std::string kind;
        ls >> kind;

        if (kind == "gap") {
            int i, j;
            std::string dist;
            long samples;
            ls >> i >> j >> dist >> samples;
            const makina::GapResult g =
                makina::gap(scene, childOf(scene, i), childOf(scene, j), kReference);
            const std::string tag = "gap " + std::to_string(i) + "," + std::to_string(j);
            checkNear(parseNum(dist), g.distance, tag + " distance");
            checkExact(samples, g.samples, tag + " samples");
            ++gaps;

        } else if (kind == "overlap") {
            int i, j, aabb;
            std::string pen, vol;
            ls >> i >> j >> pen >> vol >> aabb;
            const makina::OverlapResult o =
                makina::overlap(scene, childOf(scene, i), childOf(scene, j), 1e-3, kReference);
            const std::string tag = "overlap " + std::to_string(i) + "," + std::to_string(j);
            checkNear(parseNum(pen), o.maxPenetration, tag + " penetration");
            checkNear(parseNum(vol), o.volume, tag + " volume");
            checkExact(aabb, o.aabbIntersects ? 1 : 0, tag + " aabbIntersects");
            ++overlaps;

        } else if (kind == "floating") {
            std::string ground, tol;
            long count;
            ls >> ground >> tol >> count;
            const std::vector<makina::FloatItem> items =
                makina::floating(scene, 0, parseNum(ground), parseNum(tol), kReference);
            checkExact(count, static_cast<long>(items.size()), "floating item count");
            g_floatItems = items;
            ++floats;

        } else if (kind == "float") {
            int i, supported, sunk;
            std::string minY, gapTo;
            ls >> i >> minY >> supported >> sunk >> gapTo;
            if (static_cast<std::size_t>(i) >= g_floatItems.size()) {
                std::printf("    FAIL  float %d: the port produced fewer items\n", i);
                ++failures;
                continue;
            }
            const makina::FloatItem& it = g_floatItems[i];
            const std::string tag = "float " + std::to_string(i);
            checkNear(parseNum(minY), it.minY, tag + " minY");
            checkExact(supported, it.supported ? 1 : 0, tag + " supported");
            checkExact(sunk, it.sunk ? 1 : 0, tag + " sunk");
            checkNear(parseNum(gapTo), it.gapToNearest, tag + " gapToNearest");

        } else if (kind == "symmetry") {
            int axis;
            std::string plane, maxDev, meanDev;
            long samples, offenders;
            ls >> axis >> plane >> maxDev >> meanDev >> samples >> offenders;
            const makina::SymmetryResult sy =
                makina::symmetry(scene, 0, axis, parseNum(plane), 1e-3, kReference);
            const std::string tag = "symmetry axis" + std::to_string(axis);
            checkNear(parseNum(maxDev), sy.maxDev, tag + " maxDev");
            checkNear(parseNum(meanDev), sy.meanDev, tag + " meanDev");
            checkExact(samples, sy.samples, tag + " samples");
            checkExact(offenders, static_cast<long>(sy.offenders.size()), tag + " offenders");
            ++syms;
        }
    }

    std::printf("    %d gap, %d overlap, %d floating, %d symmetry%s\n", gaps, overlaps, floats,
                syms, failures == before ? "  (all agree)" : "");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || (argc - 1) % 2 != 0) {
        std::fprintf(stderr,
                     "usage: measure_compare <scene.json> <scene.measure.txt> [more pairs ...]\n");
        return 2;
    }

    std::printf("makina-core vs Grasp3D SceneMeasure\n\n");

    for (int i = 1; i + 1 < argc; i += 2) {
        try {
            compareOne(argv[i], argv[i + 1]);
        } catch (const std::exception& e) {
            std::printf("    FAIL  %s\n", e.what());
            ++failures;
        }
    }

    if (failures == 0) {
        std::printf("\nthe measurements agree with the reference (%d checks)\n", checks);
        return 0;
    }
    std::printf("\n%d of %d checks FAILED\n", failures, checks);
    return 1;
}
