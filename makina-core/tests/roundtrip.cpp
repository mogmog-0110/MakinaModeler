// Round-trip test for the scene format (Task #5).
//
// The check is JSON -> Scene -> JSON -> Scene, comparing the two Scene values with memcmp rather
// than comparing JSON text. Text comparison would fail on key order and float formatting without
// telling us anything; a byte comparison of the flat POD is both stricter about what we care
// about and a direct demonstration that Scene really is memcpy-able, which is the property the
// engine's rewind depends on.

#include <makina/SceneJson.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("  FAIL  %s\n", what.c_str());
        ++failures;
    }
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("could not open '" + path + "'. Was tools/gsf2json run?");
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Counts nodes by walking children, so a mismatch against nodeCount means the flattening lost
// or duplicated a subtree.
std::uint32_t walkCount(const makina::Scene& s, std::uint16_t index) {
    const makina::CsgNode& n = s.nodes[index];
    std::uint32_t total = 1;
    for (std::uint16_t i = 0; i < n.childCount; ++i) {
        total += walkCount(s, static_cast<std::uint16_t>(n.firstChild + i));
    }
    return total;
}

void testFile(const std::string& path) {
    std::printf("%s\n", path.c_str());

    const std::string original = readFile(path);

    const makina::Scene a = makina::parseScene(original);
    const std::string emitted = makina::writeScene(a);
    const makina::Scene b = makina::parseScene(emitted);

    check(std::memcmp(&a, &b, sizeof(makina::Scene)) == 0,
          "Scene differs after a JSON round trip");
    check(a.nodes.count == walkCount(a, 0),
          "nodeCount (" + std::to_string(a.nodes.count) + ") disagrees with the tree walk (" +
              std::to_string(walkCount(a, 0)) + ")");
    check(a.nodes.count > 0, "scene parsed to zero nodes");

    // Every node must be reachable and every id distinct; both are assumed by the id contract.
    std::vector<bool> seen(a.nodes.count, false);
    for (std::uint32_t i = 0; i < a.nodes.count; ++i) {
        for (std::uint32_t k = i + 1; k < a.nodes.count; ++k) {
            if (a.nodes[i].id == a.nodes[k].id) {
                check(false, "duplicate node id " + std::to_string(a.nodes[i].id));
            }
        }
        if (a.nodes[i].childCount > 0) {
            for (std::uint16_t c = 0; c < a.nodes[i].childCount; ++c) {
                const std::uint16_t child = static_cast<std::uint16_t>(a.nodes[i].firstChild + c);
                check(child < a.nodes.count, "child index out of range");
                if (child < a.nodes.count) {
                    check(!seen[child], "node " + std::to_string(child) + " has two parents");
                    seen[child] = true;
                }
            }
        }
    }

    std::printf("  nodes=%u materials=%u nextId=%u  sizeof(Scene)=%zu bytes (%.1f KB)\n",
                a.nodes.count, a.materials.count, a.nextId, sizeof(makina::Scene),
                sizeof(makina::Scene) / 1024.0);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: roundtrip <scene.json> [more.json ...]\n");
        return 2;
    }

    std::printf("makina-core round-trip test\n");
    std::printf("sizeof(CsgNode)=%zu  sizeof(Material)=%zu  sizeof(Scene)=%zu (%.1f KB)\n\n",
                sizeof(makina::CsgNode), sizeof(makina::Material), sizeof(makina::Scene),
                sizeof(makina::Scene) / 1024.0);

    // A ten second rewind ring at 60 fps is the budget the plan set at 32 MB.
    const double ringMb = sizeof(makina::Scene) * 600.0 / (1024.0 * 1024.0);
    std::printf("rewind ring, 10 s @60fps: %.1f MB (budget 32 MB)\n\n", ringMb);
    if (ringMb > 32.0) {
        std::printf("  FAIL  rewind ring exceeds the 32 MB budget\n");
        ++failures;
    }

    for (int i = 1; i < argc; ++i) {
        try {
            testFile(argv[i]);
        } catch (const std::exception& e) {
            std::printf("  FAIL  %s\n", e.what());
            ++failures;
        }
    }

    if (failures == 0) {
        std::printf("\nall checks passed\n");
        return 0;
    }
    std::printf("\n%d check(s) FAILED\n", failures);
    return 1;
}
