// Checks that editing a scene leaves a scene.
//
// The edits rebuild the node array, so the thing that can go wrong is structural: a child block
// that is not contiguous, a parent pointer that disagrees with the child range it should sit in, a
// node reachable from two parents, an id that moved. None of that shows up as a crash -- it shows
// up later as a subtree that renders twice, or a walk that never terminates.
//
// So every check below is an invariant over the whole tree, run after each edit, plus one
// behavioural check that matters more than all of them: **add then remove has to put the distance
// field back exactly**. If it does, the rebuild is not quietly reordering geometry.
//
// One thing it deliberately does not check is byte equality after add-then-remove. That would
// fail, and correctly: nextId has advanced, because an id is never handed out twice.

#include <makina/Bounds.hpp>
#include <makina/Edit.hpp>
#include <makina/Eval.hpp>
#include <makina/History.hpp>
#include <makina/SceneJson.hpp>

#include <cmath>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

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

/// Everything that has to be true of any scene, whoever built it.
void checkStructure(const makina::Scene& s, const std::string& what) {
    if (s.nodes.count == 0) {
        check(false, what + ": empty scene");
        return;
    }

    std::vector<int> parents(s.nodes.count, 0);
    std::set<std::uint32_t> ids;

    check(s.nodes[0].parent == makina::kNoParent, what + ": the root has a parent");

    for (std::uint32_t i = 0; i < s.nodes.count; ++i) {
        const makina::CsgNode& n = s.nodes[i];

        if (n.childCount > 0) {
            check(n.firstChild != makina::kNoChild,
                  what + ": node has children but no firstChild");
            check(static_cast<std::uint32_t>(n.firstChild) + n.childCount <= s.nodes.count,
                  what + ": a child block runs past the end of the array");
            // A parent must precede its children: the flattener and the evaluator both walk
            // forward and would read an unwritten slot otherwise.
            check(n.firstChild > i, what + ": children come before their parent");

            for (std::uint16_t k = 0; k < n.childCount; ++k) {
                const std::uint32_t child = static_cast<std::uint32_t>(n.firstChild) + k;
                if (child >= s.nodes.count) {
                    continue;
                }
                ++parents[child];
                check(s.nodes[child].parent == i,
                      what + ": child " + std::to_string(child) + " disagrees about its parent");
            }
        }

        check(ids.insert(n.id).second, what + ": id " + std::to_string(n.id) + " appears twice");
        check(n.nameId == i, what + ": nameId does not match the slot");
        check(n.id < s.nextId, what + ": id " + std::to_string(n.id) + " is past nextId");
    }

    for (std::uint32_t i = 1; i < s.nodes.count; ++i) {
        check(parents[i] == 1,
              what + ": node " + std::to_string(i) + " has " + std::to_string(parents[i]) +
                  " parents");
    }
}

/// Distance at a spread of points, as a fingerprint of the solid.
std::vector<double> sample(const makina::Scene& s) {
    std::vector<double> out;
    const makina::BoundsResult b = makina::worldBounds(s);
    if (!b.box.valid) {
        return out;
    }
    for (int i = 0; i < 7; ++i) {
        for (int j = 0; j < 7; ++j) {
            for (int k = 0; k < 7; ++k) {
                double p[3];
                const double u[3] = {i / 6.0, j / 6.0, k / 6.0};
                for (int a = 0; a < 3; ++a) {
                    p[a] = b.box.lo[a] + u[a] * (b.box.hi[a] - b.box.lo[a]);
                }
                out.push_back(makina::eval(s, p));
            }
        }
    }
    return out;
}

bool sameField(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (makina::isEmpty(a[i]) != makina::isEmpty(b[i])) {
            return false;
        }
        if (!makina::isEmpty(a[i]) && std::fabs(a[i] - b[i]) > 1e-9) {
            return false;
        }
    }
    return true;
}

void exercise(const std::string& path) {
    std::printf("%s\n", path.c_str());
    const makina::Scene original = makina::parseScene(readFile(path));
    checkStructure(original, "as loaded");

    const std::vector<double> before = sample(original);
    const std::uint32_t rootId = original.nodes[0].id;

    // --- add ------------------------------------------------------------------------------
    makina::CsgNode sphere{};
    sphere.op = static_cast<std::uint8_t>(makina::Op::Sphere);
    sphere.materialId = makina::kNoMaterial;
    sphere.params[0] = 0.37f;

    const makina::EditResult added = makina::addChild(original, rootId, sphere, "probe");
    check(added.ok, "addChild: " + added.why);
    if (!added.ok) {
        return;
    }
    checkStructure(added.scene, "after addChild");
    check(added.scene.nodes.count == original.nodes.count + 1, "addChild: node count");
    check(added.scene.nextId == original.nextId + 1, "addChild: nextId did not advance");
    check(makina::indexOfId(added.scene, added.newId) != makina::kNoChild,
          "addChild: the new id is not findable");

    // Every original id has to survive an insert. This is what lets a caller keep a handle.
    for (std::uint32_t i = 0; i < original.nodes.count; ++i) {
        if (makina::indexOfId(added.scene, original.nodes[i].id) == makina::kNoChild) {
            check(false, "addChild: lost id " + std::to_string(original.nodes[i].id));
            break;
        }
    }

    // --- remove puts it back --------------------------------------------------------------
    const makina::EditResult removed = makina::removeSubtree(added.scene, added.newId);
    check(removed.ok, "removeSubtree: " + removed.why);
    if (removed.ok) {
        checkStructure(removed.scene, "after removeSubtree");
        check(removed.scene.nodes.count == original.nodes.count, "remove: node count");
        check(sameField(before, sample(removed.scene)),
              "add then remove changed the distance field");

        // Not byte-identical, and it must not be: ids are never handed out twice, so nextId has
        // moved on. Everything else has to be back, which is why the tree is compared field by
        // field rather than with memcmp.
        check(removed.scene.nextId == original.nextId + 1,
              "remove: nextId went backwards, so an id could be reused");
        for (std::uint32_t i = 0; i < original.nodes.count; ++i) {
            const std::uint16_t back = makina::indexOfId(removed.scene, original.nodes[i].id);
            if (back == makina::kNoChild) {
                check(false, "remove: lost id " + std::to_string(original.nodes[i].id));
                break;
            }
            const makina::CsgNode& a = original.nodes[i];
            const makina::CsgNode& b = removed.scene.nodes[back];
            if (a.op != b.op || a.childCount != b.childCount || a.materialId != b.materialId) {
                check(false, "remove: node " + std::to_string(i) + " came back different");
                break;
            }
        }
    }

    // --- reorder inside the root leaves the solid alone -------------------------------------
    if (original.nodes[0].childCount >= 2) {
        const std::uint32_t firstChildId = original.nodes[original.nodes[0].firstChild].id;
        const makina::EditResult moved = makina::reparent(original, firstChildId, rootId, 0xFFFFu);
        check(moved.ok, "reparent to the end: " + moved.why);
        if (moved.ok) {
            checkStructure(moved.scene, "after reparent");
            check(moved.scene.nodes.count == original.nodes.count, "reparent: node count");
            // A union does not care about order, and the root is one. Difference would, which is
            // why the check is scoped to a reorder under the root.
            const bool rootIsOrdered =
                static_cast<makina::Op>(original.nodes[0].op) == makina::Op::Difference ||
                static_cast<makina::Op>(original.nodes[0].op) == makina::Op::Intersection;
            if (!rootIsOrdered) {
                check(sameField(before, sample(moved.scene)),
                      "reordering children of an unordered node changed the distance field");
            }
        }
    }

    // --- refusals ---------------------------------------------------------------------------
    check(!makina::removeSubtree(original, rootId).ok, "the root should not be removable");
    check(!makina::addChild(original, 0xFFFFFFFFu, sphere, "x").ok,
          "adding under a missing id should fail");
    if (original.nodes[0].childCount > 0) {
        const std::uint16_t child = original.nodes[0].firstChild;
        if (original.nodes[child].childCount > 0) {
            const std::uint32_t parentId = original.nodes[child].id;
            const std::uint32_t descendantId =
                original.nodes[original.nodes[child].firstChild].id;
            check(!makina::reparent(original, parentId, descendantId).ok,
                  "moving a node under its own descendant should fail");
        }
    }

    // A failed edit must hand back the scene unchanged, so a caller can apply it blindly.
    const makina::EditResult refused = makina::removeSubtree(original, rootId);
    check(refused.scene.nodes.count == original.nodes.count,
          "a refused edit changed the scene anyway");

    // --- try it and see ----------------------------------------------------------------------
    //
    // A snapshot is a copy of the struct, so an undo is exact by construction rather than by
    // getting an inverse right. The byte comparison below is not proving that -- it is checking
    // that History hands the snapshot back untouched, which is the part that could regress if
    // anyone made it clever.
    {
        makina::History history(original, 8);
        const makina::EditResult probe =
            makina::addChild(history.current(), rootId, sphere, "what-if");
        check(probe.ok, "history: the trial edit failed");
        if (probe.ok) {
            history.commit(probe.scene, "add a probe sphere");
            check(history.current().nodes.count == original.nodes.count + 1,
                  "history: the trial edit is not visible");
            check(history.canUndo(), "history: cannot undo a committed edit");
            check(history.undo(), "history: undo refused");
            check(std::memcmp(&history.current(), &original, sizeof(makina::Scene)) == 0,
                  "history: undo did not restore the scene byte for byte");
            check(history.canRedo(), "history: cannot redo after undo");
            check(history.redo(), "history: redo refused");
            check(history.current().nodes.count == original.nodes.count + 1,
                  "history: redo did not come back");

            // Editing after an undo drops the branch that was undone.
            history.undo();
            history.commit(original, "a different edit");
            check(!history.canRedo(), "history: the abandoned branch is still redoable");
        }
    }

    std::printf("    %u nodes, structure and field intact across add / remove / reparent, "
                "revert exact\n", original.nodes.count);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: edit_check <scene.json> ...\n");
        return 2;
    }

    std::printf("makina-core scene editing\n\n");

    for (int i = 1; i < argc; ++i) {
        try {
            exercise(argv[i]);
        } catch (const std::exception& e) {
            std::printf("    FAIL  %s\n", e.what());
            ++failures;
        }
    }

    if (failures == 0) {
        std::printf("\nediting preserves the tree (%d checks)\n", checks);
        return 0;
    }
    std::printf("\n%d of %d checks FAILED\n", failures, checks);
    return 1;
}
