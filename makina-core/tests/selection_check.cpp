// The selection, and the one rule worth testing on it.
//
// Toggling and clearing are small enough to read, so most of this is about `topLevel`: an edit
// must not reach the same solid twice because two of its ancestors were selected. That is the
// difference between moving a bracket and moving it twice as far, and nothing about the picture
// says which happened.

#include <makina/Bounds.hpp>
#include <makina/Eval.hpp>
#include <makina/Selection.hpp>
#include <makina/SceneJson.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

int checks = 0;
int failures = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        std::printf("    FAIL  %s\n", what.c_str());
        ++failures;
    }
}

/// A root with a Translate holding a Box, and a Sphere beside it.
///
/// Written out rather than loaded so the ids are known here: the test is about which ids survive
/// the rule, and reading them out of a fixture would make it a test of the fixture.
makina::Scene threeNodes() {
    const char* json = R"({
      "format": "makina-scene", "version": 1, "nextId": 5,
      "root": { "op": "SceneRoot", "id": 1, "name": "Scene", "children": [
        { "op": "Translate", "id": 2, "name": "arm", "x": 1.0, "y": 0.0, "z": 0.0,
          "children": [ { "op": "Box", "id": 3, "name": "plate",
                          "x1": -1, "y1": -1, "z1": -1, "x2": 1, "y2": 1, "z2": 1 } ] },
        { "op": "Sphere", "id": 4, "name": "knob", "radius": 0.5 }
      ] }
    })";
    return makina::parseScene(json);
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("makina-core selection\n\n");

    const makina::Scene s = threeNodes();

    // Picking replaces, and picking nothing clears.
    check(makina::selectOnly(3) == makina::Selection{3}, "picking one node selects it");
    check(makina::selectOnly(0).empty(), "picking nothing clears the selection");

    // The modifier toggles, so ten selected things do not have to be started over to drop one.
    makina::Selection sel = makina::selectOnly(3);
    sel = makina::toggleSelected(sel, 4);
    check(sel == (makina::Selection{3, 4}), "the modifier adds to the selection");
    check(makina::isSelected(sel, 4), "the added node reads as selected");
    sel = makina::toggleSelected(sel, 3);
    check(sel == makina::Selection{4}, "the same node again takes it back out");
    check(!makina::isSelected(sel, 3), "the removed node no longer reads as selected");
    check(makina::toggleSelected(sel, 0) == sel, "toggling nothing changes nothing");

    // Order survives, because the header names the last thing picked.
    sel = makina::toggleSelected(makina::selectOnly(4), 3);
    check(sel.back() == 3, "the last pick stays last");

    // Ancestry.
    check(makina::isUnder(s, 3, 2), "the box is under the translate");
    check(!makina::isUnder(s, 2, 3), "the translate is not under the box");
    check(!makina::isUnder(s, 3, 3), "a node is not under itself");
    check(!makina::isUnder(s, 3, 4), "the box is not under the sphere");
    check(makina::isUnder(s, 3, 1), "everything is under the root");

    // The rule. A parent and its child selected together must reach the child once.
    const makina::Selection both{2, 3};
    check(makina::topLevel(s, both) == makina::Selection{2},
          "a node inside another selected node is dropped from the edit");

    const makina::Selection apart{3, 4};
    check(makina::topLevel(s, apart) == (makina::Selection{3, 4}),
          "two nodes that do not contain each other both stay");

    check(makina::topLevel(s, makina::Selection{2, 3, 4}) == (makina::Selection{2, 4}),
          "the rule holds with a third node beside the pair");

    // Order survives the rule too.
    check(makina::topLevel(s, makina::Selection{4, 2}) == (makina::Selection{4, 2}),
          "the rule keeps the order it was given");

    // An id that is no longer in the tree is dropped rather than refused. A selection outlives an
    // undo, and moving something deleted two steps ago should be quiet, not an error naming an id
    // that is not on screen.
    check(makina::topLevel(s, makina::Selection{3, 99}) == makina::Selection{3},
          "an id that has left the tree is dropped");
    check(makina::topLevel(s, makina::Selection{}).empty(), "an empty selection stays empty");

    // ---------------------------------------------------------------- mute
    //
    // Muting is not hiding. There is no leaving a node out of the picture but in the shape when
    // the shape is the picture, so muting the cutter of a difference fills the hole in -- and that
    // is checked here, because it is the behaviour someone expecting "hide" would be surprised by.
    {
        const char* json = R"({
          "format": "makina-scene", "version": 1, "nextId": 5,
          "root": { "op": "SceneRoot", "id": 1, "name": "Scene", "children": [
            { "op": "Difference", "id": 2, "name": "plate", "children": [
              { "op": "Box", "id": 3, "name": "body",
                "x1": -1, "y1": -1, "z1": -1, "x2": 1, "y2": 1, "z2": 1 },
              { "op": "Sphere", "id": 4, "name": "bore", "radius": 0.5 }
            ] } ] }
        })";
        const makina::Scene plain = makina::parseScene(json);
        check(!makina::hasMuted(plain), "a scene with no flag set reads as unmuted");

        const double centre[3] = {0.0, 0.0, 0.0};
        check(makina::eval(plain, centre) > 0.0, "the bore should leave the centre outside");

        // The flag survives a save and a load, and only appears when it is set.
        makina::Scene muted = plain;
        const std::uint16_t bore = makina::indexOfId(muted, 4);
        check(bore != makina::kNoChild, "the bore is in the tree");
        muted.nodes[bore].flags |= makina::flags::kMuted;
        check(makina::hasMuted(muted), "the flag reads back");

        const std::string text = makina::writeScene(muted);
        check(text.find("\"muted\"") != std::string::npos, "muted is written to the file");
        check(makina::writeScene(plain).find("\"muted\"") == std::string::npos,
              "a scene with nothing muted gains no muted key");
        const makina::Scene reloaded = makina::parseScene(text);
        check(makina::hasMuted(reloaded), "the flag survives the round trip");

        // And the solid changes, which is the honest part.
        const makina::Scene visible = makina::withoutMuted(muted);
        check(!makina::hasMuted(visible), "the tree handed on carries no flag");
        check(makina::indexOfId(visible, 4) == makina::kNoChild, "the muted node is gone");
        check(makina::indexOfId(visible, 3) != makina::kNoChild, "its sibling stayed");
        check(makina::eval(visible, centre) < 0.0,
              "muting the cutter must fill the hole -- this is an edit, not a view toggle");

        // Muting the parent takes the children with it.
        makina::Scene wholeThing = plain;
        wholeThing.nodes[makina::indexOfId(wholeThing, 2)].flags |= makina::flags::kMuted;
        const makina::Scene empty = makina::withoutMuted(wholeThing);
        check(empty.nodes.count == 1, "muting a group leaves nothing under the root");
        check(makina::eval(empty, centre) >= makina::kEmpty,
              "an empty scene has no surface anywhere");

        // Nothing muted, nothing changed. A rebuild that quietly reordered the tree would make
        // every check above pass and still break a scene the moment it was saved.
        check(makina::writeScene(makina::withoutMuted(plain)) == makina::writeScene(plain),
              "a scene with nothing muted comes back byte for byte");
    }

    // ---------------------------------------------------------------- the fixtures
    //
    // Every axis of the comparison passes on a muted scene whether or not anything honours the
    // flag: if they all ignore it they all ignore it together and agree perfectly. What they check
    // is that the paths do the same thing, not that the thing is the right one.
    //
    // So the fixture itself is checked here: a scene that carries the flag has to be a scene the
    // flag changes. Without this, clearing "muted" out of the file by accident would leave every
    // check in the project passing and the fixture testing nothing.
    int carryingTheFlag = 0;
    for (int i = 1; i < argc; ++i) {
        std::ifstream in(argv[i], std::ios::binary);
        if (!in) {
            continue;
        }
        std::ostringstream buf;
        buf << in.rdbuf();
        const makina::Scene authored = makina::parseScene(buf.str());
        if (!makina::hasMuted(authored)) {
            continue;
        }
        ++carryingTheFlag;
        const makina::Scene solid = makina::withoutMuted(authored);
        check(solid.nodes.count < authored.nodes.count,
              std::string(argv[i]) + ": muting took nothing out of the tree");

        const makina::Aabb a = makina::worldBounds(authored).box;
        const makina::Aabb b = makina::worldBounds(solid).box;
        bool smaller = false;
        for (int k = 0; k < 3; ++k) {
            if (b.lo[k] > a.lo[k] + 1e-9 || b.hi[k] < a.hi[k] - 1e-9) {
                smaller = true;
            }
        }
        check(smaller, std::string(argv[i]) +
                           ": muting changed nothing the picture could show -- this fixture would "
                           "pass every axis with the flag ignored");
    }

    // And at least one of them has to carry it. Skipping the scenes that do not is right, but on
    // its own it means a fixture that lost its flag would be quietly skipped rather than fail --
    // which is what the first version of this did, and it passed with the flag deleted.
    if (argc > 1) {
        check(carryingTheFlag > 0,
              "no fixture carries a muted node, so nothing here tests the flag at all");
    }

    if (failures == 0) {
        std::printf("\nan edit reaches each selected solid exactly once (%d checks)\n", checks);
        return 0;
    }
    std::printf("\n%d of %d checks FAILED\n", failures, checks);
    return 1;
}
