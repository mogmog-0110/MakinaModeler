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
        if (entry->op == Op::Rotate) {
            n.flags |= axisFromString(j.value("axis", std::string("X")));
        }
        if (entry->op == Op::Cone && j.value("open", false)) {
            n.flags |= flags::kConeOpen;
        }
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

inline void readRoot(Scene& s, const nlohmann::json& j) {
    if (Scene::kMaxNodes < 1) {
        throw SceneJsonError("scene capacity is zero");
    }
    s.nodes.count = 1;
    s.nodes[0].parent = kNoParent;
    fillNode(s, 0, j);
}

inline nlohmann::json writeNode(const Scene& s, std::uint16_t index) {
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
        if (op == Op::Rotate) {
            j["axis"] = axisToString(n.flags);
        }
        for (int i = 0; i < 12 && entry->keys[i] != nullptr; ++i) {
            j[entry->keys[i]] = n.params[i];
        }
        if (op == Op::Cone) {
            j["open"] = (n.flags & flags::kConeOpen) != 0;
        }
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

/// Parses scene JSON. Throws SceneJsonError on a version it does not understand, rather than
/// guessing at the meaning of fields that may have changed.
inline Scene parseScene(const std::string& text) {
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

    Scene s{};
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
            dst.textureId = -1;
            dst._pad = 0;
        }
    }

    if (!j.contains("root")) {
        throw SceneJsonError("scene has no \"root\"");
    }
    detail::readRoot(s, j["root"]);

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
    }
    j["materials"] = std::move(mats);

    return j.dump(2) + "\n";
}

}  // namespace makina
