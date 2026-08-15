// Scene JSON <-> Scene (docs/SCENE_FORMAT.md).
//
// The JSON is the interchange and review format: nested children, named parameters, so a change
// reads as a diff in a pull request. The flat arrays are the memory layout. This file is the only
// place that knows both.
//
// Conversion is one-way with respect to Grasp3D: .gsf stays authoritative for Grasp3D, JSON for
// Makina, so nothing here tries to reproduce a .gsf.

#pragma once

#include "Scene.hpp"

#include <nlohmann/json.hpp>

#include <cstring>
#include <stdexcept>
#include <string>

namespace makina {

class SceneJsonError : public std::runtime_error {
public:
    explicit SceneJsonError(const std::string& what) : std::runtime_error(what) {}
};

constexpr int kSceneFormatVersion = 1;

namespace detail {

inline void setName(Scene& s, std::uint16_t slot, const std::string& name) {
    if (slot >= Scene::kMaxNodes) {
        return;
    }
    if (s.names.count <= slot) {
        s.names.count = static_cast<std::uint32_t>(slot) + 1;
    }
    char* dst = s.names[slot].text;
    const std::size_t n = name.size() < Scene::kNameLen - 1 ? name.size() : Scene::kNameLen - 1;
    std::memcpy(dst, name.data(), n);
    dst[n] = '\0';
}

inline std::uint16_t axisFromString(const std::string& axis) {
    if (axis == "Y") return flags::kAxisY;
    if (axis == "Z") return flags::kAxisZ;
    return flags::kAxisX;
}

inline const char* axisToString(std::uint16_t f) {
    switch (f & flags::kAxisMask) {
        case flags::kAxisY: return "Y";
        case flags::kAxisZ: return "Z";
        default:            return "X";
    }
}

// Fills a slot that has already been reserved.
//
// firstChild/childCount can only work if siblings are adjacent, and a plain depth-first walk does
// not give that: it lays out child 0, then all of child 0's descendants, then child 1. So every
// node reserves a contiguous block for all of its children up front and only then recurses into
// them. A parent still always precedes its children, which the flattener relies on.
inline void fillNode(Scene& s, std::uint16_t index, const nlohmann::json& j) {
    CsgNode& n = s.nodes[index];

    const std::string opName = j.value("op", "Unsupported");
    const OpEntry* entry = findOp(opName.c_str());
    if (entry == nullptr) {
        // An op this build does not know is preserved rather than dropped, so the node count and
        // the tree shape survive. Guessing at its meaning would be worse than admitting ignorance.
        n.op = static_cast<std::uint8_t>(Op::Unsupported);
    } else {
        n.op = static_cast<std::uint8_t>(entry->op);
    }

    n.id = j.value("id", 0u);
    n.materialId = static_cast<std::uint8_t>(j.value("material", int(kNoMaterial)));
    n.nameId = index;
    n.flags = 0;
    detail::setName(s, index, j.value("name", std::string{}));

    if (entry != nullptr) {
        for (int i = 0; i < 12 && entry->keys[i] != nullptr; ++i) {
            n.params[i] = j.value(entry->keys[i], 0.0f);
        }
        if (entry->op == Op::Rotate || isWarp(entry->op)) {
            n.flags |= axisFromString(j.value("axis", std::string("X")));
        }
        if (entry->op == Op::Cone && j.value("open", false)) {
            n.flags |= flags::kConeOpen;
        }
        if (entry->op == Op::SphereSweep &&
            j.value("spline", std::string("linear")) == "bspline") {
            n.flags |= flags::kSweepBspline;
        }
    }
    if (j.value("muted", false)) {
        // Read for every op, not just the ones with their own flags: any node can be taken out of
        // the solid, and a Translate carrying a subtree is the most useful one to take out.
        n.flags |= flags::kMuted;
    }

    n.firstChild = kNoChild;
    n.childCount = 0;

    if (!j.contains("children") || !j["children"].is_array() || j["children"].empty()) {
        return;
    }

    const auto& kids = j["children"];
    const std::size_t count = kids.size();
    if (s.nodes.count + count > Scene::kMaxNodes) {
        throw SceneJsonError("scene exceeds the " + std::to_string(Scene::kMaxNodes) +
                             " node limit; raise SceneStorage's capacity or split the model");
    }

    const std::uint16_t first = static_cast<std::uint16_t>(s.nodes.count);
    s.nodes.count += static_cast<std::uint32_t>(count);
    n.firstChild = first;
    n.childCount = static_cast<std::uint16_t>(count);

    for (std::size_t i = 0; i < count; ++i) {
        const std::uint16_t child = static_cast<std::uint16_t>(first + i);
        s.nodes[child].parent = index;
        fillNode(s, child, kids[i]);
    }
}

/// Reads one pigment, defaulting everything a POV pigment block would default.
///
/// An unknown pattern name is refused rather than ignored. A file that says `marble` and gets a
/// flat color looks like a renderer that cannot do marble, when what happened is that nobody
/// implemented it -- and the two call for different reactions.
inline Pigment readPigment(const nlohmann::json& j) {
    Pigment p{};
    const std::string type = j.value("type", std::string("checker"));
    if (type == "checker") {
        p.type = static_cast<std::uint8_t>(PigmentType::Checker);
    } else if (type == "gradient") {
        p.type = static_cast<std::uint8_t>(PigmentType::Gradient);
    } else if (type == "radial") {
        p.type = static_cast<std::uint8_t>(PigmentType::Radial);
    } else {
        throw SceneJsonError("pigment type '" + type + "' is not one this renderer has; it takes "
                             "checker, gradient or radial");
    }

    // The map. "stops" is the general form; "colorA"/"colorB" is what every file written before
    // maps grew says, and reads as two stops at 0 and 1 -- the same picture it always was.
    if (j.contains("stops") && j["stops"].is_array()) {
        const auto& stops = j["stops"];
        if (stops.size() < 2 || stops.size() > static_cast<std::size_t>(Pigment::kMaxStops)) {
            throw SceneJsonError("a pigment needs 2 to " + std::to_string(Pigment::kMaxStops) +
                                 " stops; this one has " + std::to_string(stops.size()));
        }
        int n = 0;
        for (const auto& st : stops) {
            if (!st.is_array() || st.size() != 2 || !st[1].is_array() || st[1].size() != 3) {
                throw SceneJsonError("a pigment stop is [position, [r, g, b]]");
            }
            p.stop[n][3] = st[0].get<float>();
            for (int c = 0; c < 3; ++c) {
                // 0-255 like Material::diffuse, so one file does not mix two conventions.
                p.stop[n][c] = st[1][c].get<float>() / 255.0f;
            }
            if (n > 0 && p.stop[n][3] < p.stop[n - 1][3]) {
                throw SceneJsonError("pigment stops must be in ascending position");
            }
            ++n;
        }
        p.stopCount = static_cast<std::uint8_t>(n);
    } else {
        const auto rgb = [&j](const char* key, float* dst, float fallback) {
            if (j.contains(key) && j[key].is_array() && j[key].size() == 3) {
                for (int c = 0; c < 3; ++c) {
                    dst[c] = j[key][c].get<float>() / 255.0f;
                }
            } else {
                dst[0] = dst[1] = dst[2] = fallback;
            }
        };
        rgb("colorA", p.stop[0], 1.0f);
        rgb("colorB", p.stop[1], 0.0f);
        p.stop[0][3] = 0.0f;
        p.stop[1][3] = 1.0f;
        p.stopCount = 2;
    }

    const auto vec3 = [&j](const char* key, float (&dst)[3], float fx, float fy, float fz) {
        if (j.contains(key) && j[key].is_array() && j[key].size() == 3) {
            for (int c = 0; c < 3; ++c) {
                dst[c] = j[key][c].get<float>();
            }
        } else {
            dst[0] = fx;
            dst[1] = fy;
            dst[2] = fz;
        }
    };
    vec3("scale", p.scale, 1.0f, 1.0f, 1.0f);
    vec3("translate", p.translate, 0.0f, 0.0f, 0.0f);
    vec3("axis", p.axis, 1.0f, 0.0f, 0.0f);

    // A zero scale divides by itself in the shader. POV rejects it too.
    for (int c = 0; c < 3; ++c) {
        if (p.scale[c] == 0.0f) {
            throw SceneJsonError("a pigment scale of zero has no meaning; POV rejects it as well");
        }
    }
    return p;
}

/// The inverse of readPigment, field for field, so a scene survives a round trip.
inline nlohmann::ordered_json writePigment(const Pigment& p) {
    const char* type = "checker";
    switch (static_cast<PigmentType>(p.type)) {
        case PigmentType::Gradient: type = "gradient"; break;
        case PigmentType::Radial:   type = "radial"; break;
        default:                    type = "checker"; break;
    }
    const auto rgb = [](const float* c) {
        return nlohmann::ordered_json{int(c[0] * 255.0f + 0.5f), int(c[1] * 255.0f + 0.5f),
                                      int(c[2] * 255.0f + 0.5f)};
    };
    const auto vec3 = [](const float (&c)[3]) {
        return nlohmann::ordered_json{c[0], c[1], c[2]};
    };
    nlohmann::ordered_json out{{"type", type}};
    // Two stops at 0 and 1 are written the way they always were, so every existing scene round
    // trips byte for byte; anything else takes the general form.
    const bool plain = p.stopCount == 2 && p.stop[0][3] == 0.0f && p.stop[1][3] == 1.0f;
    if (plain) {
        out["colorA"] = rgb(p.stop[0]);
        out["colorB"] = rgb(p.stop[1]);
    } else {
        nlohmann::ordered_json stops = nlohmann::ordered_json::array();
        for (int i = 0; i < p.stopCount; ++i) {
            stops.push_back(nlohmann::ordered_json{p.stop[i][3], rgb(p.stop[i])});
        }
        out["stops"] = std::move(stops);
    }
    out["scale"] = vec3(p.scale);
    out["translate"] = vec3(p.translate);
    out["axis"] = vec3(p.axis);
    return out;
}

/// Reads one light, defaulting what POV defaults.
inline Light readLight(const nlohmann::json& j) {
    Light l{};
    l.directional = j.value("directional", false) ? 1u : 0u;
    l.shadowless = j.value("shadowless", false) ? 1u : 0u;

    const auto vec3 = [&j](const char* key, float (&dst)[3], float fx, float fy, float fz) {
        if (j.contains(key) && j[key].is_array() && j[key].size() == 3) {
            for (int c = 0; c < 3; ++c) {
                dst[c] = j[key][c].get<float>();
            }
        } else {
            dst[0] = fx;
            dst[1] = fy;
            dst[2] = fz;
        }
    };
    vec3("position", l.position, 0.0f, 10.0f, 0.0f);
    if (j.contains("color") && j["color"].is_array() && j["color"].size() == 3) {
        for (int c = 0; c < 3; ++c) {
            // 0-255 like every other color in this format.
            l.color[c] = j["color"][c].get<float>() / 255.0f;
        }
    } else {
        l.color[0] = l.color[1] = l.color[2] = 1.0f;
    }

    l.softness = j.value("softness", 0.0f);
    l.fadeDistance = j.value("fadeDistance", 0.0f);
    l.fadePower = j.value("fadePower", 0.0f);

    if (l.directional != 0u) {
        // A direction of zero length has no meaning and would divide by itself in the shader.
        const float len = std::sqrt(l.position[0] * l.position[0] + l.position[1] * l.position[1] +
                                    l.position[2] * l.position[2]);
        if (len < 1e-6f) {
            throw SceneJsonError("a directional light needs a direction; this one has none");
        }
    }
    return l;
}

inline nlohmann::ordered_json writeLight(const Light& l) {
    return nlohmann::ordered_json{
        {"directional", l.directional != 0u},
        {"shadowless", l.shadowless != 0u},
        {"position", {l.position[0], l.position[1], l.position[2]}},
        {"color", {int(l.color[0] * 255.0f + 0.5f), int(l.color[1] * 255.0f + 0.5f),
                   int(l.color[2] * 255.0f + 0.5f)}},
        {"softness", l.softness},
        {"fadeDistance", l.fadeDistance},
        {"fadePower", l.fadePower}};
}

inline void readRoot(Scene& s, const nlohmann::json& j) {
    if (Scene::kMaxNodes < 1) {
        throw SceneJsonError("scene capacity is zero");
    }
    s.nodes.count = 1;
    s.nodes[0].parent = kNoParent;
    fillNode(s, 0, j);
}

/// Returns ordered_json, not json.
///
/// The whole node is built into an ordered_json and then handed back, so declaring the return type
/// as plain json converted it and dropped the key order on the way out -- every node came back
/// alphabetised, which is not what a file a person reads should look like.
///
/// GCC is what caught it: pushing a json into an ordered_json array is an ambiguous overload
/// there, while MSVC picked one and carried on. The ambiguity was the symptom; the conversion was
/// the bug.
inline nlohmann::ordered_json writeNode(const Scene& s, std::uint16_t index) {
    const CsgNode& n = s.nodes[index];
    const Op op = static_cast<Op>(n.op);
    const OpEntry* entry = findOp(op);

    nlohmann::ordered_json j;
    j["id"] = n.id;
    j["op"] = opName(op);
    j["name"] = s.names[n.nameId].text;
    if (n.materialId != kNoMaterial) {
        j["material"] = n.materialId;
    }

    if (entry != nullptr) {
        if (op == Op::Rotate || isWarp(op)) {
            j["axis"] = axisToString(n.flags);
        }
        for (int i = 0; i < 12 && entry->keys[i] != nullptr; ++i) {
            j[entry->keys[i]] = n.params[i];
        }
        if (op == Op::Cone) {
            j["open"] = (n.flags & flags::kConeOpen) != 0;
        }
        if (op == Op::SphereSweep) {
            j["spline"] = (n.flags & flags::kSweepBspline) != 0 ? "bspline" : "linear";
        }
    }
    if ((n.flags & flags::kMuted) != 0) {
        // Written only when set, so every scene that has ever been saved still reads back byte for
        // byte -- the same rule reflection and ior follow.
        j["muted"] = true;
    }

    if (n.childCount > 0) {
        nlohmann::ordered_json kids = nlohmann::ordered_json::array();
        for (std::uint16_t i = 0; i < n.childCount; ++i) {
            kids.push_back(writeNode(s, static_cast<std::uint16_t>(n.firstChild + i)));
        }
        j["children"] = std::move(kids);
    }

    return j;
}

}  // namespace detail

/// Parses scene JSON into `s`, replacing whatever it held. Throws SceneJsonError on a version
/// it does not understand, rather than guessing at the meaning of fields that may have changed.
///
/// The in-place form is the primary one because a Scene is ~390 KB (Scene.hpp): returning it
/// by value puts a whole copy on the caller's frame, and two of those in one call chain are past
/// a default 1 MB thread stack. Callers that own their Scene on the heap parse straight into it.
inline void parseSceneInto(Scene& s, const std::string& text) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception& e) {
        throw SceneJsonError(std::string("scene JSON is not well formed: ") + e.what());
    }

    const std::string format = j.value("format", std::string{});
    if (format != "makina-scene") {
        throw SceneJsonError("not a makina scene: \"format\" was \"" + format + "\"");
    }

    const int version = j.value("version", 0);
    if (version > kSceneFormatVersion) {
        throw SceneJsonError("scene format version " + std::to_string(version) +
                             " is newer than this build understands (" +
                             std::to_string(kSceneFormatVersion) + ")");
    }

    s = Scene{};
    s.nextId = j.value("nextId", 1u);

    if (j.contains("materials") && j["materials"].is_array()) {
        for (const auto& m : j["materials"]) {
            if (s.materials.count >= Scene::kMaxMaterials) {
                throw SceneJsonError("scene exceeds the " + std::to_string(Scene::kMaxMaterials) +
                                     " material limit");
            }
            Material& dst = s.materials[s.materials.count++];
            // Grasp3D stores colors as 0-255 integers; the core works in 0..1.
            if (m.contains("diffuse") && m["diffuse"].is_array() && m["diffuse"].size() == 3) {
                for (int c = 0; c < 3; ++c) {
                    dst.diffuse[c] = m["diffuse"][c].get<float>() / 255.0f;
                }
            }
            dst.alpha = m.value("alpha", 1.0f);
            dst.ambient = m.value("ambient", 0.0f);
            dst.specular = m.value("specular", 0.0f);
            dst.shininess = m.value("shininess", 0.0f);
            dst.emission = m.value("emission", 0.0f);
            // A pigment, when the material names one. Read here rather than from a separate table
            // so a material and its pattern stay one thing in the file -- which is how POV writes
            // it too, and how anyone editing the JSON by hand would expect to find it.
            dst.textureId = -1;
            dst.reflection = m.value("reflection", 0.0f);
            dst.ior = m.value("ior", 1.0f);
            dst.brilliance = m.value("brilliance", 1.0f);
            dst.finishDiffuse = m.value("finishDiffuse", 0.6f);
            if (m.contains("pigment") && m["pigment"].is_object()) {
                if (s.pigments.count >= Scene::kMaxPigments) {
                    throw SceneJsonError("scene exceeds the " +
                                         std::to_string(Scene::kMaxPigments) + " pigment limit");
                }
                dst.textureId = static_cast<std::int32_t>(s.pigments.count);
                s.pigments[s.pigments.count++] = detail::readPigment(m["pigment"]);
            }
        }
    }

    if (j.contains("lights") && j["lights"].is_array()) {
        for (const auto& l : j["lights"]) {
            if (s.lights.count >= Scene::kMaxLights) {
                throw SceneJsonError("scene exceeds the " + std::to_string(Scene::kMaxLights) +
                                     " light limit");
            }
            s.lights[s.lights.count++] = detail::readLight(l);
        }
    }

    if (!j.contains("root")) {
        throw SceneJsonError("scene has no \"root\"");
    }
    detail::readRoot(s, j["root"]);
}

/// By-value convenience for callers whose stack can afford one Scene: tools and tests linked
/// with a widened stack. Anything holding a Scene as a member should use parseSceneInto.
inline Scene parseScene(const std::string& text) {
    Scene s{};
    parseSceneInto(s, text);
    return s;
}

inline std::string writeScene(const Scene& s, const std::string& sourceFile = {}) {
    nlohmann::ordered_json j;
    j["format"] = "makina-scene";
    j["version"] = kSceneFormatVersion;
    j["units"] = "meter";
    j["coordinates"] = "right-handed-y-up";
    j["angles"] = "degrees";
    if (!sourceFile.empty()) {
        j["source"] = {{"tool", "makina-core"}, {"file", sourceFile}};
    }
    j["nextId"] = s.nextId;

    if (s.nodes.count > 0) {
        j["root"] = detail::writeNode(s, 0);
    }

    nlohmann::ordered_json mats = nlohmann::ordered_json::array();
    for (std::uint32_t i = 0; i < s.materials.count; ++i) {
        const Material& m = s.materials[i];
        mats.push_back({
            {"id", i},
            {"diffuse", {int(m.diffuse[0] * 255.0f + 0.5f),
                         int(m.diffuse[1] * 255.0f + 0.5f),
                         int(m.diffuse[2] * 255.0f + 0.5f)}},
            {"alpha", m.alpha},
            {"ambient", m.ambient},
            {"specular", m.specular},
            {"shininess", m.shininess},
            {"emission", m.emission},
            {"texture", nullptr},
        });
        if (m.ior > 1.0f) {
            // The same rule as reflection below: written only when it says something, so a scene
            // that predates refraction still round trips byte for byte.
            mats.back()["ior"] = m.ior;
        }
        if (m.reflection != 0.0f) {
            // Written only when set. A file that never mentioned reflection round trips unchanged
            // rather than gaining a zero, which keeps every existing scene byte for byte.
            mats.back()["reflection"] = m.reflection;
        }
        if (m.brilliance != 0.0f && m.brilliance != 1.0f) {
            // Same rule: one is POV's default and what an unset field means, so only another value
            // is worth a line.
            mats.back()["brilliance"] = m.brilliance;
        }
        if (m.finishDiffuse != 0.0f && m.finishDiffuse != 0.6f) {
            mats.back()["finishDiffuse"] = m.finishDiffuse;
        }
        if (m.textureId >= 0 && static_cast<std::uint32_t>(m.textureId) < s.pigments.count) {
            mats.back()["pigment"] = detail::writePigment(s.pigments[m.textureId]);
        }
    }
    j["materials"] = std::move(mats);

    if (s.lights.count > 0) {
        // Omitted entirely when there are none, so a file that never mentioned lighting round
        // trips unchanged rather than gaining an empty array.
        nlohmann::ordered_json lights = nlohmann::ordered_json::array();
        for (std::uint32_t i = 0; i < s.lights.count; ++i) {
            lights.push_back(detail::writeLight(s.lights[i]));
        }
        j["lights"] = std::move(lights);
    }

    return j.dump(2) + "\n";
}

}  // namespace makina
