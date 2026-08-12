// Checks the keymap, and mostly checks that it refuses bad ones.
//
// A keymap gets edited by hand, while someone is sitting there saying "no, the middle button".
// The mistakes that follow are all of one kind -- a typo in an action name, two things bound to
// the same gesture -- and every one of them, if accepted, shows up as "that key does nothing" or
// "that key does the wrong thing" long after the edit that caused it. So loading is strict, and
// most of the checks here are on the refusals.

#include <makina/Keymap.hpp>

#include <cstdio>
#include <string>

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

makina::InputEvent key(const std::string& k, int modifiers = makina::mods::kNone,
                       makina::KeyContext ctx = makina::KeyContext::Global) {
    makina::InputEvent e;
    e.key = k;
    e.modifiers = modifiers;
    e.context = ctx;
    return e;
}

makina::InputEvent drag(makina::MouseButton b, int modifiers = makina::mods::kNone) {
    makina::InputEvent e;
    e.button = b;
    e.modifiers = modifiers;
    e.dragging = true;
    return e;
}

makina::InputEvent click(makina::MouseButton b, int modifiers = makina::mods::kNone) {
    makina::InputEvent e;
    e.button = b;
    e.modifiers = modifiers;
    return e;
}

}  // namespace

int main() {
    std::printf("makina-core keymap\n\n");

    // --- both presets load, and the camera is reachable in both ------------------------------
    for (const char* json : {makina::mayaKeymapJson(), makina::blenderKeymapJson()}) {
        makina::Keymap km;
        std::string error;
        check(km.load(json, error), "a shipped preset does not load: " + error);

        // The camera is the one thing a preset must not leave unbound; without it the viewport
        // cannot be moved at all.
        for (const char* action : {"view.orbit", "view.pan", "view.dolly", "select.pick"}) {
            check(!km.bindingsFor(action).empty(),
                  std::string("preset '") + km.name() + "' does not bind " + action);
        }
    }

    // --- the presets really differ -------------------------------------------------------------
    {
        makina::Keymap maya, blender;
        std::string error;
        check(maya.load(makina::mayaKeymapJson(), error), error);
        check(blender.load(makina::blenderKeymapJson(), error), error);

        // Maya: alt plus the left button orbits, and a bare left click selects.
        check(maya.resolve(drag(makina::MouseButton::Left, makina::mods::kAlt)) == "view.orbit",
              "maya: alt+left drag does not orbit");
        check(maya.resolve(click(makina::MouseButton::Left)) == "select.pick",
              "maya: a bare left click does not select");

        // Blender: the middle button orbits with no modifier, and shift pans.
        check(blender.resolve(drag(makina::MouseButton::Middle)) == "view.orbit",
              "blender: middle drag does not orbit");
        check(blender.resolve(drag(makina::MouseButton::Middle, makina::mods::kShift)) ==
                  "view.pan",
              "blender: shift+middle drag does not pan");

        // The same gesture means different things in the two, which is why both exist.
        check(maya.resolve(drag(makina::MouseButton::Middle)) !=
                  blender.resolve(drag(makina::MouseButton::Middle)),
              "the two presets agree on the middle drag, so one of them is wrong");

        check(maya.resolve(key("F")) == "view.fitSelected", "maya: F does not fit");
        check(blender.resolve(key("NUMPADPERIOD")) == "view.fitSelected",
              "blender: numpad period does not fit");
    }

    // --- modifiers match exactly ---------------------------------------------------------------
    {
        makina::Keymap maya;
        std::string error;
        check(maya.load(makina::mayaKeymapJson(), error), error);

        // A subset match would make alt+left drag also fire the plain left binding, so orbiting
        // would select at the same time.
        check(maya.resolve(drag(makina::MouseButton::Left)).empty(),
              "a bare left drag resolved to something");
        check(maya.resolve(click(makina::MouseButton::Left, makina::mods::kAlt)).empty(),
              "alt+left click resolved to the unmodified binding");
    }

    // --- context is what lets X mean two things ------------------------------------------------
    {
        makina::Keymap blender;
        std::string error;
        check(blender.load(makina::blenderKeymapJson(), error), error);

        check(blender.resolve(key("X")) == "edit.delete",
              "blender: X outside a transform does not delete");
        check(blender.resolve(key("X", makina::mods::kNone, makina::KeyContext::Transform)) ==
                  "axis.x",
              "blender: X during a transform does not constrain to X");
    }

    // --- refusals -------------------------------------------------------------------------------
    {
        makina::Keymap km;
        std::string error;

        check(!km.load("{ not json", error), "malformed JSON was accepted");
        check(!km.load(R"({"name":"x"})", error), "a keymap with no bindings was accepted");

        check(!km.load(R"({"bindings":[{"action":"view.orbitt","button":"left","drag":true}]})",
                       error),
              "a misspelt action was accepted");
        check(error.find("view.orbitt") != std::string::npos,
              "the error does not name the unknown action: " + error);

        check(!km.load(R"({"bindings":[{"action":"view.orbit"}]})", error),
              "a binding with neither key nor button was accepted");

        check(!km.load(R"({"bindings":[
                  {"action":"edit.move","key":"G"},
                  {"action":"edit.scale","key":"G"}]})",
                       error),
              "two actions on the same key were accepted");
        check(error.find("edit.move") != std::string::npos &&
                  error.find("edit.scale") != std::string::npos,
              "the error does not name both colliding actions: " + error);

        // The same key in two different contexts is not a collision -- that is the whole point.
        check(km.load(R"({"bindings":[
                  {"action":"edit.delete","key":"X"},
                  {"action":"axis.x","key":"X","context":"transform"}]})",
                      error),
              "the same key in two contexts was refused: " + error);

        // A refused load leaves the previous map in place, so a bad edit does not strip the
        // application of its camera controls while someone is using it.
        makina::Keymap keep;
        check(keep.load(makina::mayaKeymapJson(), error), error);
        const std::size_t before = keep.size();
        check(!keep.load("{ broken", error), "a broken file was accepted");
        check(keep.size() == before, "a refused load emptied the keymap");
        check(keep.resolve(key("F")) == "view.fitSelected", "a refused load lost the bindings");
    }

    // --- what a preset leaves unbound is visible ------------------------------------------------
    {
        makina::Keymap maya;
        std::string error;
        check(maya.load(makina::mayaKeymapJson(), error), error);

        // Maya has no numpad axis views. That is a real difference from Blender, and the UI should
        // be able to say "not bound" rather than showing a key that does nothing.
        bool sawFront = false;
        for (const std::string& a : maya.unbound()) {
            if (a == "view.front") {
                sawFront = true;
            }
        }
        check(sawFront, "maya binds view.front, or unbound() does not report it");
    }

    if (failures == 0) {
        std::printf("\nthe keymap resolves and refuses (%d checks)\n", checks);
        return 0;
    }
    std::printf("\n%d of %d checks FAILED\n", failures, checks);
    return 1;
}
