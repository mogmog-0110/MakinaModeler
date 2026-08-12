// Scene edits as JSON commands, over a history.
//
// This is the surface an agent drives. It exists because the alternative -- letting a caller build
// a CsgNode and call Edit.hpp directly -- means the caller has to know that a cylinder's radius is
// params[2], and that is a fact about the storage layout, not about the model. Here a command says
// `{"radius": 0.7}` and Op.hpp's key table does the rest, so the same names appear in the JSON
// scene file, in the command, and in the error when the name is wrong.
//
// Two properties matter more than the command set:
//
//   nothing half-applies   a command that cannot run leaves the history untouched and says why.
//                          An agent can send a batch and read back what happened, rather than
//                          discovering it edited three of five things.
//   everything is undoable each successful command is one history step, and a step is a snapshot
//                          (History.hpp). "Try it, look, put it back" is therefore exact, which is
//                          the whole reason an agent can experiment on a real model.

#pragma once

#include "Edit.hpp"
#include "History.hpp"
#include "Op.hpp"
#include "Scene.hpp"
#include "SceneJson.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace makina {

struct CommandResult {
    bool          ok = false;
    std::string   message;   ///< what happened, or what was wrong with the command
    std::uint32_t newId = 0; ///< set by "add"
};

namespace detail {

/// Fills a node's parameters from named keys, so a caller never indexes params[] by hand.
///
/// Returns the first unknown key, or "" when every key was recognised. Unknown keys are refused
/// rather than ignored: a typo that silently does nothing is worse than one that says so, and an
/// agent has no way to notice the difference.
inline std::string applyNamedParams(CsgNode& n, const nlohmann::json& params) {
    const OpEntry* entry = findOp(static_cast<Op>(n.op));
    if (entry == nullptr) {
        return "(this op takes no parameters)";
    }
    for (auto it = params.begin(); it != params.end(); ++it) {
        bool found = false;
        for (int i = 0; i < 12 && entry->keys[i] != nullptr; ++i) {
            if (it.key() == entry->keys[i]) {
                n.params[i] = it.value().get<float>();
                found = true;
                break;
            }
        }
        if (!found) {
            return it.key();
        }
    }
    return std::string();
}

/// Every parameter name this op accepts, for an error message that is actually useful.
inline std::string keyList(Op op) {
    const OpEntry* entry = findOp(op);
    if (entry == nullptr) {
        return "(none)";
    }
    std::string out;
    for (int i = 0; i < 12 && entry->keys[i] != nullptr; ++i) {
        if (!out.empty()) {
            out += ", ";
        }
        out += entry->keys[i];
    }
    return out.empty() ? "(none)" : out;
}

/// Builds the node an "add" command describes.
inline bool buildNode(const nlohmann::json& spec, CsgNode& out, std::string& why) {
    const std::string opName = spec.value("op", std::string());
    const OpEntry* entry = findOp(opName.c_str());
    if (entry == nullptr) {
        why = "unknown op '" + opName + "'";
        return false;
    }

    out = CsgNode{};
    out.op = static_cast<std::uint8_t>(entry->op);
    out.materialId = static_cast<std::uint8_t>(spec.value("material", int(kNoMaterial)));
    out.firstChild = kNoChild;
    out.childCount = 0;

    if (entry->op == Op::Rotate) {
        const std::string axis = spec.value("axis", std::string("X"));
        out.flags |= detail::axisFromString(axis);
    }
    if (entry->op == Op::Cone && spec.value("open", false)) {
        out.flags |= flags::kConeOpen;
    }

    // Parameters may arrive either nested under "params" or flat beside "op", because both read
    // naturally and an agent should not have to remember which.
    nlohmann::json params = spec.value("params", nlohmann::json::object());
    for (auto it = spec.begin(); it != spec.end(); ++it) {
        const std::string& k = it.key();
        if (k == "op" || k == "params" || k == "material" || k == "axis" || k == "open" ||
            k == "name") {
            continue;
        }
        params[k] = it.value();
    }

    const std::string bad = applyNamedParams(out, params);
    if (!bad.empty()) {
        why = "op '" + opName + "' has no parameter '" + bad + "'; it takes " +
              keyList(entry->op);
        return false;
    }
    return true;
}

}  // namespace detail

/// Runs one command against a history, committing on success.
///
/// The history is only touched when the command succeeds, so a caller can run a batch and stop at
/// the first failure with the scene in the last good state rather than somewhere in between.
inline CommandResult runCommand(History& history, const nlohmann::json& cmd) {
    CommandResult r;
    const std::string op = cmd.value("op", std::string());
    const Scene& s = history.current();

    if (op == "undo") {
        r.ok = history.undo();
        r.message = r.ok ? "undid '" + history.label() + "'" : "nothing to undo";
        return r;
    }
    if (op == "redo") {
        r.ok = history.redo();
        r.message = r.ok ? "redid '" + history.label() + "'" : "nothing to redo";
        return r;
    }

    if (op == "add") {
        if (!cmd.contains("node")) {
            r.message = "add needs a 'node'";
            return r;
        }
        CsgNode node{};
        std::string why;
        if (!detail::buildNode(cmd["node"], node, why)) {
            r.message = why;
            return r;
        }
        const std::string name = cmd["node"].value("name", cmd.value("name", std::string()));
        const std::uint16_t at =
            static_cast<std::uint16_t>(cmd.value("at", int(0xFFFF)));
        const EditResult e = addChild(s, cmd.value("parent", 0u), node, name, at);
        if (!e.ok) {
            r.message = e.why;
            return r;
        }
        history.commit(e.scene, "add " + std::string(opName(static_cast<Op>(node.op))));
        r.ok = true;
        r.newId = e.newId;
        r.message = "added id " + std::to_string(e.newId);
        return r;
    }

    if (op == "remove") {
        const EditResult e = removeSubtree(s, cmd.value("id", 0u));
        if (!e.ok) {
            r.message = e.why;
            return r;
        }
        history.commit(e.scene, "remove id " + std::to_string(cmd.value("id", 0u)));
        r.ok = true;
        r.message = "removed";
        return r;
    }

    if (op == "move") {
        const std::uint16_t at = static_cast<std::uint16_t>(cmd.value("at", int(0xFFFF)));
        const EditResult e = reparent(s, cmd.value("id", 0u), cmd.value("parent", 0u), at);
        if (!e.ok) {
            r.message = e.why;
            return r;
        }
        history.commit(e.scene, "move id " + std::to_string(cmd.value("id", 0u)));
        r.ok = true;
        r.message = "moved";
        return r;
    }

    if (op == "set") {
        const std::uint32_t id = cmd.value("id", 0u);
        const std::uint16_t index = indexOfId(s, id);
        if (index == kNoChild) {
            r.message = "no node with id " + std::to_string(id);
            return r;
        }
        CsgNode node = s.nodes[index];
        nlohmann::json params = cmd.value("params", nlohmann::json::object());
        for (auto it = cmd.begin(); it != cmd.end(); ++it) {
            if (it.key() == "op" || it.key() == "id" || it.key() == "params") {
                continue;
            }
            params[it.key()] = it.value();
        }
        const std::string bad = detail::applyNamedParams(node, params);
        if (!bad.empty()) {
            r.message = "op '" + std::string(opName(static_cast<Op>(node.op))) +
                        "' has no parameter '" + bad + "'; it takes " +
                        detail::keyList(static_cast<Op>(node.op));
            return r;
        }
        const EditResult e = setParams(s, id, node.params);
        if (!e.ok) {
            r.message = e.why;
            return r;
        }
        history.commit(e.scene, "set on id " + std::to_string(id));
        r.ok = true;
        r.message = "set";
        return r;
    }

    if (op == "rename") {
        const EditResult e = rename(s, cmd.value("id", 0u), cmd.value("name", std::string()));
        if (!e.ok) {
            r.message = e.why;
            return r;
        }
        history.commit(e.scene, "rename id " + std::to_string(cmd.value("id", 0u)));
        r.ok = true;
        r.message = "renamed";
        return r;
    }

    if (op == "material") {
        const EditResult e = setMaterial(s, cmd.value("id", 0u),
                                         static_cast<std::uint8_t>(cmd.value("material", 255)));
        if (!e.ok) {
            r.message = e.why;
            return r;
        }
        history.commit(e.scene, "material on id " + std::to_string(cmd.value("id", 0u)));
        r.ok = true;
        r.message = "material set";
        return r;
    }

    r.message = "unknown command '" + op + "'; expected one of add, remove, move, set, rename, "
                "material, undo, redo";
    return r;
}

/// Runs commands until one fails.
///
/// Stopping is the point. Carrying on past a failure would leave the caller reading a list of
/// results to work out which of its later commands acted on the tree it thought it had.
inline std::vector<CommandResult> runCommands(History& history, const nlohmann::json& list) {
    std::vector<CommandResult> out;
    if (!list.is_array()) {
        CommandResult r;
        r.message = "expected an array of commands";
        out.push_back(r);
        return out;
    }
    for (const auto& cmd : list) {
        out.push_back(runCommand(history, cmd));
        if (!out.back().ok) {
            break;
        }
    }
    return out;
}

}  // namespace makina
