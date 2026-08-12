// Checks the JSON command layer: what it accepts, what it refuses, and that a refusal is free.
//
// The refusals are the interesting half. A command surface an agent drives has to fail loudly --
// a typo that silently does nothing produces an agent that believes it has edited the model and
// then reasons from a picture that never changed. So every "bad" case below asserts both that the
// command failed and that the scene is where it was.

#include <makina/Command.hpp>
#include <makina/SceneJson.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

int failures = 0;
int checks = 0;

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

void exercise(const std::string& path) {
    using nlohmann::json;
    std::printf("%s\n", path.c_str());

    const int checksBefore = checks;
    const makina::Scene original = makina::parseScene(readFile(path));
    makina::History history(original, 16);
    const std::uint32_t rootId = original.nodes[0].id;
    const std::uint32_t before = original.nodes.count;

    // Parameters by name, flat beside "op" -- the form an agent writes without thinking.
    makina::CommandResult r = makina::runCommand(
        history, json{{"op", "add"},
                      {"parent", rootId},
                      {"node", json{{"op", "Cylinder"},
                                    {"capPoint", 1.0},
                                    {"basePoint", -1.0},
                                    {"radius", 0.4},
                                    {"name", "pin"}}}});
    check(r.ok, "add by name: " + r.message);
    const std::uint32_t pin = r.newId;
    check(history.current().nodes.count == before + 1, "add: the node is not there");

    // The same parameters nested, which reads better in a hand-written file.
    r = makina::runCommand(history, json{{"op", "add"},
                                         {"parent", rootId},
                                         {"node", json{{"op", "Sphere"},
                                                       {"params", json{{"radius", 0.6}}}}}});
    check(r.ok, "add with nested params: " + r.message);

    // A misspelt parameter has to be refused, and the message has to name the alternatives.
    const std::uint32_t nodesNow = history.current().nodes.count;
    r = makina::runCommand(history, json{{"op", "set"}, {"id", pin}, {"radius2", 0.9}});
    check(!r.ok, "a misspelt parameter was accepted");
    check(r.message.find("capPoint") != std::string::npos,
          "the error does not list the parameters this op takes: " + r.message);
    check(history.current().nodes.count == nodesNow, "a refused set changed the scene");

    r = makina::runCommand(history, json{{"op", "add"},
                                         {"parent", rootId},
                                         {"node", json{{"op", "Dodecahedron"}}}});
    check(!r.ok, "an unknown op was accepted");
    check(history.current().nodes.count == nodesNow, "a refused add changed the scene");

    r = makina::runCommand(history, json{{"op", "wiggle"}});
    check(!r.ok, "an unknown command was accepted");
    check(r.message.find("rename") != std::string::npos,
          "the error does not list the commands: " + r.message);

    // set by name really does reach the right slot.
    r = makina::runCommand(history, json{{"op", "set"}, {"id", pin}, {"radius", 0.75}});
    check(r.ok, "set by name: " + r.message);
    const std::uint16_t at = makina::indexOfId(history.current(), pin);
    check(at != makina::kNoChild, "set: lost the node");
    if (at != makina::kNoChild) {
        // Cylinder's keys are capPoint, basePoint, radius -- so radius is params[2].
        check(std::fabs(history.current().nodes[at].params[2] - 0.75f) < 1e-6f,
              "set by name wrote the wrong slot");
    }

    // duplicate hands back the id of the copy, and it has to be a new one. Returning the
    // original's id would make the very next command edit the wrong node, and the caller has no
    // way to tell -- both are Cylinders with the same parameters.
    const std::uint32_t beforeCopy = history.current().nodes.count;
    r = makina::runCommand(history, json{{"op", "duplicate"}, {"id", pin}});
    check(r.ok, "duplicate: " + r.message);
    check(r.newId != pin, "duplicate handed back the original's id");
    check(history.current().nodes.count == beforeCopy + 1, "duplicate: the copy is not there");
    check(makina::indexOfId(history.current(), r.newId) != makina::kNoChild,
          "duplicate: the id it returned is not in the tree");
    r = makina::runCommand(history, json{{"op", "duplicate"}, {"id", rootId}});
    check(!r.ok, "duplicating the root was accepted");
    history.undo();

    // A batch stops at the first failure rather than applying the rest.
    const std::uint32_t beforeBatch = history.current().nodes.count;
    const auto batch = makina::runCommands(
        history, json::array({json{{"op", "add"},
                                   {"parent", rootId},
                                   {"node", json{{"op", "Sphere"}, {"radius", 0.2}}}},
                              json{{"op", "remove"}, {"id", 999999}},
                              json{{"op", "add"},
                                   {"parent", rootId},
                                   {"node", json{{"op", "Sphere"}, {"radius", 0.3}}}}}));
    check(batch.size() == 2, "the batch did not stop at the failure");
    check(history.current().nodes.count == beforeBatch + 1,
          "the command after the failure ran anyway");

    // Undo all the way back and the scene has to be the one that was loaded.
    while (history.canUndo()) {
        history.undo();
    }
    check(history.current().nodes.count == original.nodes.count,
          "undoing everything did not get back to the start");

    std::printf("    %d commands accepted and refused as expected\n", checks);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: command_check <scene.json> ...\n");
        return 2;
    }

    std::printf("makina-core JSON command layer\n\n");

    for (int i = 1; i < argc; ++i) {
        try {
            exercise(argv[i]);
        } catch (const std::exception& e) {
            std::printf("    FAIL  %s\n", e.what());
            ++failures;
        }
    }

    if (failures == 0) {
        std::printf("\nthe command layer behaves (%d checks)\n", checks);
        return 0;
    }
    std::printf("\n%d of %d checks FAILED\n", failures, checks);
    return 1;
}
