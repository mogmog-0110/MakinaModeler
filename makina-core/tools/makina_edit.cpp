// Headless command line for reading, editing and measuring a scene.
//
// This is the surface an agent actually drives today, before the app exists. It is deliberately
// three verbs and no state: an agent reads the tree, sends edits, and then *checks its own work*
// with the measurement commands rather than looking at a picture and guessing.
//
// That last part is the point. An agent that can only see a render has to infer whether the boss
// it just moved now clears the bore; one that can ask for the gap gets a number. Everything here
// is headless, so it runs in a shell with no GPU and no window.
//
//   describe <scene.json>                     the tree, with ids and named parameters
//   apply    <scene.json> <commands.json>     run edits, write the result
//   measure  <scene.json>                     gaps, overlaps, floating parts, symmetry

#include <makina/Bounds.hpp>
#include <makina/Command.hpp>
#include <makina/Measure.hpp>
#include <makina/SceneJson.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("could not write '" + path + "'");
    }
    out << text;
}

nlohmann::json boundsJson(const makina::Scene& s, std::uint16_t index) {
    const makina::BoundsResult b = makina::worldBounds(s, index);
    if (!b.box.valid) {
        return nullptr;
    }
    return nlohmann::json{{"min", {b.box.lo[0], b.box.lo[1], b.box.lo[2]}},
                          {"max", {b.box.hi[0], b.box.hi[1], b.box.hi[2]}}};
}

int describe(const std::string& path) {
    const makina::Scene s = makina::parseScene(readFile(path));

    // The tree comes straight from the scene writer, so what an agent reads here is exactly what a
    // scene file holds -- same keys, same ids. One vocabulary, not two.
    nlohmann::json out = nlohmann::json::parse(makina::writeScene(s));

    int unsupported = 0;
    for (std::uint32_t i = 0; i < s.nodes.count; ++i) {
        if (static_cast<makina::Op>(s.nodes[i].op) == makina::Op::Unsupported) {
            ++unsupported;
        }
    }

    out["summary"] = nlohmann::json{{"nodes", s.nodes.count},
                                    {"materials", s.materials.count},
                                    {"unsupported", unsupported},
                                    {"nextId", s.nextId},
                                    {"bounds", boundsJson(s, 0)}};
    std::printf("%s\n", out.dump(2).c_str());
    return 0;
}

int apply(const std::string& scenePath, const std::string& commandPath, const std::string& outPath) {
    const makina::Scene start = makina::parseScene(readFile(scenePath));
    makina::History history(start, 128);

    const nlohmann::json commands = nlohmann::json::parse(readFile(commandPath));
    const std::vector<makina::CommandResult> results = makina::runCommands(history, commands);

    nlohmann::json report = nlohmann::json::array();
    bool allOk = true;
    for (std::size_t i = 0; i < results.size(); ++i) {
        nlohmann::json entry{{"index", i}, {"ok", results[i].ok},
                             {"message", results[i].message}};
        if (results[i].newId != 0) {
            entry["newId"] = results[i].newId;
        }
        report.push_back(entry);
        allOk = allOk && results[i].ok;
    }

    // Written even when a command failed: the scene is then the last good one, and an agent that
    // wants to look at where it got to should not have to re-run the batch to see it.
    if (!outPath.empty()) {
        writeFile(outPath, makina::writeScene(history.current()));
    }

    nlohmann::json out{{"applied", report},
                       {"nodes", history.current().nodes.count},
                       {"steps", history.size() - 1},
                       {"canUndo", history.canUndo()}};
    if (!outPath.empty()) {
        out["wrote"] = outPath;
    }
    std::printf("%s\n", out.dump(2).c_str());
    return allOk ? 0 : 1;
}

/// The measurement commands over the root's direct children, which is the granularity a person --
/// or an agent -- thinks in: "does this part clear that one".
int measure(const std::string& path) {
    const makina::Scene s = makina::parseScene(readFile(path));
    if (s.nodes.count == 0) {
        std::printf("{}\n");
        return 0;
    }

    const makina::CsgNode& root = s.nodes[0];
    std::vector<std::uint16_t> parts;
    for (std::uint16_t i = 0; i < root.childCount; ++i) {
        const std::uint16_t c = static_cast<std::uint16_t>(root.firstChild + i);
        if (static_cast<makina::Op>(s.nodes[c].op) != makina::Op::Label) {
            parts.push_back(c);
        }
    }

    auto describePart = [&s](std::uint16_t index) {
        return nlohmann::json{{"id", s.nodes[index].id},
                              {"name", s.nameOf(s.nodes[index])},
                              {"op", makina::opName(static_cast<makina::Op>(s.nodes[index].op))}};
    };

    nlohmann::json pairs = nlohmann::json::array();
    // Quadratic, and capped for that reason: each gap costs thousands of SDF evaluations, so a
    // 30-part scene would be 435 of them and an agent waiting on a shell command.
    constexpr std::size_t kMaxParts = 8;
    const std::size_t n = parts.size() < kMaxParts ? parts.size() : kMaxParts;
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            const makina::GapResult g = makina::gap(s, parts[i], parts[j]);
            const makina::OverlapResult o = makina::overlap(s, parts[i], parts[j], 1e-3);
            nlohmann::json entry{{"a", describePart(parts[i])},
                                 {"b", describePart(parts[j])},
                                 {"overlapping", o.overlapping}};
            if (g.valid && !makina::isEmpty(g.distance)) {
                entry["gap"] = g.distance;
            }
            if (o.maxPenetration > 0.0) {
                entry["penetration"] = o.maxPenetration;
                entry["sharedVolume"] = o.volume;
            }
            pairs.push_back(entry);
        }
    }

    nlohmann::json floatingJson = nlohmann::json::array();
    for (const makina::FloatItem& it : makina::floating(s, 0, 0.0, 1e-3)) {
        nlohmann::json entry = describePart(it.node);
        entry["minY"] = it.minY;
        entry["supported"] = it.supported;
        entry["sunk"] = it.sunk;
        if (!makina::isEmpty(it.gapToNearest)) {
            entry["gapToNearest"] = it.gapToNearest;
        }
        floatingJson.push_back(entry);
    }

    nlohmann::json symmetryJson = nlohmann::json::object();
    static const char* kAxisName[3] = {"x", "y", "z"};
    for (int axis = 0; axis < 3; ++axis) {
        const makina::SymmetryResult sy = makina::symmetry(s, 0, axis, 0.0, 1e-3);
        symmetryJson[kAxisName[axis]] =
            nlohmann::json{{"maxDeviation", sy.maxDev},
                           {"meanDeviation", sy.meanDev},
                           {"samples", sy.samples},
                           {"offenders", sy.offenders.size()}};
    }

    nlohmann::json out{{"parts", parts.size()},
                       {"pairsChecked", pairs.size()},
                       {"pairs", pairs},
                       {"floating", floatingJson},
                       {"symmetry", symmetryJson},
                       {"bounds", boundsJson(s, 0)}};
    if (parts.size() > kMaxParts) {
        out["note"] = "only the first " + std::to_string(kMaxParts) +
                      " parts were paired; the pair count is quadratic";
    }
    std::printf("%s\n", out.dump(2).c_str());
    return 0;
}

void usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  makina_edit describe <scene.json>\n"
                 "  makina_edit apply    <scene.json> <commands.json> [-o out.json]\n"
                 "  makina_edit measure  <scene.json>\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        usage();
        return 2;
    }

    const std::string verb = argv[1];
    try {
        if (verb == "describe") {
            return describe(argv[2]);
        }
        if (verb == "measure") {
            return measure(argv[2]);
        }
        if (verb == "apply") {
            if (argc < 4) {
                usage();
                return 2;
            }
            std::string out;
            for (int i = 4; i < argc; ++i) {
                if (std::string(argv[i]) == "-o" && i + 1 < argc) {
                    out = argv[++i];
                }
            }
            return apply(argv[2], argv[3], out);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    usage();
    return 2;
}
