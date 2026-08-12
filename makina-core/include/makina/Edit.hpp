// Tree edits, as Scene -> Scene.
//
// Every edit here returns a new Scene rather than patching one. That is not a style choice:
// firstChild/childCount only work while a node's children sit at adjacent indices, so inserting or
// removing anything shifts every index after it. Patching in place would mean fixing up parent,
// firstChild and childCount across the whole array, and getting one of them wrong produces a tree
// where a node has two parents -- which is exactly the bug the loader hit before it switched to
// reserve-then-recurse. Rebuilding cannot get it wrong, because it is the same walk the loader
// does.
//
// Nodes are addressed by **id**, never by index. An index is a fact about the current layout and
// stops being true the moment anything is inserted; an id is handed out once and never reused
// (CSG_NODE.md 6.3). A caller -- a UI, a bridge, an agent -- holds ids.
//
// Cost: a Scene is a flat POD, so a rebuild is a walk over at most kMaxNodes entries and a copy of
// two fixed arrays. For the sizes this modeller targets that is cheaper than the bookkeeping the
// in-place version would need, and it is the same copy the undo history takes anyway.

#pragma once

#include "Op.hpp"
#include "Scene.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace makina {

/// What an edit produced.
///
/// `ok` false leaves `scene` equal to the input: an edit that cannot be carried out changes
/// nothing, so a caller can apply the result unconditionally. `why` says what was wrong.
struct EditResult {
    Scene         scene;
    bool          ok = false;
    std::uint32_t newId = 0;   ///< id of the node an insert created; 0 otherwise
    std::string   why;
};

/// Index of the node with this id, or kNoChild.
///
/// Linear, because a Scene has no id index and building one would be a second thing to keep in
/// step with the tree. At these node counts the scan is not what costs.
inline std::uint16_t indexOfId(const Scene& s, std::uint32_t id) {
    for (std::uint32_t i = 0; i < s.nodes.count; ++i) {
        if (s.nodes[i].id == id) {
            return static_cast<std::uint16_t>(i);
        }
    }
    return kNoChild;
}

/// Whether `maybeAncestor` is at or above `index`.
///
/// Reparenting needs this: moving a node under its own descendant would detach the pair from the
/// tree and leave a cycle that every recursive walk in the codebase would follow forever.
inline bool isAncestorOf(const Scene& s, std::uint16_t maybeAncestor, std::uint16_t index) {
    for (std::uint16_t a = index; a != kNoParent; a = s.nodes[a].parent) {
        if (a == maybeAncestor) {
            return true;
        }
    }
    return false;
}

namespace detail {

inline void copyName(const Scene& from, std::uint16_t src, Scene& to, std::uint16_t dst) {
    if (dst >= Scene::kMaxNodes) {
        return;
    }
    if (to.names.count <= dst) {
        to.names.count = static_cast<std::uint32_t>(dst) + 1;
    }
    const char* text = (src < from.names.count) ? from.names[src].text : "";
    std::memcpy(to.names[dst].text, text, Scene::kNameLen);
    to.names[dst].text[Scene::kNameLen - 1] = '\0';
}

inline void setNameText(Scene& to, std::uint16_t dst, const std::string& name) {
    if (dst >= Scene::kMaxNodes) {
        return;
    }
    if (to.names.count <= dst) {
        to.names.count = static_cast<std::uint32_t>(dst) + 1;
    }
    char* text = to.names[dst].text;
    const std::size_t n = name.size() < Scene::kNameLen - 1 ? name.size() : Scene::kNameLen - 1;
    std::memcpy(text, name.data(), n);
    text[n] = '\0';
}

/// What a rebuild should do differently from a straight copy.
///
/// One struct rather than one rebuild per operation: insert, remove and move are all "copy the
/// tree, but skip this and add that", and writing the contiguous-children walk three times is
/// three chances to get it wrong.
struct RebuildPlan {
    std::uint16_t skip = kNoChild;        ///< source index whose subtree is dropped
    std::uint16_t insertUnder = kNoChild; ///< source index that gains a child
    std::uint16_t insertAt = 0xFFFFu;     ///< position among that node's children; past the end appends
    const CsgNode* insertNode = nullptr;  ///< the node to add, or null when it is a moved subtree
    const Scene*  moveFrom = nullptr;     ///< scene holding the moved subtree
    std::uint16_t moveRoot = kNoChild;    ///< its root in moveFrom
};

bool rebuildInto(const Scene& from, std::uint16_t src, Scene& to, std::uint16_t dst,
                 const RebuildPlan& plan);

/// Copies the subtree at `src` into the reserved slot `dst`, unconditionally.
inline bool copySubtree(const Scene& from, std::uint16_t src, Scene& to, std::uint16_t dst) {
    static const RebuildPlan kNothing;
    return rebuildInto(from, src, to, dst, kNothing);
}

/// The children `src` will have after the plan is applied, in order.
///
/// Returned as source indices, with kNoChild standing for "the inserted node goes here". Working
/// out the list before reserving is what keeps the reservation contiguous.
inline std::vector<std::uint16_t> plannedChildren(const Scene& from, std::uint16_t src,
                                                  const RebuildPlan& plan) {
    std::vector<std::uint16_t> out;
    const CsgNode& n = from.nodes[src];
    const bool inserting = (src == plan.insertUnder);

    int position = 0;
    for (std::uint16_t i = 0; i < n.childCount; ++i) {
        const std::uint16_t child = static_cast<std::uint16_t>(n.firstChild + i);
        if (child == plan.skip) {
            continue;
        }
        if (inserting && position == plan.insertAt) {
            out.push_back(kNoChild);
        }
        out.push_back(child);
        ++position;
    }
    if (inserting && plan.insertAt >= position) {
        out.push_back(kNoChild);
    }
    return out;
}

inline bool rebuildInto(const Scene& from, std::uint16_t src, Scene& to, std::uint16_t dst,
                        const RebuildPlan& plan) {
    // The caller has already written the parent for this slot, and it is the only field that
    // cannot be copied from the source: the source's parent is an index into the *old* layout.
    // Copying the node wholesale would put that stale index back, which is how the first version
    // of this produced a tree where half the nodes named the wrong parent.
    const std::uint16_t parent = to.nodes[dst].parent;
    to.nodes[dst] = from.nodes[src];
    to.nodes[dst].parent = parent;
    copyName(from, src, to, dst);
    to.nodes[dst].nameId = dst;
    to.nodes[dst].firstChild = kNoChild;
    to.nodes[dst].childCount = 0;

    // Heap rather than a stack buffer: this recurses to the depth of the tree, and a buffer sized
    // for the worst case would put kMaxNodes entries on the stack at every level.
    const std::vector<std::uint16_t> kids = plannedChildren(from, src, plan);
    const std::size_t count = kids.size();
    if (count == 0) {
        return true;
    }

    if (to.nodes.count + static_cast<std::uint32_t>(count) > Scene::kMaxNodes) {
        return false;
    }
    const std::uint16_t first = static_cast<std::uint16_t>(to.nodes.count);
    to.nodes.count += static_cast<std::uint32_t>(count);
    to.nodes[dst].firstChild = first;
    to.nodes[dst].childCount = static_cast<std::uint16_t>(count);

    for (std::size_t i = 0; i < count; ++i) {
        const std::uint16_t slot = static_cast<std::uint16_t>(first + i);
        to.nodes[slot].parent = dst;
        if (kids[i] != kNoChild) {
            if (!rebuildInto(from, kids[i], to, slot, plan)) {
                return false;
            }
        } else if (plan.insertNode != nullptr) {
            to.nodes[slot] = *plan.insertNode;
            to.nodes[slot].parent = dst;
            to.nodes[slot].nameId = slot;
            to.nodes[slot].firstChild = kNoChild;
            to.nodes[slot].childCount = 0;
        } else if (plan.moveFrom != nullptr) {
            if (!copySubtree(*plan.moveFrom, plan.moveRoot, to, slot)) {
                return false;
            }
            to.nodes[slot].parent = dst;
        }
    }
    return true;
}

/// Runs a plan over the whole scene, keeping materials and the id counter.
inline EditResult rebuild(const Scene& from, const RebuildPlan& plan) {
    EditResult r;
    r.scene.materials = from.materials;
    r.scene.nextId = from.nextId;
    r.scene.nodes.count = 1;
    r.scene.names.count = 0;
    r.scene.nodes[0].parent = kNoParent;

    if (!rebuildInto(from, 0, r.scene, 0, plan)) {
        r.scene = from;
        r.why = "the scene would exceed its " + std::to_string(Scene::kMaxNodes) + " node capacity";
        return r;
    }
    r.scene.nodes[0].parent = kNoParent;
    r.ok = true;
    return r;
}

}  // namespace detail

// ---------------------------------------------------------------- commands

/// Adds `proto` as a child of the node with id `parentId`.
///
/// `position` past the end appends. The new node's id comes from the scene's counter, so it is
/// unique for the life of the scene even after the node is deleted again.
inline EditResult addChild(const Scene& s, std::uint32_t parentId, const CsgNode& proto,
                           const std::string& name, std::uint16_t position = 0xFFFFu) {
    EditResult bad;
    bad.scene = s;

    const std::uint16_t parent = indexOfId(s, parentId);
    if (parent == kNoChild) {
        bad.why = "no node with id " + std::to_string(parentId);
        return bad;
    }

    CsgNode node = proto;
    node.id = s.nextId;
    node.firstChild = kNoChild;
    node.childCount = 0;

    detail::RebuildPlan plan;
    plan.insertUnder = parent;
    plan.insertAt = position;
    plan.insertNode = &node;

    EditResult r = detail::rebuild(s, plan);
    if (!r.ok) {
        return r;
    }
    r.scene.nextId = s.nextId + 1;
    r.newId = node.id;

    const std::uint16_t added = indexOfId(r.scene, node.id);
    if (added != kNoChild) {
        detail::setNameText(r.scene, added, name);
        r.scene.nodes[added].nameId = added;
    }
    return r;
}

/// Removes the node with id `id` and everything under it.
inline EditResult removeSubtree(const Scene& s, std::uint32_t id) {
    EditResult bad;
    bad.scene = s;

    const std::uint16_t index = indexOfId(s, id);
    if (index == kNoChild) {
        bad.why = "no node with id " + std::to_string(id);
        return bad;
    }
    if (index == 0) {
        bad.why = "the root cannot be removed";
        return bad;
    }

    detail::RebuildPlan plan;
    plan.skip = index;
    return detail::rebuild(s, plan);
}

/// Moves the node with id `id` under `newParentId`.
///
/// Refuses to move a node under its own descendant. That would cut the pair loose from the root
/// and leave a cycle, and every walk in this codebase is a plain recursion that would not come
/// back.
inline EditResult reparent(const Scene& s, std::uint32_t id, std::uint32_t newParentId,
                           std::uint16_t position = 0xFFFFu) {
    EditResult bad;
    bad.scene = s;

    const std::uint16_t index = indexOfId(s, id);
    const std::uint16_t parent = indexOfId(s, newParentId);
    if (index == kNoChild) {
        bad.why = "no node with id " + std::to_string(id);
        return bad;
    }
    if (parent == kNoChild) {
        bad.why = "no node with id " + std::to_string(newParentId);
        return bad;
    }
    if (index == 0) {
        bad.why = "the root cannot be moved";
        return bad;
    }
    if (isAncestorOf(s, index, parent)) {
        bad.why = "that would put the node under its own descendant";
        return bad;
    }
    // A move within the same parent is a reorder, and skip-then-insert already does that -- the
    // position is read against the child list with the node taken out, which is what a caller
    // dragging a row in an outliner means by "put it third".
    detail::RebuildPlan plan;
    plan.skip = index;
    plan.insertUnder = parent;
    plan.insertAt = position;
    plan.moveFrom = &s;
    plan.moveRoot = index;
    return detail::rebuild(s, plan);
}

/// Replaces a node's parameters. No structural change, so this is a copy and a patch.
inline EditResult setParams(const Scene& s, std::uint32_t id, const float params[12]) {
    EditResult r;
    r.scene = s;
    const std::uint16_t index = indexOfId(s, id);
    if (index == kNoChild) {
        r.why = "no node with id " + std::to_string(id);
        return r;
    }
    for (int i = 0; i < 12; ++i) {
        r.scene.nodes[index].params[i] = params[i];
    }
    r.ok = true;
    return r;
}

inline EditResult rename(const Scene& s, std::uint32_t id, const std::string& name) {
    EditResult r;
    r.scene = s;
    const std::uint16_t index = indexOfId(s, id);
    if (index == kNoChild) {
        r.why = "no node with id " + std::to_string(id);
        return r;
    }
    detail::setNameText(r.scene, index, name);
    r.scene.nodes[index].nameId = index;
    r.ok = true;
    return r;
}

inline EditResult setMaterial(const Scene& s, std::uint32_t id, std::uint8_t materialId) {
    EditResult r;
    r.scene = s;
    const std::uint16_t index = indexOfId(s, id);
    if (index == kNoChild) {
        r.why = "no node with id " + std::to_string(id);
        return r;
    }
    if (materialId != kNoMaterial && materialId >= s.materials.count) {
        r.why = "no material " + std::to_string(materialId);
        return r;
    }
    r.scene.nodes[index].materialId = materialId;
    r.ok = true;
    return r;
}

}  // namespace makina
