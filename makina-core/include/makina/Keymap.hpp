// What a key or a drag means, as data.
//
// The keys are not in the code because Phase 3's exit condition is "someone who uses Maya or
// Blender picks it up and is not lost", and the only way to act on what they say is to change a
// binding while they are still sitting there. A rebuild between "this should be the middle button"
// and trying it is enough to end the session.
//
// Two presets ship. They differ in more than which key: Maya holds a modifier and uses all three
// mouse buttons for the camera, Blender puts the camera on the middle button and gives the left
// one to selection. Encoding that as data rather than as a branch is what keeps a third preset
// from being a third branch.
//
// This file resolves; it does not read input. Nothing here knows about a window.

#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace makina {

/// Modifier flags, combinable.
namespace mods {
constexpr int kNone = 0;
constexpr int kShift = 1 << 0;
constexpr int kCtrl = 1 << 1;
constexpr int kAlt = 1 << 2;
}  // namespace mods

enum class MouseButton { None, Left, Middle, Right };

/// When a binding applies.
///
/// Without this the two presets are not expressible. Blender's X is "delete" normally and
/// "constrain to X" while a transform is running, and that is not a different key -- it is the
/// same key in a different mode. Resolving without a context makes one of the two unreachable,
/// and which one depends on the order of the file.
enum class KeyContext {
    Global,     ///< nothing in progress
    Transform,  ///< a move / rotate / scale is running
};

/// A key press or a mouse drag, as it arrives from the platform.
struct InputEvent {
    /// Uppercase name: "F", "A", "NUMPAD1", "TAB". Empty for a pure mouse gesture.
    std::string key;
    MouseButton button = MouseButton::None;
    int         modifiers = mods::kNone;
    /// True when the button is held and the cursor is moving, false for a click or a key press.
    bool        dragging = false;
    KeyContext  context = KeyContext::Global;
};

/// What the app should do. Empty means the event is not bound.
///
/// A string rather than an enum: the set grows as the app grows, and an enum here would make
/// every new command a change to this file plus a change to the map plus a change to the switch.
/// The cost is that a typo is only found when the action never fires, which is why `load` checks
/// every binding against `knownActions()` instead of accepting whatever it is given.
using Action = std::string;

/// Actions this build knows how to carry out.
///
/// Listed so a keymap file can be checked when it loads rather than when a user presses the key
/// and nothing happens. A binding to an unknown action is a typo every time.
///
/// The list has to mean what it says, which is what keymap_audit.cpp holds it to: every name here
/// must appear in the viewport that carries them out. `select.add` sat here for a while and the
/// viewport never named it, so Shift and a click did nothing at all -- not even the plain pick,
/// because the event resolved to an action no branch claimed.
inline const std::vector<std::string>& knownActions() {
    static const std::vector<std::string> kActions = {
        "view.orbit",      "view.pan",         "view.dolly",
        "view.fitSelected", "view.fitAll",
        "view.front",      "view.right",       "view.top",
        "view.back",       "view.left",        "view.bottom",
        "view.toggleOrthographic",
        // The camera as it was before an axis view took it over. Grasp3D keeps its
        // interactive camera apart from the X/Y/Z-direction ones and its toolbar goes back
        // to it; here lookAlong() overwrites yaw and pitch, so a view someone spent a
        // minute framing is gone the moment they glance down an axis.
        //
        // No preset binds it. It is a toolbar control in Grasp3D and inventing a keystroke
        // for it would be inventing one neither Maya nor Blender users have -- keymap_audit
        // takes the shell as a third source for exactly this case.
        "view.genuine",
        "select.pick",     "select.add",       "select.descend",  "select.clear",
        "select.box",
        "edit.move",       "edit.rotate",      "edit.scale",
        "edit.duplicate",  "edit.delete",      "edit.toggleMute",
        "edit.undo",       "edit.redo",
        "axis.x",          "axis.y",           "axis.z",
        "snap.hold",
    };
    return kActions;
}

/// Which command does the same thing as an `edit.` action.
///
/// Phase 3's condition is that the same edits are reachable from the command line as from the
/// viewport, and that condition was quietly false twice: muting had a key and no command, and
/// translate, rotate and scale had never had one at all -- `move` in the command layer is a
/// reparent, so a script could put a node elsewhere in the tree and not shift it a millimetre.
/// Both were found by hand, one after the other, which is not a way to keep a promise.
///
/// So the pairing is declared rather than remembered. keymap_audit walks this table and fails when
/// an `edit.` action has no entry, or when the command it names is not one the command layer
/// dispatches. Adding an edit to the viewport now means saying which command performs it, and the
/// answer "none yet" is not one the build accepts.
///
/// Only `edit.` actions. A camera has nothing to reach from a script -- there is no camera in a
/// scene file -- and a selection is state the viewport holds while a command names its node by id.
inline const std::vector<std::pair<std::string, std::string>>& editCommands() {
    static const std::vector<std::pair<std::string, std::string>> kPairs = {
        {"edit.move", "translate"},
        {"edit.rotate", "rotate"},
        {"edit.scale", "scale"},
        {"edit.duplicate", "duplicate"},
        {"edit.delete", "remove"},
        {"edit.toggleMute", "mute"},
        {"edit.undo", "undo"},
        {"edit.redo", "redo"},
    };
    return kPairs;
}

namespace detail {

inline int parseModifiers(const nlohmann::json& j) {
    int m = mods::kNone;
    if (!j.is_array()) {
        return m;
    }
    for (const auto& entry : j) {
        const std::string name = entry.get<std::string>();
        if (name == "shift") m |= mods::kShift;
        else if (name == "ctrl") m |= mods::kCtrl;
        else if (name == "alt") m |= mods::kAlt;
    }
    return m;
}

inline MouseButton parseButton(const std::string& name) {
    if (name == "left") return MouseButton::Left;
    if (name == "middle") return MouseButton::Middle;
    if (name == "right") return MouseButton::Right;
    return MouseButton::None;
}

/// One binding, flattened to something comparable.
struct Binding {
    std::string key;
    MouseButton button = MouseButton::None;
    int         modifiers = mods::kNone;
    bool        dragging = false;
    KeyContext  context = KeyContext::Global;
    Action      action;
};

inline KeyContext parseContext(const std::string& name) {
    return name == "transform" ? KeyContext::Transform : KeyContext::Global;
}

/// Whether two bindings would be reached by the same event.
inline bool sameTrigger(const Binding& a, const Binding& b) {
    return a.key == b.key && a.button == b.button && a.modifiers == b.modifiers &&
           a.dragging == b.dragging && a.context == b.context;
}

}  // namespace detail

/// A resolved keymap.
class Keymap {
public:
    /// Loads from the JSON form. Returns false and leaves the map untouched on a bad file.
    ///
    /// Untouched on failure matters: a user editing a keymap while the app runs should get their
    /// old bindings back with an error, not an application with no camera controls.
    [[nodiscard]] bool load(const std::string& text, std::string& error) {
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(text);
        } catch (const std::exception& e) {
            error = std::string("keymap is not well formed JSON: ") + e.what();
            return false;
        }

        if (!j.contains("bindings") || !j["bindings"].is_array()) {
            error = "keymap has no \"bindings\" array";
            return false;
        }

        std::vector<detail::Binding> parsed;
        for (const auto& b : j["bindings"]) {
            detail::Binding out;
            out.action = b.value("action", std::string());
            if (out.action.empty()) {
                error = "a binding has no action";
                return false;
            }
            bool known = false;
            for (const std::string& a : knownActions()) {
                if (a == out.action) {
                    known = true;
                    break;
                }
            }
            if (!known) {
                // Refused rather than ignored. An unrecognised action is a typo, and a typo that
                // loads quietly turns into "that key does nothing" much later.
                error = "unknown action '" + out.action + "'";
                return false;
            }

            out.key = b.value("key", std::string());
            out.button = detail::parseButton(b.value("button", std::string()));
            out.modifiers = detail::parseModifiers(b.value("modifiers", nlohmann::json::array()));
            out.dragging = b.value("drag", false);
            out.context = detail::parseContext(b.value("context", std::string("global")));

            if (out.key.empty() && out.button == MouseButton::None) {
                error = "binding for '" + out.action + "' names neither a key nor a button";
                return false;
            }

            // Two bindings reachable by the same event: one of them can never fire, and which one
            // depends on the order of the file. Caught here rather than left as "that key does
            // the wrong thing".
            for (const detail::Binding& seen : parsed) {
                if (detail::sameTrigger(seen, out)) {
                    error = "'" + seen.action + "' and '" + out.action +
                            "' are bound to the same thing";
                    return false;
                }
            }
            parsed.push_back(std::move(out));
        }

        m_name = j.value("name", std::string("unnamed"));
        m_bindings = std::move(parsed);
        error.clear();
        return true;
    }

    /// What this event means, or "" when it is not bound.
    ///
    /// Modifiers must match exactly. A subset match would make Alt+drag also fire the plain drag
    /// binding, so orbiting would select at the same time.
    [[nodiscard]] Action resolve(const InputEvent& e) const {
        for (const detail::Binding& b : m_bindings) {
            if (b.context != e.context) {
                continue;
            }
            if (b.modifiers != e.modifiers) {
                continue;
            }
            if (b.button != e.button) {
                continue;
            }
            if (b.dragging != e.dragging) {
                continue;
            }
            if (b.key != e.key) {
                continue;
            }
            return b.action;
        }
        return Action{};
    }

    /// Every event bound to this action, for showing the user what the key is.
    [[nodiscard]] std::vector<InputEvent> bindingsFor(const Action& action) const {
        std::vector<InputEvent> out;
        for (const detail::Binding& b : m_bindings) {
            if (b.action == action) {
                out.push_back(InputEvent{b.key, b.button, b.modifiers, b.dragging, b.context});
            }
        }
        return out;
    }

    /// Actions in `knownActions()` that this keymap never binds.
    ///
    /// Not an error -- a preset may leave something unbound on purpose -- but a UI should be able
    /// to say so, and a preset that has quietly lost its pan binding should be findable.
    [[nodiscard]] std::vector<std::string> unbound() const {
        std::vector<std::string> out;
        for (const std::string& a : knownActions()) {
            bool found = false;
            for (const detail::Binding& b : m_bindings) {
                if (b.action == a) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                out.push_back(a);
            }
        }
        return out;
    }

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] std::size_t size() const noexcept { return m_bindings.size(); }

private:
    std::string                 m_name;
    std::vector<detail::Binding> m_bindings;
};

// ---------------------------------------------------------------- presets

/// Maya: a modifier plus all three mouse buttons for the camera, leaving an unmodified click free
/// for selection.
inline const char* mayaKeymapJson() {
    return R"({
  "name": "maya",
  "bindings": [
    { "action": "view.orbit",  "button": "left",   "drag": true, "modifiers": ["alt"] },
    { "action": "view.pan",    "button": "middle", "drag": true, "modifiers": ["alt"] },
    { "action": "view.dolly",  "button": "right",  "drag": true, "modifiers": ["alt"] },

    { "action": "select.pick",    "button": "left", "drag": false },
    { "action": "select.add",     "button": "left", "drag": false, "modifiers": ["shift"] },
    { "action": "select.box",     "button": "left", "drag": true },
    { "action": "select.descend", "button": "left", "drag": false, "modifiers": ["ctrl"] },
    { "action": "select.clear",   "key": "A", "modifiers": ["alt"] },

    { "action": "view.fitSelected", "key": "F" },
    { "action": "view.fitAll",      "key": "A" },

    { "action": "edit.move",   "key": "W" },
    { "action": "edit.rotate", "key": "E" },
    { "action": "edit.scale",  "key": "R" },

    { "action": "edit.duplicate", "key": "D", "modifiers": ["ctrl"] },
    { "action": "edit.delete",    "key": "DELETE" },
    { "action": "edit.toggleMute", "key": "H" },
    { "action": "edit.undo",      "key": "Z", "modifiers": ["ctrl"] },
    { "action": "edit.redo",      "key": "Y", "modifiers": ["ctrl"] },

    { "action": "axis.x", "key": "X", "context": "transform" },
    { "action": "axis.y", "key": "Y", "context": "transform" },
    { "action": "axis.z", "key": "Z", "context": "transform" },
    { "action": "snap.hold", "key": "CONTROL" }
  ]
})";
}

/// Blender: the camera lives on the middle button with no modifier, the left button selects, and
/// the numpad holds the axis views.
inline const char* blenderKeymapJson() {
    return R"({
  "name": "blender",
  "bindings": [
    { "action": "view.orbit", "button": "middle", "drag": true },
    { "action": "view.pan",   "button": "middle", "drag": true, "modifiers": ["shift"] },
    { "action": "view.dolly", "button": "middle", "drag": true, "modifiers": ["ctrl"] },

    { "action": "select.pick",    "button": "left", "drag": false },
    { "action": "select.add",     "button": "left", "drag": false, "modifiers": ["shift"] },
    { "action": "select.box",     "button": "left", "drag": true },
    { "action": "select.descend", "button": "left", "drag": false, "modifiers": ["alt"] },
    { "action": "select.clear",   "key": "A", "modifiers": ["alt"] },

    { "action": "view.fitSelected", "key": "NUMPADPERIOD" },
    { "action": "view.fitAll",      "key": "HOME" },
    { "action": "view.front",  "key": "NUMPAD1" },
    { "action": "view.back",   "key": "NUMPAD1", "modifiers": ["ctrl"] },
    { "action": "view.right",  "key": "NUMPAD3" },
    { "action": "view.left",   "key": "NUMPAD3", "modifiers": ["ctrl"] },
    { "action": "view.top",    "key": "NUMPAD7" },
    { "action": "view.bottom", "key": "NUMPAD7", "modifiers": ["ctrl"] },
    { "action": "view.toggleOrthographic", "key": "NUMPAD5" },

    { "action": "edit.move",   "key": "G" },
    { "action": "edit.rotate", "key": "R" },
    { "action": "edit.scale",  "key": "S" },

    { "action": "edit.duplicate", "key": "D", "modifiers": ["shift"] },
    { "action": "edit.delete",    "key": "X" },
    { "action": "edit.toggleMute", "key": "H" },
    { "action": "edit.undo",      "key": "Z", "modifiers": ["ctrl"] },
    { "action": "edit.redo",      "key": "Z", "modifiers": ["ctrl", "shift"] },

    { "action": "axis.x", "key": "X", "context": "transform" },
    { "action": "axis.y", "key": "Y", "context": "transform" },
    { "action": "axis.z", "key": "Z", "context": "transform" },
    { "action": "snap.hold", "key": "CONTROL" }
  ]
})";
}

}  // namespace makina
