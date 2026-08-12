// Compares makina-core's CPU evaluator against Grasp3D's SceneSdf (Phase 1 exit criterion).
//
// tools/sdfdump writes the reference samples; this reads the same coordinates back and evaluates
// them through the C++ port. Regenerating the lattice here instead would put the two languages'
// floating point in the loop, and every mismatch would then be an argument about the last bits
// rather than a finding.
//
// Tolerance exists for one reason: CsgNode stores parameters as float where Grasp3D uses double.
// That is a deliberate choice (it halves GameMemory and the GPU wants float anyway) and it costs
// about seven significant digits, so the comparison is relative rather than exact.

#include <makina/Bounds.hpp>
#include <makina/Eval.hpp>
#include <makina/Flatten.hpp>
#include <makina/SceneJson.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Sample {
    double x, y, z;
    double expected;   // infinity for an empty subtree
};

constexpr double kRelTolerance = 1e-5;

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("could not open '" + path + "'");
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<Sample> readDump(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("could not open '" + path +
                                 "'. Was tools/sdfdump run?");
    }

    std::vector<Sample> out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream ls(line);
        Sample s{};
        std::string dist;
        if (!(ls >> s.x >> s.y >> s.z >> dist)) {
            throw std::runtime_error("malformed sample line: " + line);
        }
        s.expected = (dist == "inf") ? INFINITY : std::stod(dist);
        out.push_back(s);
    }
    return out;
}

bool agrees(double got, double expected) {
    const bool gotEmpty = makina::isEmpty(got);
    const bool expEmpty = std::isinf(expected);
    if (gotEmpty || expEmpty) {
        return gotEmpty == expEmpty;
    }
    const double scale = std::fabs(expected) > 1.0 ? std::fabs(expected) : 1.0;
    return std::fabs(got - expected) <= kRelTolerance * scale;
}

int compareOne(const std::string& jsonPath, const std::string& dumpPath) {
    std::printf("%s\n", jsonPath.c_str());

    const makina::Scene scene = makina::parseScene(readFile(jsonPath));
    const std::vector<Sample> samples = readDump(dumpPath);

    int mismatches = 0;
    double worstAbs = 0.0;
    Sample worst{};
    double worstGot = 0.0;

    for (const Sample& s : samples) {
        const double p[3] = {s.x, s.y, s.z};
        // The reference's own answer: SceneSdf hides a Label's children and Makina does not, so
        // comparing against the dump means asking for the reference reading (Fidelity.hpp).
        const double got = makina::eval(scene, p, makina::kGrasp3D);

        if (!agrees(got, s.expected)) {
            ++mismatches;
            const double diff = std::isinf(s.expected) ? INFINITY : std::fabs(got - s.expected);
            if (mismatches <= 5) {
                std::printf("    mismatch at (%.4f, %.4f, %.4f): java=%s cpp=%s\n",
                            s.x, s.y, s.z,
                            std::isinf(s.expected) ? "inf" : std::to_string(s.expected).c_str(),
                            makina::isEmpty(got) ? "empty" : std::to_string(got).c_str());
            }
            if (!std::isinf(diff) && diff > worstAbs) {
                worstAbs = diff;
                worst = s;
                worstGot = got;
            }
        }
    }

    // The flattened program has to describe the same solid as the tree it came from. Baked
    // transforms, balanced binarisation and canonical primitives are each an opportunity to change
    // the shape by accident, and nothing downstream would notice: the render would simply be of a
    // slightly different model.
    const makina::EvalProgram prog = makina::flatten(scene);
    const makina::FlattenReport& fr = prog.report;

    if (fr.skippedFaces > 0 || fr.skippedUnsupported > 0) {
        std::printf("    flatten: %zu nodes, stack %d; %d face(s) and %d unsupported op(s) "
                    "dropped, so the program is a subset -- agreement not checked\n",
                    prog.nodes.size(), prog.maxStackDepth, fr.skippedFaces, fr.skippedUnsupported);
    } else {
        int progMismatch = 0;
        double worstProg = 0.0;
        for (const Sample& s : samples) {
            const double p[3] = {s.x, s.y, s.z};
            const double tree = makina::eval(scene, p);
            const double flat = makina::evalProgram(prog, p);
            if (makina::isEmpty(tree) != makina::isEmpty(flat)) {
                ++progMismatch;
                continue;
            }
            if (makina::isEmpty(tree)) {
                continue;
            }
            const double scale = std::fabs(tree) > 1.0 ? std::fabs(tree) : 1.0;
            const double gap = std::fabs(tree - flat);
            if (gap > kRelTolerance * scale) {
                ++progMismatch;
                if (gap > worstProg) {
                    worstProg = gap;
                }
            }
        }
        if (progMismatch > 0) {
            std::printf("    FAIL  flattened program disagrees with the tree at %d sample(s), "
                        "worst gap %.9g\n", progMismatch, worstProg);
            mismatches += progMismatch;
        } else {
            std::printf("    flatten: %zu nodes, stack %d, matches the tree everywhere\n",
                        prog.nodes.size(), prog.maxStackDepth);
        }
    }
    if (fr.facesUnderBoolean > 0) {
        std::printf("    note: %d zero-thickness face(s) sit under a boolean, which is not a "
                    "valid CSG operand (PLAN.md D-12)\n", fr.facesUnderBoolean);
    }

    // Bounds are checked here rather than in their own test because this is where the samples
    // are: a point the evaluator says is inside the solid must lie within the box, which is the
    // property the tightened Difference and Intersection handling could plausibly break.
    bool hasPlane = false;
    for (std::uint32_t i = 0; i < scene.nodes.count; ++i) {
        if (static_cast<makina::Op>(scene.nodes[i].op) == makina::Op::Plane) {
            hasPlane = true;
            break;
        }
    }

    const makina::BoundsResult tight = makina::worldBounds(scene);
    const makina::BoundsResult loose = makina::worldBounds(scene, makina::kGrasp3D);

    if (!loose.box.contains(tight.box)) {
        std::printf("    FAIL  tightened bounds are not inside the reference bounds\n");
        ++mismatches;
    }

    if (hasPlane) {
        std::printf("    bounds: scene contains an infinite Plane; containment not checked\n");
    } else if (tight.box.valid) {
        // Parameters are stored as float, so a corner can land a whisker outside a box computed
        // from the same floats. The slack is far below any modelling scale.
        const double slack = 1e-6;
        int outside = 0;
        for (const Sample& s : samples) {
            const double p[3] = {s.x, s.y, s.z};
            if (makina::eval(scene, p) < 0.0 && !tight.box.contains(p, slack)) {
                ++outside;
            }
        }
        if (outside > 0) {
            std::printf("    FAIL  %d interior sample(s) fall outside the tightened bounds\n",
                        outside);
            mismatches += outside;
        } else {
            std::printf("    bounds: %d primitives, tightened box encloses every interior sample\n",
                        tight.primitiveCount);
        }
    }

    if (mismatches == 0) {
        std::printf("    %zu samples, all agree (relative tolerance %.0e)\n",
                    samples.size(), kRelTolerance);
    } else {
        std::printf("    %d of %zu samples disagree; worst absolute gap %.9g at (%.4f, %.4f, %.4f)"
                    " java=%.9g cpp=%.9g\n",
                    mismatches, samples.size(), worstAbs, worst.x, worst.y, worst.z,
                    worst.expected, worstGot);
    }
    return mismatches;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || (argc - 1) % 2 != 0) {
        std::fprintf(stderr, "usage: sdf_compare <scene.json> <scene.sdf.txt> [more pairs ...]\n");
        return 2;
    }

    std::printf("makina-core vs Grasp3D SceneSdf\n\n");

    int totalMismatches = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        try {
            totalMismatches += compareOne(argv[i], argv[i + 1]);
        } catch (const std::exception& e) {
            std::printf("    FAIL  %s\n", e.what());
            ++totalMismatches;
        }
    }

    if (totalMismatches == 0) {
        std::printf("\nthe port agrees with the reference\n");
        return 0;
    }
    std::printf("\n%d disagreement(s)\n", totalMismatches);
    return 1;
}
