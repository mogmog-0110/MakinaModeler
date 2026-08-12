// Undo, redo, and "try it and see".
//
// A Scene is one trivially-copyable struct, so a snapshot is a copy and nothing else -- no command
// objects with an inverse, no journal to replay, no chance of an undo that does not quite undo.
// That is the whole reason the model was built as a flat POD, and this is where it pays: about
// 26 KB a step at the current capacities, so a hundred steps is under 3 MB.
//
// The reason it matters beyond convenience: an agent editing the tree needs to be able to try
// something, look at the result, and put it back exactly. "Exactly" is the hard part for a
// command-based undo, because every command needs a correct inverse and the one that is subtly
// wrong is found months later. Here there is nothing to get wrong.
//
// Not thread safe, and deliberately not: the tree has one owner (PLAN.md D-09).

#pragma once

#include "Scene.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace makina {

/// One point in the history.
struct HistoryStep {
    Scene       scene;
    std::string label;   ///< what the edit was, for a UI or a log
};

/// A bounded stack of scene snapshots.
///
/// `commit` discards anything that was redoable, which is the usual rule: once you edit after
/// undoing, the branch you undid is gone. Keeping it would need a tree of histories and a way to
/// show it, and no modeller this size earns that.
class History {
public:
    explicit History(const Scene& initial, std::size_t depth = 64)
        : m_depth(depth < 2 ? 2 : depth) {
        m_steps.push_back(HistoryStep{initial, "initial"});
    }

    [[nodiscard]] const Scene& current() const { return m_steps[m_at].scene; }
    [[nodiscard]] const std::string& label() const { return m_steps[m_at].label; }

    [[nodiscard]] std::size_t size() const { return m_steps.size(); }
    [[nodiscard]] std::size_t position() const { return m_at; }
    [[nodiscard]] bool canUndo() const { return m_at > 0; }
    [[nodiscard]] bool canRedo() const { return m_at + 1 < m_steps.size(); }

    void commit(const Scene& next, const std::string& what) {
        m_steps.resize(m_at + 1);
        m_steps.push_back(HistoryStep{next, what});

        // Drop from the front once the stack is full. The oldest state stops being reachable,
        // which is what a bounded history means; the alternative is holding every state a long
        // session ever had.
        while (m_steps.size() > m_depth) {
            m_steps.erase(m_steps.begin());
        }
        m_at = m_steps.size() - 1;
    }

    bool undo() {
        if (!canUndo()) {
            return false;
        }
        --m_at;
        return true;
    }

    bool redo() {
        if (!canRedo()) {
            return false;
        }
        ++m_at;
        return true;
    }

    /// Labels from oldest to newest, with `position()` saying where the cursor sits.
    [[nodiscard]] std::vector<std::string> labels() const {
        std::vector<std::string> out;
        out.reserve(m_steps.size());
        for (const HistoryStep& s : m_steps) {
            out.push_back(s.label);
        }
        return out;
    }

private:
    std::vector<HistoryStep> m_steps;
    std::size_t              m_at = 0;
    std::size_t              m_depth;
};

}  // namespace makina
