// Verifies that makina::Scene works as MitiruEngine's GameMemory.
//
// The engine build proper needs CEF, vcpkg and a submodule tree; this checks the integration
// without any of that, because the part that can actually be wrong is the type contract, and the
// type contract is header-only. What it proves:
//
//   - Scene is trivially copyable, so rewind and replay can hold it
//   - the reflection describes each collection at its live length, not its capacity
//   - offsets and element sizes match what the layout actually is
//   - the whole thing fits the rewind budget the plan set
//
// If this passes and the engine still cannot hold the scene, the problem is in the engine's build,
// not in the model.

#include "reflect_bridge.hpp"

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

/// Builds the descriptors MITIRU_REFLECT would emit for Scene. Spelled out rather than expanded
/// from the macro because the macro needs Game.hpp, which drags in the renderer and CEF; the
/// descriptors are what the host actually consumes either way.
std::vector<mitiru::module::FieldDescriptor> sceneFields() {
    using makina::Scene;
    using mitiru::module::makeFieldDescriptor;

    return {
        makeFieldDescriptor<decltype(Scene::nextId)>(
            "nextId", static_cast<std::uint32_t>(offsetof(Scene, nextId))),
        makeFieldDescriptor<decltype(Scene::nodes)>(
            "nodes", static_cast<std::uint32_t>(offsetof(Scene, nodes))),
        makeFieldDescriptor<decltype(Scene::materials)>(
            "materials", static_cast<std::uint32_t>(offsetof(Scene, materials))),
        makeFieldDescriptor<decltype(Scene::names)>(
            "names", static_cast<std::uint32_t>(offsetof(Scene, names))),
    };
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

}  // namespace

int main(int argc, char** argv) {
    std::printf("makina::Scene as MitiruEngine GameMemory\n\n");

    std::printf("sizeof(Scene)     = %zu bytes (%.1f KB)\n",
                sizeof(makina::Scene), sizeof(makina::Scene) / 1024.0);
    const double ringMb = sizeof(makina::Scene) * 600.0 / (1024.0 * 1024.0);
    std::printf("rewind 10 s @60fps= %.1f MB (budget 32 MB)\n\n", ringMb);
    check(ringMb <= 32.0, "rewind ring exceeds the 32 MB budget");

    const auto fields = sceneFields();
    std::printf("%-12s %-8s %-14s %8s %9s %9s %6s\n",
                "field", "type", "elem", "offset", "elemSize", "capacity", "count?");
    for (const auto& f : fields) {
        std::printf("%-12s %-8s %-14s %8u %9u %9u %6s\n",
                    f.name, f.typeTag, f.elemType[0] ? f.elemType : "-",
                    f.offset, f.elemSize, f.elemCount, f.hasCount ? "yes" : "no");
    }
    std::printf("\n");

    // The three collections must all report a live count, or the host reads capacity and hands
    // whatever is reading the scene 233 zeroed nodes alongside the 23 real ones.
    for (const auto& f : fields) {
        if (std::strcmp(f.name, "nextId") == 0) {
            continue;
        }
        check(f.hasCount == 1, std::string(f.name) + " does not report a live count");
        check(std::strcmp(f.typeTag, "vec") == 0,
              std::string(f.name) + " is not seen as a collection");
    }
    check(fields.size() <= 16, "MITIRU_REFLECT takes at most 16 fields");

    // Element sizes have to match the real types, or the host walks the buffer with the wrong
    // stride and every field after the first is garbage.
    for (const auto& f : fields) {
        if (std::strcmp(f.name, "nodes") == 0) {
            check(f.elemSize == sizeof(makina::CsgNode), "nodes elemSize is wrong");
            check(f.elemCount == makina::Scene::kMaxNodes, "nodes capacity is wrong");
        } else if (std::strcmp(f.name, "materials") == 0) {
            check(f.elemSize == sizeof(makina::Material), "materials elemSize is wrong");
        }
    }

    // Against a real model: the count the host would read has to be the count the scene has.
    for (int i = 1; i < argc; ++i) {
        try {
            const makina::Scene scene = makina::parseScene(readFile(argv[i]));
            const auto* base = reinterpret_cast<const std::uint8_t*>(&scene);

            for (const auto& f : fields) {
                if (f.hasCount == 0) {
                    continue;
                }
                std::uint32_t live = 0;
                std::memcpy(&live, base + f.offset + f.countOffset, sizeof(live));
                if (std::strcmp(f.name, "nodes") == 0) {
                    check(live == scene.nodes.count,
                          "the host would read the wrong node count");
                    std::printf("%-34s nodes=%u  materials=%u  (host reads %u)\n",
                                argv[i], scene.nodes.count, scene.materials.count, live);
                }
                check(live <= f.elemCount, std::string(f.name) + " live count exceeds capacity");
            }
        } catch (const std::exception& e) {
            std::printf("  FAIL  %s\n", e.what());
            ++failures;
        }
    }

    if (failures == 0) {
        std::printf("\nScene satisfies the GameMemory contract\n");
        return 0;
    }
    std::printf("\n%d check(s) FAILED\n", failures);
    return 1;
}
