// What is selected, and the one rule that keeps an edit from happening twice to the same solid.
//
// A selection is a list of node ids rather than one, because every edit the viewport offers reads
// better on several: moving two brackets together, deleting a row of holes, duplicating a pair.
// Ids and not indices, for the reason CSG_NODE.md gives -- an index is a position in an array that
// every edit rewrites, and an id outlives the rewrite.
//
// **The rule is `topLevel`.** Selecting a solid and something inside it and then moving both moves
// the inner one twice: once because it was selected, once because its parent carried it. Deleting
// both refuses on the second, because the first already took it. Duplicating both makes three
// copies of the inner one. All three are the same mistake, so all three are prevented in the same
// place: an edit applies to the selected nodes that no other selected node contains.
//
// Order is kept. The last thing picked is the one whose name the header shows and the one a
// transform pivots on, and a set would lose that.

#pragma once

// Edit.hpp for indexOfId: an id is only a node once something has looked it up, and the lookup
// belongs with the edits that rewrite the array it searches.
#include "Edit.hpp"
#include "Scene.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace makina {

/// The selected node ids, in the order they were picked. Empty means nothing is selected.
using Selection = std::vector<std::uint32_t>;

/// Whether this id is in the selection.
[[nodiscard]] inline bool isSelected(const Selection& s, std::uint32_t id) {
    return std::find(s.begin(), s.end(), id) != s.end();
}

/// Replaces the selection with one node, or clears it when the id is zero.
[[nodiscard]] inline Selection selectOnly(std::uint32_t id) {
    return id == 0 ? Selection{} : Selection{id};
}

/// Adds the id, or removes it when it is already there.
///
/// A toggle rather than an add, which is what both Maya and Blender do with a modifier held: with
/// only an add there is no way to take one thing back out of a selection of ten except to start
/// over.
[[nodiscard]] inline Selection toggleSelected(const Selection& s, std::uint32_t id) {
    if (id == 0) {
        return s;
    }
    Selection out;
    out.reserve(s.size() + 1);
    bool removed = false;
    for (const std::uint32_t existing : s) {
        if (existing == id) {
            removed = true;
            continue;
        }
        out.push_back(existing);
    }
    if (!removed) {
        out.push_back(id);
    }
    return out;
}

/// Whether `id` sits underneath `ancestorId` in the tree. False when they are the same node.
[[nodiscard]] inline bool isUnder(const Scene& s, std::uint32_t id, std::uint32_t ancestorId) {
    std::uint16_t at = indexOfId(s, id);
    if (at == kNoChild || id == ancestorId) {
        return false;
    }
    // Bounded rather than while(true): a malformed parent chain would otherwise hang here, and a
    // hang inside a selection is the hardest kind of failure to place.
    for (std::size_t guard = 0; guard < Scene::kMaxNodes; ++guard) {
        const std::uint16_t parent = s.nodes[at].parent;
        if (parent == kNoParent) {
            return false;
        }
        if (s.nodes[parent].id == ancestorId) {
            return true;
        }
        at = parent;
    }
    return false;
}

/// The selected nodes an edit should act on: those no other selected node contains.
///
/// Ids that are no longer in the tree are dropped too. A selection outlives an undo, and asking to
/// move something that was deleted two steps ago should do nothing rather than refuse and name an
/// id the user cannot see.
[[nodiscard]] inline Selection topLevel(const Scene& s, const Selection& selection) {
    Selection out;
    out.reserve(selection.size());
    for (const std::uint32_t id : selection) {
        if (indexOfId(s, id) == kNoChild) {
            continue;
        }
        bool nested = false;
        for (const std::uint32_t other : selection) {
            if (other != id && isUnder(s, id, other)) {
                nested = true;
                break;
            }
        }
        if (!nested) {
            out.push_back(id);
        }
    }
    return out;
}

}  // namespace makina
