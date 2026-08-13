// What the shell is allowed to see, decided in one place.
//
// app/ui/shell.html binds to names like `view.tree` and `view.status.live`, and the engine's
// binder resolves a name it was never given to nothing at all -- no error, no console line, the
// element simply keeps whatever the HTML fell back to. So a key the HTML reads and C++ never
// writes is invisible from both ends: the page renders, the number never moves.
//
// BINDING.md calls this out and says a static pass cross-checks the two. That pass does not exist
// in the engine, so Makina does its own: `publishedKeys()` below is the whole vocabulary, and
// tools/shell_audit.py fails when the shell binds to something outside it. Adding a panel to the
// shell now means adding its key here, and the answer "C++ will push it eventually" is not one the
// build accepts.
//
// Values are strings because that is what crosses the bridge. Lists are JSON, matching the
// engine's `StateWriter::array` encoding, so the same document works whether Makina hosts the page
// itself or hands it to the engine.
//
// Headless on purpose. Everything here is a function of the tree, the selection and a few numbers
// the caller already has, so it is tested by ctest rather than by looking at a window.

#pragma once

#include <makina/Edit.hpp>
#include <makina/Op.hpp>
#include <makina/Scene.hpp>
#include <makina/Selection.hpp>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace makina {

/// Every name the shell may bind to. shell_audit.py reads this list out of this file.
inline const std::vector<std::string>& publishedKeys() {
    static const std::vector<std::string> kKeys = {
        "view.tree",
        "view.selection.title",
        "view.selection.fields",
        "view.status.nodes",
        "view.status.selection",
        "view.status.live",
        "view.status.frame",
        "view.distance",
    };
    return kKeys;
}

/// The fields the items of each list carry, for the same reason `publishedKeys()` exists.
///
/// Inside a `data-m-repeat` the shell names fields bare -- `data-m-text="name"` -- so they are not
/// covered by the list above, and a misspelt one is just as silent. Declared rather than derived
/// because shell_audit.py reads this file as text; viewstate_check.cpp then parses what the
/// builders actually emit and fails if the two ever disagree, which is what keeps the declaration
/// from becoming a wish.
inline const std::vector<std::pair<std::string, std::vector<std::string>>>&
publishedItemFields() {
    static const std::vector<std::pair<std::string, std::vector<std::string>>> kFields = {
        {"view.tree", {"id", "name", "op", "icon", "indent", "selected", "muted"}},
        {"view.selection.fields", {"key", "label", "value"}},
    };
    return kFields;
}

/// The numbers that do not come from the tree: the camera's distance and the frame time.
struct ViewNumbers {
    double      distance = 0.0;
    double      frameMs = 0.0;
    /// Transform.hpp's status() during a drag ("Move Y: 3 (typed)"), empty when nothing is running.
    std::string live;
};

namespace detail {

/// JSON string escaping, for names that came out of a scene file rather than out of this code.
///
/// A node called `"hex" bolt` would otherwise close the string early and the shell's whole tree
/// would fail to parse -- one bad name and the outliner goes blank, which is a long way from the
/// mistake that caused it.
inline std::string jsonQuote(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c) & 0xFFu);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out + "\"";
}

/// Trailing zeros off a number the user is going to read.
///
/// A radius of 2 shows as "2", not "2.000000". The property panel is a place people type into, and
/// a field that answers a typed 2 with 2.000000 reads as though something was changed.
inline std::string number(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

/// Which icon a node shows, following Grasp3D's MyDnDTreeCellRenderer.
///
/// The stem only -- the shell writes `assets/icons/{icon}` -- because where the files sit is the
/// page's business and which picture belongs to which op is the port's.
///
/// Sphere is the one departure, and not a choice: Grasp3D drew it with Sun's
/// `development/WebComponent16.gif` from the Java Look and Feel Graphics Repository, which is not
/// Grasp3D's own artwork and does not come with it. Lucide's circle stands in.
inline const char* iconFor(Op op) {
    switch (op) {
        case Op::Box:          return "grasp3d/box16.gif";
        case Op::Sphere:       return "lucide/circle.svg";
        case Op::Plane:        return "grasp3d/plane16.gif";
        case Op::Cylinder:     return "grasp3d/cylinder16.gif";
        case Op::Cone:         return "grasp3d/cone16.gif";
        case Op::Torus:        return "grasp3d/torus16.gif";
        case Op::Disc:         return "grasp3d/disc16.gif";
        case Op::Triangle:     return "grasp3d/triangle16.gif";
        case Op::Intersection: return "grasp3d/intersection16.gif";
        case Op::Difference:   return "grasp3d/difference16.gif";
        case Op::Merge:        return "grasp3d/merge16.gif";
        // Scale, Rotate and Translate were Sun's icons too (Zoom16, Undo16, Export16), and the
        // remaining ops never had one. Lucide's arrows stand in for the transforms; anything else
        // falls back to the same marker Grasp3D used for an unrecognised element.
        case Op::Scale:        return "lucide/scaling.svg";
        case Op::Rotate:       return "lucide/rotate-cw.svg";
        case Op::Translate:    return "lucide/move.svg";
        default:               return "lucide/info.svg";
    }
}

}  // namespace detail

/// A key and the string that goes with it. Ordered as `publishedKeys()` lists them.
using ViewEntries = std::vector<std::pair<std::string, std::string>>;

/// The outliner: one row per node, in the order the tree stores them.
///
/// Pre-order, which for this array is simply index order -- Flatten.hpp keeps a node's children
/// contiguous and after it -- so the rows read down the page the way the tree reads. `indent` is
/// the depth in pixels rather than a level, because the page has to multiply it by something and
/// doing that in CSS would need a custom property per row.
///
/// `selected` is membership in the raw selection, not in `topLevel`: the user picked those nodes
/// and expects to see them lit. `topLevel` is about which ones an *edit* reaches, and showing that
/// distinction as a highlight would explain the rule at exactly the wrong moment.
[[nodiscard]] inline std::string treeJson(const Scene& s, const Selection& selection) {
    std::string out = "[";
    for (std::uint32_t i = 0; i < s.nodes.count; ++i) {
        const CsgNode& n = s.nodes[i];
        const Op op = static_cast<Op>(n.op);

        int depth = 0;
        for (std::uint16_t p = n.parent; p != kNoParent && depth < 64; p = s.nodes[p].parent) {
            ++depth;
        }

        bool picked = false;
        for (const std::uint32_t id : selection) {
            if (id == n.id) {
                picked = true;
            }
        }

        const std::string name = s.nameOf(n);
        if (i != 0) {
            out += ",";
        }
        out += "{\"id\":" + std::to_string(n.id);
        out += ",\"name\":" + detail::jsonQuote(name.empty() ? opName(op) : name);
        out += ",\"op\":" + detail::jsonQuote(opName(op));
        out += ",\"icon\":" + detail::jsonQuote(detail::iconFor(op));
        out += ",\"indent\":" + std::to_string(depth * 12);
        out += ",\"selected\":" + std::string(picked ? "true" : "false");
        out += ",\"muted\":" +
               std::string((n.flags & flags::kMuted) != 0 ? "true" : "false");
        out += "}";
    }
    return out + "]";
}

/// The property rows for the last node picked.
///
/// The names come from `opTable()`, so the panel cannot drift from what the op actually stores --
/// deciding here that a Cone has a `height` while Op.hpp calls it something else would put a field
/// on screen that no edit can reach. Rotate's axis is not among them: it travels in `flags`, and a
/// text field that silently did nothing would be worse than no field.
[[nodiscard]] inline std::string fieldsJson(const Scene& s, const Selection& selection) {
    if (selection.empty()) {
        return "[]";
    }
    const std::uint16_t index = indexOfId(s, selection.back());
    if (index == kNoChild) {
        return "[]";
    }
    const CsgNode& n = s.nodes[index];
    const OpEntry* entry = findOp(static_cast<Op>(n.op));
    if (entry == nullptr) {
        return "[]";
    }

    std::string out = "[";
    for (int k = 0; k < 12 && entry->keys[k] != nullptr; ++k) {
        if (k != 0) {
            out += ",";
        }
        out += "{\"key\":" + detail::jsonQuote(entry->keys[k]);
        out += ",\"label\":" + detail::jsonQuote(entry->keys[k]);
        out += ",\"value\":" + detail::jsonQuote(detail::number(n.params[k]));
        out += "}";
    }
    return out + "]";
}

/// Everything the shell reads, for one frame.
[[nodiscard]] inline ViewEntries viewState(const Scene& s, const Selection& selection,
                                           const ViewNumbers& numbers) {
    std::string title = "プロパティ";
    std::string picked = "nothing selected";
    if (!selection.empty()) {
        const std::uint16_t index = indexOfId(s, selection.back());
        if (index != kNoChild) {
            const CsgNode& n = s.nodes[index];
            const std::string name = s.nameOf(n);
            title = name.empty() ? opName(static_cast<Op>(n.op)) : name;
        }
        picked = selection.size() == 1
                     ? title
                     : title + " +" + std::to_string(selection.size() - 1);
    }

    ViewEntries out;
    out.emplace_back("view.tree", treeJson(s, selection));
    out.emplace_back("view.selection.title", title);
    out.emplace_back("view.selection.fields", fieldsJson(s, selection));
    out.emplace_back("view.status.nodes", std::to_string(s.nodes.count) + " nodes");
    out.emplace_back("view.status.selection", picked);
    out.emplace_back("view.status.live", numbers.live);
    out.emplace_back("view.status.frame", detail::number(numbers.frameMs) + " ms");
    out.emplace_back("view.distance", detail::number(numbers.distance));
    return out;
}

}  // namespace makina
