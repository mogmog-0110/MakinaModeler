// The authoring representation (D-01): the tree the user edits, not the program the GPU walks.
//
// Everything here is a flat POD with fixed capacity, so a whole scene is one trivially copyable
// value. That is what lets MitiruEngine hold it as GameMemory and get rewind, replay, structured
// AI observation and what-if from a single memcpy (see the engine's docs/FLAT_POD.md).
//
// Nothing in this header knows about the engine, a GPU, or a graphics API. Keeping it that way
// is what makes the whole model testable without a device.

#pragma once

#include "FixedArray.hpp"
#include "Op.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace makina {

/// A node of the authoring tree. 64 bytes; see CSG_NODE.md for why each field is sized this way.
struct CsgNode {
    /// Stable across insertion and deletion, unlike the array index. Never reused.
    std::uint32_t id;
    std::uint8_t  op;
    std::uint8_t  materialId;   ///< index into Scene::materials, or kNoMaterial
    std::uint16_t flags;        ///< Rotate axis, Cone open — see namespace flags
    std::uint16_t firstChild;   ///< index into Scene::nodes
    std::uint16_t childCount;   ///< n-ary; Difference is children[0] minus the rest
    std::uint16_t nameId;       ///< index into Scene::names; design intent, never dropped
    /// Index of the parent, or kNoParent at the root. Evaluating a subtree needs the ancestor
    /// transforms, and walking up is the only way to get them from a flat array. This field costs
    /// nothing: it occupies padding the 64-byte layout already had.
    std::uint16_t parent;
    float         params[12];   ///< meaning depends on op; longest is Triangle at 10
};
static_assert(sizeof(CsgNode) == 64, "CsgNode must stay 64 bytes; the size feeds the rewind budget");
static_assert(std::is_trivially_copyable_v<CsgNode>);

constexpr std::uint8_t kNoMaterial = 0xFF;
constexpr std::uint16_t kNoName = 0xFFFF;
constexpr std::uint16_t kNoChild = 0xFFFF;
constexpr std::uint16_t kNoParent = 0xFFFF;

struct Material {
    float        diffuse[3];   ///< 0..1
    float        alpha;
    float        ambient;
    float        specular;
    float        shininess;
    float        emission;
    std::int32_t textureId;    ///< index into a texture table, or -1
    std::int32_t _pad;
};
static_assert(sizeof(Material) == 40);
static_assert(std::is_trivially_copyable_v<Material>);

/// A node's name, boxed so it can live in a counted collection the engine's reflection understands.
template <std::size_t N>
struct NameSlot {
    char text[N]{};
};

/// Fixed-capacity scene. The engine typedefs one instantiation as its GameMemory.
///
/// Every collection carries its own count rather than sharing one at the top, so each is reported
/// at its live length rather than its capacity (see FixedArray.hpp).
template <std::size_t MaxNodes, std::size_t MaxMaterials, std::size_t NameLen>
struct SceneStorage {
    static constexpr std::size_t kMaxNodes = MaxNodes;
    static constexpr std::size_t kMaxMaterials = MaxMaterials;
    static constexpr std::size_t kNameLen = NameLen;

    /// Monotonic. Never hands out an id twice, so a stale reference reads as dangling rather
    /// than as a different node (CSG_NODE.md 6.3).
    std::uint32_t nextId = 1;
    std::uint32_t _pad[3]{};

    FixedArray<CsgNode, MaxNodes>          nodes;
    FixedArray<Material, MaxMaterials>     materials;
    /// Indexed in step with nodes: entry i is the name of node i.
    FixedArray<NameSlot<NameLen>, MaxNodes> names;

    [[nodiscard]] std::uint32_t nodeCount() const { return nodes.count; }
    [[nodiscard]] std::uint32_t materialCount() const { return materials.count; }

    [[nodiscard]] const char* nameOf(const CsgNode& n) const {
        return n.nameId < names.count ? names[n.nameId].text : "";
    }
};

/// 256 nodes covers the largest real scene by a wide margin: pettobotoru.gsf, the biggest in the
/// Grasp3D corpus, is 87 nodes (SCENE_FORMAT.md 5).
using Scene = SceneStorage<256, 64, 32>;

static_assert(std::is_trivially_copyable_v<Scene>,
              "Scene must be memcpy-able or the engine's rewind and replay cannot hold it");

}  // namespace makina
