// Checks the JSON command layer: what it accepts, what it refuses, and that a refusal is free.
//
// The refusals are the interesting half. A command surface an agent drives has to fail loudly --
// a typo that silently does nothing produces an agent that believes it has edited the model and
// then reasons from a picture that never changed. So every "bad" case below asserts both that the
// command failed and that the scene is where it was.

#include <makina/Command.hpp>
#include <makina/Edit.hpp>
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

    // Translating, rotating and scaling: the three the viewport spends its time on, and the ones
    // the command layer had no answer for at all. `move` is a reparent, so before this a script
    // could put a node elsewhere in the tree and not shift it.
    //
    // Checked against the tree rather than against the return value: what matters is that a node
    // with no transform of its own grew one, which is the case a caller cannot do with `set`.
    {
        const std::uint32_t before = history.current().nodes.count;
        r = makina::runCommand(history,
                               json{{"op", "translate"}, {"id", pin}, {"axis", "y"},
                                    {"amount", 2.5}});
        check(r.ok, "translate: " + r.message);
        check(history.current().nodes.count == before + 1,
              "translate: a node with no transform should have grown one");

        // And again on the same node, which now has one: the numbers change and no node appears.
        const std::uint32_t grown = history.current().nodes.count;
        r = makina::runCommand(history,
                               json{{"op", "translate"}, {"id", pin}, {"axis", "y"},
                                    {"amount", 1.0}});
        check(r.ok, "translate again: " + r.message);
        check(history.current().nodes.count == grown,
              "translate: a second move should not grow a second transform");

        r = makina::runCommand(history,
                               json{{"op", "rotate"}, {"id", pin}, {"axis", "z"}, {"amount", 30.0}});
        check(r.ok, "rotate: " + r.message);
        r = makina::runCommand(history,
                               json{{"op", "scale"}, {"id", pin}, {"axis", "x"}, {"amount", 1.5}});
        check(r.ok, "scale: " + r.message);

        // An axis has to be given. Guessing one moves the node somewhere nobody asked for.
        r = makina::runCommand(history, json{{"op", "translate"}, {"id", pin}, {"amount", 1.0}});
        check(!r.ok, "a transform with no axis was accepted");
        r = makina::runCommand(history,
                               json{{"op", "translate"}, {"id", 999999}, {"axis", "x"},
                                    {"amount", 1.0}});
        check(!r.ok, "translating an id that is not there was accepted");
    }

    // Muting from here, because the viewport can and the two have to offer the same edits. An
    // operation that only one of them has is a modeller whose capabilities depend on which way it
    // is driven.
    r = makina::runCommand(history, json{{"op", "mute"}, {"id", pin}});
    check(r.ok, "mute: " + r.message);
    {
        const std::uint16_t muted = makina::indexOfId(history.current(), pin);
        check(muted != makina::kNoChild, "mute: lost the node");
        if (muted != makina::kNoChild) {
            check((history.current().nodes[muted].flags & makina::flags::kMuted) != 0,
                  "mute: the flag is not set");
            // Still in the tree. That is the whole difference between muting and deleting, and it
            // is why muting is undoable by muting again rather than by putting a node back.
            check(makina::withoutMuted(history.current()).nodes.count <
                      history.current().nodes.count,
                  "mute: the node stayed in the solid");
        }
    }
    r = makina::runCommand(history, json{{"op", "mute"}, {"id", pin}, {"muted", false}});
    check(r.ok, "unmute: " + r.message);
    {
        const std::uint16_t back = makina::indexOfId(history.current(), pin);
        check(back != makina::kNoChild && (history.current().nodes[back].flags &
                                           makina::flags::kMuted) == 0,
              "unmute: the flag is still set");
    }
    r = makina::runCommand(history, json{{"op", "mute"}, {"id", 999999}});
    check(!r.ok, "muting an id that is not there was accepted");
    r = makina::runCommand(history, json{{"op", "mute"}, {"id", rootId}});
    check(!r.ok, "muting the root was accepted");

    // Keying (D-15), for the same reason as mute: the viewport's K goes through here. A key
    // without a value takes the node's current one; a parameter the op lacks is refused by name.
    r = makina::runCommand(history, json{{"op", "add"},
                                         {"parent", rootId},
                                         {"node", json{{"op", "Joint"}, {"degree", 30.0}}}});
    check(r.ok, "add joint: " + r.message);
    const std::uint32_t jointId = r.newId;
    r = makina::runCommand(history, json{{"op", "key"}, {"id", jointId}, {"param", "degree"},
                                         {"time", 0.0}});
    check(r.ok, "key without a value: " + r.message);
    r = makina::runCommand(history, json{{"op", "key"}, {"id", jointId}, {"param", "degree"},
                                         {"time", 1.0}, {"value", 90.0}});
    check(r.ok, "key with a value: " + r.message);
    {
        const makina::Scene& s = history.current();
        check(s.tracks.count == 1 && s.tracks[0].keyCount == 2 && s.tracks[0].value[0] == 30.0f &&
                  s.tracks[0].value[1] == 90.0f,
              "key: one track of two keys, the first at the node's own value");
    }
    r = makina::runCommand(history, json{{"op", "key"}, {"id", jointId}, {"param", "radius"},
                                         {"time", 0.0}});
    check(!r.ok, "keying a parameter a Joint lacks was accepted");
    r = makina::runCommand(history, json{{"op", "key"}, {"id", jointId}, {"param", "degree"}});
    check(!r.ok, "keying without a time was accepted");
    r = makina::runCommand(history, json{{"op", "undo"}});
    r = makina::runCommand(history, json{{"op", "undo"}});
    r = makina::runCommand(history, json{{"op", "undo"}});
    check(history.current().tracks.count == 0, "undo did not take the keys back");

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
