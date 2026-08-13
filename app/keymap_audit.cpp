// Every action the keymap knows must be one the viewport carries out.
//
// Keymap.hpp says its list is "actions this build knows how to carry out", and checks a loaded
// file against it so a typo is caught at load rather than as a key that does nothing. That only
// works while the list is true. It was not: `select.add` sat in it, both presets bound Shift and
// a click to it, and the viewport never mentioned it -- so Shift-clicking did nothing whatsoever,
// not even the plain pick, because the event resolved to an action no branch claimed.
//
// A key that does nothing is the quietest kind of wrong. Nothing crashes, nothing logs, and the
// only way to notice is to try it and wonder whether you pressed the right thing.
//
// So this reads the viewport's source and asks, for each known action, whether its name appears
// there outside a comment. That is coarser than tracing the dispatch, and it is honest about what
// it proves: the name reaches the code. It cannot catch a branch that names an action and then
// does nothing useful, and it is not meant to -- viewport-check.bat drives the keys and compares
// the tree that comes out for that half.

#include <makina/Keymap.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

/// The source with its comments blanked, so a name that only appears in prose does not count.
///
/// Line comments and block comments only; a `//` inside a string literal would be blanked too.
/// That would make the check stricter rather than looser, which is the safe direction for a
/// checker to be wrong in.
std::string withoutComments(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') {
                ++i;
            }
            out += '\n';
            continue;
        }
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) {
                ++i;
            }
            ++i;
            continue;
        }
        out += src[i];
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: keymap_audit <viewport source> [<more sources>]\n");
        return 2;
    }

    std::string code;
    for (int i = 1; i < argc; ++i) {
        std::ifstream in(argv[i], std::ios::binary);
        if (!in) {
            std::printf("    FAIL  could not open '%s'\n", argv[i]);
            return 1;
        }
        std::ostringstream buf;
        buf << in.rdbuf();
        code += withoutComments(buf.str());
    }

    std::printf("every action the keymap knows, carried out by the viewport\n\n");

    int missing = 0;
    for (const std::string& action : makina::knownActions()) {
        if (code.find("\"" + action + "\"") == std::string::npos) {
            std::printf("    FAIL  '%s' is a known action and the viewport never names it\n",
                        action.c_str());
            ++missing;
        }
    }

    // The other direction, twice over.
    //
    // A preset that binds something the build does not know is refused by Keymap::load, so that
    // one only needs the built-in presets run through it -- they never come from a file and so
    // never meet that check.
    //
    // And an action nothing can reach is as dead as an action nothing implements. select.clear
    // was exactly that for a while: in the list, handled by the viewport, and bound to nothing in
    // either preset, so there was no way to press it. Checking one direction only is what let it
    // sit there.
    //
    // At least one preset, not both. The two are meant to differ -- Maya has no key for the axis
    // views or the orthographic toggle and PLAN.md's own table marks that column with a dash, so
    // demanding both would be demanding that this project invent bindings Maya users do not have.
    std::vector<makina::Keymap> presets;
    for (const char* json : {makina::mayaKeymapJson(), makina::blenderKeymapJson()}) {
        makina::Keymap map;
        std::string error;
        if (!map.load(json, error)) {
            std::printf("    FAIL  a built-in keymap does not load: %s\n", error.c_str());
            ++missing;
            continue;
        }
        presets.push_back(std::move(map));
    }
    for (const std::string& action : makina::knownActions()) {
        bool reachable = false;
        for (const makina::Keymap& map : presets) {
            if (!map.bindingsFor(action).empty()) {
                reachable = true;
            }
        }
        if (!reachable) {
            std::printf("    FAIL  no preset binds anything to '%s'\n", action.c_str());
            ++missing;
        }
    }

    if (missing != 0) {
        std::printf("\n%d action(s) promised and not reachable\n", missing);
        return 1;
    }
    std::printf("    %zu actions, every one of them reaches the viewport\n",
                makina::knownActions().size());
    return 0;
}
