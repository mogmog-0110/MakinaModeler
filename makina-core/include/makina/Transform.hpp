// Moving, rotating and scaling: the modal state machine, and where the numbers land in the tree.
//
// This is the part of the plan that says it matters most -- "CSG is dimension-driven modelling, so
// typing the number is what pays". Pressing G, then X, then 5, then Enter should mean "five along
// X", exactly five, not however far the mouse happened to go.
//
// Two problems, kept apart because they fail differently:
//
//   TransformSession  what the user is in the middle of. Which kind, which axis, what has been
//                     typed, what the header should read. Pure state; no scene, no tree.
//
//   applyTransform    where a translation actually goes in a CSG tree. There is no "position" on
//                     a primitive -- placement is a Translate node above it -- so moving something
//                     either edits the transform it already has or gives it one.
//
// The second is the one with a real decision in it, and the decision is: **edit the transform the
// node already carries; otherwise wrap it in a new one**. The alternative -- always adding another
// Translate -- grows a chain of them, one per drag, and a tree nobody can read afterwards.

#pragma once

#include "Edit.hpp"
#include "Op.hpp"
#include "Scene.hpp"

#include <cmath>
#include <cstdint>
#include <string>

namespace makina {

enum class TransformKind { None, Move, Rotate, Scale };
enum class TransformAxis { None, X, Y, Z };

/// What the user is part-way through.
///
/// Deliberately holds no scene. A transform in progress is a question about the input, and mixing
/// it with the tree is what makes "cancel put it back" hard: here cancelling is simply never
/// committing, because nothing was changed in the first place.
class TransformSession {
public:
    [[nodiscard]] bool active() const noexcept { return m_kind != TransformKind::None; }
    [[nodiscard]] TransformKind kind() const noexcept { return m_kind; }
    [[nodiscard]] TransformAxis axis() const noexcept { return m_axis; }
    [[nodiscard]] bool typing() const noexcept { return !m_typed.empty(); }

    void begin(TransformKind kind) {
        m_kind = kind;
        m_axis = TransformAxis::None;
        m_typed.clear();
        m_mouse = 0.0;
        m_snap = false;
    }

    void cancel() { m_kind = TransformKind::None; }

    /// Constrains to an axis, or lifts the constraint when the same one is pressed again.
    ///
    /// Pressing X twice to release is Blender's behaviour and it is the reason people press it
    /// without looking -- an axis you cannot get out of is worse than no axis.
    void setAxis(TransformAxis axis) {
        m_axis = (m_axis == axis) ? TransformAxis::None : axis;
    }

    /// Cursor movement while a transform is running.
    ///
    /// Ignored once anything has been typed. Blender does the same, and the reason is not
    /// consistency: having typed "5", the hand is on the keyboard, and letting a stray twitch of
    /// the mouse move the value would make the typed number a suggestion rather than an answer.
    void mouseDelta(double amount) {
        if (m_typed.empty()) {
            m_mouse += amount;
        }
    }

    /// A digit, a minus, or a decimal point.
    ///
    /// Returns false for anything else, so a caller can pass every key through and let this decide
    /// what belongs to the number.
    bool type(char c) {
        if (!active()) {
            return false;
        }
        if (c >= '0' && c <= '9') {
            m_typed.push_back(c);
            return true;
        }
        if (c == '-' && m_typed.empty()) {
            m_typed.push_back(c);
            return true;
        }
        if (c == '.' && m_typed.find('.') == std::string::npos) {
            // A leading point reads as 0.5 rather than as an error.
            if (m_typed.empty() || m_typed == "-") {
                m_typed.push_back('0');
            }
            m_typed.push_back(c);
            return true;
        }
        return false;
    }

    /// Removes the last typed character. Emptying the buffer hands control back to the mouse.
    void backspace() {
        if (!m_typed.empty()) {
            m_typed.pop_back();
        }
    }

    /// Grid snapping while held.
    void setSnap(bool on) { m_snap = on; }

    /// The number the transform currently stands for.
    ///
    /// Scale starts from 1 and everything else from 0, so an empty session is the identity for
    /// whichever kind it is -- confirming without touching anything leaves the model alone.
    [[nodiscard]] double value(double snapStep = 0.1) const {
        const double identity = (m_kind == TransformKind::Scale) ? 1.0 : 0.0;
        if (!active()) {
            return identity;
        }
        if (!m_typed.empty()) {
            // A buffer of "-" or "0." on its own is not a number yet; treat it as the identity
            // rather than as zero, so a half-typed minus does not collapse a scale.
            try {
                return std::stod(m_typed);
            } catch (const std::exception&) {
                return identity;
            }
        }
        double v = (m_kind == TransformKind::Scale) ? 1.0 + m_mouse : m_mouse;
        if (m_snap && snapStep > 0.0) {
            v = std::round(v / snapStep) * snapStep;
        }
        return v;
    }

    /// What the header shows: the operation, the axis, and the number as it stands.
    ///
    /// Live feedback is the difference between typing a number confidently and typing it hoping.
    /// The trailing marker distinguishes a typed value from a dragged one, because the two behave
    /// differently and the user needs to know which they are in.
    [[nodiscard]] std::string status(double snapStep = 0.1) const {
        if (!active()) {
            return std::string();
        }
        std::string out;
        switch (m_kind) {
            case TransformKind::Move:   out = "Move"; break;
            case TransformKind::Rotate: out = "Rotate"; break;
            case TransformKind::Scale:  out = "Scale"; break;
            default: break;
        }
        switch (m_axis) {
            case TransformAxis::X: out += " X"; break;
            case TransformAxis::Y: out += " Y"; break;
            case TransformAxis::Z: out += " Z"; break;
            case TransformAxis::None: break;
        }
        out += ": ";
        if (!m_typed.empty()) {
            out += m_typed;
        } else {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.3f", value(snapStep));
            out += buf;
        }
        if (m_kind == TransformKind::Rotate) {
            out += " deg";
        }
        if (!m_typed.empty()) {
            out += "  (typed)";
        } else if (m_snap) {
            out += "  (snap)";
        }
        return out;
    }

private:
    TransformKind m_kind = TransformKind::None;
    TransformAxis m_axis = TransformAxis::None;
    std::string   m_typed;
    double        m_mouse = 0.0;
    bool          m_snap = false;
};

// ---------------------------------------------------------------- applying it

namespace detail {

/// The op a transform of this kind is expressed by.
inline Op transformOp(TransformKind kind) {
    switch (kind) {
        case TransformKind::Move:   return Op::Translate;
        case TransformKind::Rotate: return Op::Rotate;
        case TransformKind::Scale:  return Op::Scale;
        default:                    return Op::Unsupported;
    }
}

inline std::uint16_t axisFlag(TransformAxis axis) {
    switch (axis) {
        case TransformAxis::Y: return flags::kAxisY;
        case TransformAxis::Z: return flags::kAxisZ;
        default:               return flags::kAxisX;
    }
}

}  // namespace detail

/// Applies a transform to the node with this id.
///
/// Where it lands:
///
///   the node already is a transform of that kind   its parameters change
///   it is not                                      a new transform node is put above it
///
/// The second case is a structural edit -- insert, then reparent -- but it comes back as one
/// scene, so the caller commits one history step and one undo puts the whole thing back.
///
/// A rotation with no axis is refused rather than guessed. Grasp3D's Rotate is single-axis by
/// construction, so "rotate by 30" has no meaning until an axis is chosen, and silently picking X
/// would turn a mis-keyed transform into a wrong model.
inline EditResult applyTransform(const Scene& s, std::uint32_t id, TransformKind kind,
                                 TransformAxis axis, double amount) {
    EditResult bad;
    bad.scene = s;

    const Op want = detail::transformOp(kind);
    if (want == Op::Unsupported) {
        bad.why = "no transform in progress";
        return bad;
    }
    const std::uint16_t index = indexOfId(s, id);
    if (index == kNoChild) {
        bad.why = "no node with id " + std::to_string(id);
        return bad;
    }
    if (index == 0) {
        bad.why = "the root cannot be transformed";
        return bad;
    }
    if (kind == TransformKind::Rotate && axis == TransformAxis::None) {
        bad.why = "a rotation needs an axis";
        return bad;
    }

    // Editing an existing node of the right kind, when there is one. A Rotate only counts if it
    // already turns about the axis being asked for -- adding 30 degrees of Y to a node that
    // rotates about X would silently change what the existing rotation meant.
    const CsgNode& node = s.nodes[index];
    const auto rightKind = [&](const CsgNode& n) {
        return static_cast<Op>(n.op) == want &&
               (kind != TransformKind::Rotate ||
                (n.flags & flags::kAxisMask) == detail::axisFlag(axis));
    };

    // The wrapper this function put there last time counts as well.
    //
    // Without this, nudging the same part twice leaves two Translates stacked on it, and ten
    // nudges leave ten. The tree is a fixed-capacity array, so that is not only untidy -- a
    // session of small adjustments walks towards the node limit and the outliner fills with
    // wrappers nobody made on purpose.
    //
    // Only when the parent holds this node and nothing else. A transform with several children is
    // a group the user built, and folding one child's move into it would move its siblings too.
    std::uint32_t editId = id;
    bool reuse = rightKind(node);
    if (!reuse && node.parent != kNoParent && node.parent != 0) {
        const CsgNode& above = s.nodes[node.parent];
        if (above.childCount == 1 && rightKind(above)) {
            reuse = true;
            editId = above.id;
        }
    }

    if (reuse) {
        const CsgNode& target = s.nodes[indexOfId(s, editId)];
        float params[12];
        for (int i = 0; i < 12; ++i) {
            params[i] = target.params[i];
        }
        switch (kind) {
            case TransformKind::Move:
                if (axis == TransformAxis::None) {
                    bad.why = "a move needs an axis";
                    return bad;
                }
                params[axis == TransformAxis::X ? 0 : axis == TransformAxis::Y ? 1 : 2] +=
                    static_cast<float>(amount);
                break;
            case TransformKind::Rotate:
                params[0] += static_cast<float>(amount);
                break;
            case TransformKind::Scale: {
                // Multiplied, not added: scaling by 2 twice is four times, and adding would make
                // it three. Uniform when no axis is named, which is what dragging a scale does.
                const double f = amount;
                if (axis == TransformAxis::None) {
                    for (int i = 0; i < 3; ++i) {
                        params[i] = static_cast<float>(params[i] * f);
                    }
                } else {
                    const int i = axis == TransformAxis::X ? 0 : axis == TransformAxis::Y ? 1 : 2;
                    params[i] = static_cast<float>(params[i] * f);
                }
                break;
            }
            default:
                break;
        }
        return setParams(s, editId, params);
    }

    // Otherwise the node gains a transform above it.
    CsgNode proto{};
    proto.op = static_cast<std::uint8_t>(want);
    proto.materialId = kNoMaterial;
    switch (kind) {
        case TransformKind::Move:
            if (axis == TransformAxis::None) {
                bad.why = "a move needs an axis";
                return bad;
            }
            proto.params[axis == TransformAxis::X ? 0 : axis == TransformAxis::Y ? 1 : 2] =
                static_cast<float>(amount);
            break;
        case TransformKind::Rotate:
            proto.params[0] = static_cast<float>(amount);
            proto.flags |= detail::axisFlag(axis);
            break;
        case TransformKind::Scale:
            // A fresh Scale starts at 1 on the axes it does not touch, or the node collapses.
            for (int i = 0; i < 3; ++i) {
                proto.params[i] = 1.0f;
            }
            if (axis == TransformAxis::None) {
                for (int i = 0; i < 3; ++i) {
                    proto.params[i] = static_cast<float>(amount);
                }
            } else {
                proto.params[axis == TransformAxis::X ? 0 : axis == TransformAxis::Y ? 1 : 2] =
                    static_cast<float>(amount);
            }
            break;
        default:
            break;
    }

    const std::uint32_t parentId = s.nodes[s.nodes[index].parent].id;
    // Inserted where the node currently sits, so the tree reads the same afterwards. Appending
    // instead would reorder the siblings, and under a Difference the order is the geometry.
    std::uint16_t position = 0;
    {
        const CsgNode& parent = s.nodes[s.nodes[index].parent];
        for (std::uint16_t i = 0; i < parent.childCount; ++i) {
            if (static_cast<std::uint16_t>(parent.firstChild + i) == index) {
                position = i;
                break;
            }
        }
    }

    const std::string name = std::string(opName(want));
    EditResult inserted = addChild(s, parentId, proto, name, position);
    if (!inserted.ok) {
        return inserted;
    }
    // Reparenting under the node just made. Both edits are folded into one scene, so this is one
    // step in the history and one undo takes both back.
    EditResult moved = reparent(inserted.scene, id, inserted.newId);
    if (!moved.ok) {
        moved.scene = s;
        return moved;
    }
    moved.newId = inserted.newId;
    return moved;
}

}  // namespace makina
