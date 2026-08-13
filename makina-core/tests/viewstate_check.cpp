// The state the shell reads, checked without a window.
//
// ViewState.hpp declares two vocabularies -- the keys the shell may bind to, and the fields the
// items of each list carry -- and shell_audit.py checks the HTML against them. That only helps
// while the declarations are true, so this parses what the builders actually emit and compares.
// A declaration nothing verifies is a wish, and a wish is what the shell would then be audited
// against.
//
// The rest is the behaviour worth pinning: rows in tree order with the right depth, the mute flag
// reaching the row rather than removing it, property names coming from Op.hpp rather than from
// here, and a name with a quote in it not taking the whole outliner down with it.

#include <makina/SceneJson.hpp>
#include <makina/ViewState.hpp>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("    FAIL  %s\n", what.c_str());
        ++failures;
    }
}

/// A scene built here rather than loaded: the cases below need a mute, a nested transform and a
/// name that would break JSON, and no fixture has all three.
makina::Scene buildScene() {
    makina::Scene s;
    s.nodes.count = 4;

    s.nodes[0] = {};
    s.nodes[0].id = 1;
    s.nodes[0].op = static_cast<std::uint8_t>(makina::Op::SceneRoot);
    s.nodes[0].parent = makina::kNoParent;
    s.nodes[0].firstChild = 1;
    s.nodes[0].childCount = 1;
    s.nodes[0].nameId = 0;

    s.nodes[1] = {};
    s.nodes[1].id = 2;
    s.nodes[1].op = static_cast<std::uint8_t>(makina::Op::Difference);
    s.nodes[1].parent = 0;
    s.nodes[1].firstChild = 2;
    s.nodes[1].childCount = 2;
    s.nodes[1].nameId = 1;

    s.nodes[2] = {};
    s.nodes[2].id = 3;
    s.nodes[2].op = static_cast<std::uint8_t>(makina::Op::Sphere);
    s.nodes[2].parent = 1;
    s.nodes[2].firstChild = makina::kNoChild;
    s.nodes[2].nameId = 2;
    s.nodes[2].params[0] = 2.0f;

    s.nodes[3] = {};
    s.nodes[3].id = 4;
    s.nodes[3].op = static_cast<std::uint8_t>(makina::Op::Box);
    s.nodes[3].parent = 1;
    s.nodes[3].firstChild = makina::kNoChild;
    s.nodes[3].nameId = 3;
    s.nodes[3].flags |= makina::flags::kMuted;

    s.names.count = 4;
    const char* names[] = {"root", "body", "\"hex\" bolt", "cut"};
    for (int i = 0; i < 4; ++i) {
        std::snprintf(s.names[i].text, sizeof(s.names[i].text), "%s", names[i]);
    }
    return s;
}

}  // namespace

int main() {
    const makina::Scene s = buildScene();

    // --- the declarations describe what is emitted ------------------------------------------
    {
        makina::Selection all = {3};
        const makina::ViewEntries entries =
            makina::viewState(s, all, makina::ViewNumbers{4.0, 16.7, "Move Y: 3"});

        // Every declared key is produced, exactly once, and nothing else is.
        std::vector<std::string> produced;
        for (const auto& e : entries) {
            produced.push_back(e.first);
        }
        for (const std::string& key : makina::publishedKeys()) {
            int seen = 0;
            for (const std::string& p : produced) {
                if (p == key) {
                    ++seen;
                }
            }
            check(seen == 1, "publishedKeys() lists '" + key + "' and viewState() emits it " +
                                 std::to_string(seen) + " time(s)");
        }
        check(produced.size() == makina::publishedKeys().size(),
              "viewState() emits keys publishedKeys() does not list");

        // And the item fields. Parsed out of the JSON, so the declaration cannot drift from the
        // builder without this failing -- which is the whole reason the declaration is trusted.
        for (const auto& list : makina::publishedItemFields()) {
            std::string raw;
            for (const auto& e : entries) {
                if (e.first == list.first) {
                    raw = e.second;
                }
            }
            check(!raw.empty(), "publishedItemFields() names '" + list.first +
                                    "', which viewState() does not emit");
            if (raw.empty()) {
                continue;
            }
            const nlohmann::json parsed = nlohmann::json::parse(raw, nullptr, false);
            check(!parsed.is_discarded(), list.first + " is not valid JSON");
            check(parsed.is_array() && !parsed.empty(),
                  list.first + " has no items to check the fields of");
            if (parsed.is_discarded() || !parsed.is_array() || parsed.empty()) {
                continue;
            }
            for (const auto& item : parsed) {
                check(item.size() == list.second.size(),
                      list.first + ": an item carries " + std::to_string(item.size()) +
                          " fields and " + std::to_string(list.second.size()) + " are declared");
                for (const std::string& field : list.second) {
                    check(item.contains(field),
                          list.first + ": '" + field + "' is declared and not emitted");
                }
            }
        }
    }

    // --- the outliner ------------------------------------------------------------------------
    {
        const makina::Selection picked = {3};
        const nlohmann::json tree = nlohmann::json::parse(makina::treeJson(s, picked));

        check(tree.size() == s.nodes.count, "one row per node");
        check(tree[0]["indent"] == 0, "the root sits at the left edge");
        check(tree[1]["indent"] == 12, "a child is one step in");
        check(tree[2]["indent"] == 24, "a grandchild is two steps in");

        // The name is the design intent and survives being unprintable in JSON.
        check(tree[2]["name"] == "\"hex\" bolt", "a quoted name comes through whole");

        check(tree[2]["selected"] == true, "the picked node is lit");
        check(tree[3]["selected"] == false, "an unpicked node is not");

        // Muting is an edit, not a hide: the row stays and says so. If it vanished from here the
        // user could not select it back, and a mute you cannot undo is a delete under a kind name.
        check(tree[3]["muted"] == true, "a muted node still has a row, marked");
        check(tree[1]["muted"] == false, "an unmuted node is not marked");

        check(tree[2]["icon"] == "lucide/circle.svg", "Grasp3D had no sphere icon of its own");
        check(tree[3]["icon"] == "grasp3d/box16.gif", "the box icon is Grasp3D's");
        check(tree[1]["op"] == "Difference", "the row names its op");
    }

    // --- the property rows -------------------------------------------------------------------
    {
        const nlohmann::json fields = nlohmann::json::parse(makina::fieldsJson(s, {3}));
        check(fields.size() == 1, "a Sphere has one parameter");
        check(fields[0]["key"] == "radius", "and Op.hpp is what names it");
        check(fields[0]["value"] == "2", "a whole number reads as one, not as 2.000000");

        const nlohmann::json boxFields = nlohmann::json::parse(makina::fieldsJson(s, {4}));
        check(boxFields.size() == 6, "a Box has six");

        // The last one picked, not the first: that is the one whose name the header shows.
        const nlohmann::json two = nlohmann::json::parse(makina::fieldsJson(s, {4, 3}));
        check(two.size() == 1, "the panel follows the last node picked");

        check(makina::fieldsJson(s, {}) == "[]", "nothing selected, nothing to edit");
        check(makina::fieldsJson(s, {999}) == "[]", "an id that is not there is not a crash");
    }

    // --- the status bar ----------------------------------------------------------------------
    {
        const makina::ViewEntries e =
            makina::viewState(s, {3, 4}, makina::ViewNumbers{7.5, 16.666, "Move Y: 3  (typed)"});
        auto value = [&](const std::string& key) {
            for (const auto& kv : e) {
                if (kv.first == key) {
                    return kv.second;
                }
            }
            return std::string("<missing>");
        };
        check(value("view.status.nodes") == "4 nodes", "the node count is the tree's");
        check(value("view.status.live") == "Move Y: 3  (typed)",
              "Transform.hpp's status() goes through untouched");
        check(value("view.distance") == "7.5", "the distance is the camera's");
        check(value("view.status.selection").find("+1") != std::string::npos,
              "two selected reads as one name and a count");
    }

    if (failures != 0) {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("    the shell's state is what ViewState.hpp says it is\n");
    return 0;
}
