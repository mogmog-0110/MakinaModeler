// Writes a scene out as a mesh.
//
//   makina_mesh <scene.makina.json> -o <out.stl|out.obj>
//
// The boundary representation the BSP boolean already produces, serialized. Not an isosurface
// extraction -- MeshExport.hpp explains the difference and why it matters here.

#include <makina/MeshExport.hpp>
#include <makina/SceneJson.hpp>

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

std::string lowerExtension(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return std::string();
    }
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
}

}  // namespace

int main(int argc, char** argv) {
    std::string scenePath;
    std::string outPath;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) {
            outPath = argv[++i];
        } else {
            scenePath = a;
        }
    }

    if (scenePath.empty() || outPath.empty()) {
        std::fprintf(stderr, "usage: makina_mesh <scene.makina.json> -o <out.stl|out.obj>\n");
        return 2;
    }

    const std::string ext = lowerExtension(outPath);
    if (ext != "stl" && ext != "obj") {
        std::fprintf(stderr, "error: '%s' names neither .stl nor .obj; those are the two formats "
                             "this writes\n", outPath.c_str());
        return 2;
    }

    try {
        const std::filesystem::path source(scenePath);
        const makina::Scene scene = makina::parseScene(readFile(scenePath));
        const makina::TessellationResult mesh = makina::tessellate(scene);

        if (mesh.solids.empty()) {
            // Refused rather than written. An empty STL is a valid file that a printer will
            // happily accept and produce nothing from.
            std::fprintf(stderr, "error: '%s' has no solid form to write\n", scenePath.c_str());
            return 1;
        }
        if (!mesh.complete) {
            // A warning, not a failure: the rest of the model is still worth having, and the
            // caller is told exactly what is not in the file.
            std::fprintf(stderr, "warning: no solid form for '%s'; it is missing from the mesh\n",
                         mesh.missing.c_str());
        }

        const std::string name = source.filename().string();
        const std::size_t triangles = makina::meshTriangles(mesh).size();

        if (ext == "stl") {
            const std::vector<char> bytes = makina::writeStl(mesh, name);
            std::ofstream out(outPath, std::ios::binary);
            if (!out) {
                throw std::runtime_error("could not write '" + outPath + "'");
            }
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        } else {
            std::filesystem::path mtlPath(outPath);
            mtlPath.replace_extension(".mtl");
            const std::string mtlName = mtlPath.filename().string();

            const makina::ObjExport e = makina::writeObj(mesh, scene, name, mtlName);
            std::ofstream out(outPath, std::ios::binary);
            if (!out) {
                throw std::runtime_error("could not write '" + outPath + "'");
            }
            out << e.obj;
            if (!e.mtl.empty()) {
                std::ofstream mtl(mtlPath, std::ios::binary);
                if (!mtl) {
                    throw std::runtime_error("could not write '" + mtlPath.string() + "'");
                }
                mtl << e.mtl;
            }
        }

        std::printf("%s\n", name.c_str());
        std::printf("    %zu solid(s) -> %zu triangles -> %s\n", mesh.solids.size(), triangles,
                    outPath.c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
