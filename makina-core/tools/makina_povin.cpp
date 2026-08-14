// Reads a POV-Ray file into a Makina scene.
//
//   makina_povin <scene.pov> [-o <out.makina.json>] [--survey]
//
// --survey walks the whole file and prints a report card instead of importing: every construct
// it recognises, with a count, the first line, and whether this project can hold it. The
// importer stops at the first refusal, which answers "why did this fail" but not "what would it
// take" -- the survey answers the second question for a file picked up off the internet.
//
// Prints what it could not take before anything else. A file that reads with omissions is not a
// failure -- most POV scenes use something outside this subset -- but the omissions are the first
// thing whoever runs this needs to see, because they are the difference between the file and the
// scene it produced.

#include <makina/PovImport.hpp>
#include <makina/PovSurvey.hpp>
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
    bool survey = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) {
            outPath = argv[++i];
        } else if (a == "--survey") {
            survey = true;
        } else {
            source = a;
        }
    }
    if (source.empty()) {
        std::fprintf(stderr,
                     "usage: makina_povin <scene.pov> [-o <out.makina.json>] [--survey]\n");
        return 2;
    }

    try {
        if (survey) {
            const makina::PovSurveyResult r = makina::povSurvey(readFile(source));
            std::printf("%s\n", source.c_str());
            const char* labels[] = {"supported", "ignored (does not move a surface)",
                                    "UNSUPPORTED shape", "UNSUPPORTED language",
                                    "UNSUPPORTED (changes the picture)"};
            for (int group = 0; group < 5; ++group) {
                bool headed = false;
                for (const makina::PovSurveyItem& item : r.items) {
                    if (static_cast<int>(item.status) != group) {
                        continue;
                    }
                    if (!headed) {
                        std::printf("  %s:\n", labels[group]);
                        headed = true;
                    }
                    std::printf("    %-16s x%-3d first at line %-5d %s\n", item.name.c_str(),
                                item.count, item.firstLine, item.note.c_str());
                }
            }
            std::printf(r.clean ? "  VERDICT: reads whole -- povImport will take all of it\n"
                                : "  VERDICT: not 1:1 yet -- the UNSUPPORTED rows are what is "
                                  "missing\n");
            return r.clean ? 0 : 1;
        }

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
