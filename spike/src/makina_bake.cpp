// Bakes a scene's ray-march shader to DXIL, once, at asset import time.
//
// This is the answer the measurement forced. The modeller generates straight-line code per scene
// and compiles it while the user is still dragging, which costs 150-250 ms and is fine. A game
// cannot do that -- it would have to ship a shader compiler to load a prop -- and the obvious
// alternative, interpreting the program from a buffer, measured 5.6x slower at 25 nodes and 11.4x
// at 75 (SPIKE_PERF.md 9). Fifty-five milliseconds for one prop is not a thing you ship.
//
// So the compile moves to import. The engine reads two .cso files and a manifest, and runs at
// generated-code speed with no compiler present.
//
//   makina_bake <scene.makina.json> [-o <dir>] [--shading <file.hlsl>] [--live]
//
// --live (PLAN.md D-15) bakes a shader specialised to the tree's structure only: the leaf
// numbers are read from the program buffer at t0, so the engine uploads flatten(sampleAt(t))
// each frame and a joint moves without a recompile. The manifest says "live": true and the
// engine refuses to upload a program of another node count to it.
//
// Writes <name>.vs.cso, <name>.ps.cso and <name>.csgbake.json next to each other.

#include "dxc_invoke.hpp"
#include "scene_codegen.hpp"

#include <makina/Edit.hpp>
#include <makina/Flatten.hpp>
#include <makina/SceneJson.hpp>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("could not open '" + path + "'");
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// FNV-1a over the scene text.
///
/// Not a security hash and not trying to be. Its whole job is to let the engine notice that the
/// .cso next to a scene was baked from a different version of it, because the failure that catches
/// is the worst kind: the model loads, draws, and is quietly the wrong shape.
std::string hashOf(const std::string& text) {
    std::uint64_t h = 1469598103934665603ull;
    for (const char c : text) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 1099511628211ull;
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

/// "a/b/hero_flange.makina.json" -> "hero_flange"
std::string stemOf(const std::filesystem::path& p) {
    std::string name = p.filename().string();
    const std::size_t dot = name.find('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

}  // namespace

int main(int argc, char** argv) {
    std::string scenePath;
    std::string outDir;
    std::string shading = "scene_shading.hlsl";
    bool live = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) {
            outDir = argv[++i];
        } else if (a == "--shading" && i + 1 < argc) {
            shading = argv[++i];
        } else if (a == "--live") {
            live = true;
        } else {
            scenePath = a;
        }
    }

    if (scenePath.empty()) {
        std::fprintf(stderr,
                     "usage: makina_bake <scene.makina.json> [-o <dir>] [--shading <file.hlsl>] "
                     "[--live]\n");
        return 2;
    }

    try {
        const std::filesystem::path source(scenePath);
        const std::string text = readFile(scenePath);
        // The solid, not the tree that was authored: a muted subtree is not part of the
        // shape, and baking one in would put it in the shader and nowhere else.
        const makina::Scene scene = makina::withoutMuted(makina::parseScene(text));
        const makina::EvalProgram prog = makina::flatten(scene);

        if (prog.nodes.empty()) {
            // Refused rather than baked. An empty program compiles to a shader that draws nothing,
            // and shipping that is how a missing asset becomes an invisible one.
            std::fprintf(stderr, "error: '%s' has nothing renderable in it\n", scenePath.c_str());
            return 1;
        }

        const std::filesystem::path dir =
            outDir.empty() ? source.parent_path() : std::filesystem::path(outDir);
        std::filesystem::create_directories(dir);

        const std::string stem = stemOf(source);
        const std::string hlslPath = (dir / (stem + ".gen.hlsl")).string();
        const std::string vsPath = (dir / (stem + ".vs.cso")).string();
        const std::string psPath = (dir / (stem + ".ps.cso")).string();

        {
            std::ofstream out(hlslPath, std::ios::binary);
            if (!out) {
                throw std::runtime_error("could not write '" + hlslPath + "'");
            }
            out << spike::generateShader(prog, shading, false, live);
        }

        const spike::DxcPaths paths{DXC_PATH, SPIKE_SHADER_DIR, MAKINA_CORE_INCLUDE};
        const double vsMs = spike::compileHlsl(paths, hlslPath, "vs_6_0", "VSMain", vsPath);
        const double psMs = spike::compileHlsl(paths, hlslPath, "ps_6_0", "PSMain", psPath);

        // The compiler log is only interesting when the compile failed, and on success it is an
        // empty file sitting next to a shipped asset.
        std::error_code ignored;
        std::filesystem::remove(vsPath + ".log", ignored);
        std::filesystem::remove(psPath + ".log", ignored);

        const nlohmann::json manifest{
            {"format", "makina-bake"},
            {"version", 1},
            {"scene", source.filename().string()},
            // The engine compares this against the scene it actually loaded.
            {"sceneHash", hashOf(text)},
            {"shading", shading},
            {"vs", std::filesystem::path(vsPath).filename().string()},
            {"ps", std::filesystem::path(psPath).filename().string()},
            {"programNodes", prog.nodes.size()},
            {"maxStackDepth", prog.maxStackDepth},
            // Whether the shader reads its numbers from the buffer (D-15). Absent in older
            // manifests, which the engine reads as false: those shaders ignore any upload.
            {"live", live}};

        const std::string manifestPath = (dir / (stem + ".csgbake.json")).string();
        std::ofstream out(manifestPath, std::ios::binary);
        out << manifest.dump(2) << "\n";

        std::printf("%s\n", stem.c_str());
        std::printf("    %u authoring nodes -> %zu program nodes, stack %d\n", scene.nodes.count,
                    prog.nodes.size(), prog.maxStackDepth);
        if (prog.report.skippedFaces > 0) {
            std::printf("    %d zero-thickness face(s) skipped\n", prog.report.skippedFaces);
        }
        if (prog.report.skippedUnsupported > 0) {
            std::printf("    %d unsupported op(s) skipped\n", prog.report.skippedUnsupported);
        }
        std::printf("    vs %.0f ms, ps %.0f ms -> %s\n", vsMs, psMs, manifestPath.c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
