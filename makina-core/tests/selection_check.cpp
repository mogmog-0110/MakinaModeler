// The selection, and the one rule worth testing on it.
//
// Toggling and clearing are small enough to read, so most of this is about `topLevel`: an edit
// must not reach the same solid twice because two of its ancestors were selected. That is the
// difference between moving a bracket and moving it twice as far, and nothing about the picture
// says which happened.

#include <makina/Selection.hpp>
#include <makina/SceneJson.hpp>

#include <cstdio>
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

int main() {
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

    if (failures == 0) {
        std::printf("\nan edit reaches each selected solid exactly once (%d checks)\n", checks);
        return 0;
    }
    std::printf("\n%d of %d checks FAILED\n", failures, checks);
    return 1;
}
