// Reads a POV-Ray file into a Makina scene.
//
//   makina_povin <scene.pov> [-o <out.makina.json>]
//
// Prints what it could not take before anything else. A file that reads with omissions is not a
// failure -- most POV scenes use something outside this subset -- but the omissions are the first
// thing whoever runs this needs to see, because they are the difference between the file and the
// scene it produced.

#include <makina/PovImport.hpp>
#include <makina/SceneJson.hpp>

#include <cstdio>
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

}  // namespace

int main(int argc, char** argv) {
    std::string source;
    std::string outPath;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) {
            outPath = argv[++i];
        } else {
            source = a;
        }
    }
    if (source.empty()) {
        std::fprintf(stderr, "usage: makina_povin <scene.pov> [-o <out.makina.json>]\n");
        return 2;
    }

    try {
        const makina::PovImportResult r = makina::importPov(readFile(source));

        std::printf("%s\n", source.c_str());
        if (r.unsupported.empty()) {
            std::printf("    read whole\n");
        } else {
            std::printf("    %zu construct(s) this reader does not represent:\n",
                        r.unsupported.size());
            for (const std::string& s : r.unsupported) {
                std::printf("        %s\n", s.c_str());
            }
        }
        std::printf("    %u nodes, %u material(s)\n", r.scene.nodes.count,
                    r.scene.materials.count);

        if (!outPath.empty()) {
            std::ofstream out(outPath, std::ios::binary);
            if (!out) {
                throw std::runtime_error("could not write '" + outPath + "'");
            }
            out << makina::writeScene(r.scene);
            std::printf("    wrote %s\n", outPath.c_str());
        }
        return 0;
    } catch (const makina::PovParseError& e) {
        // Refused, with a position. This is the designed outcome for anything outside the subset
        // that would change the model rather than only its look.
        std::fprintf(stderr, "%s: %s\n", source.c_str(), e.what());
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
