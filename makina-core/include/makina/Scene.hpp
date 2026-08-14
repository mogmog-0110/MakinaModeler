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
    /// POV's finish{reflection}: how much of the mirrored ray is added on top. Zero is POV's
    /// default and the value every Grasp3D file means, so nothing that existed before changes.
    ///
    /// This sits in what used to be padding, so Material is the same 40 bytes and every scene
    /// that has ever been written still reads back the same size.
    float reflection;
    /// POV's interior{ior}: how much the ray bends on the way through a see-through surface.
    ///
    /// Zero means the same as one here. A Scene is zero-filled and every file written before this
    /// field existed says nothing about it, so reading an unset value as "no bending" is what keeps
    /// those scenes rendering as they did. POV does not accept an index below one either.
    float ior;
};
static_assert(sizeof(Material) == 44);
static_assert(std::is_trivially_copyable_v<Material>);

/// What a POV-Ray pigment pattern is, as far as this renderer goes.
///
/// Only the patterns that are pure arithmetic. POV's noise-driven ones -- marble, wood, granite,
/// bozo -- need POV's own permutation table to land on the same values, and a pattern that is
/// merely "noise that looks similar" cannot be compared against the oracle at all. Adding them
/// without that table would trade the one thing this renderer can claim for a texture that looks
/// about right.
enum class PigmentType : std::uint8_t {
    None     = 0,
    Checker  = 1,  ///< POV's checker: the parity of floor(x)+floor(y)+floor(z)
    Gradient = 2,  ///< the fractional part along an axis
    Radial   = 3,  ///< the angle about Y, which POV measures from -Z
};

/// A two-stop pigment. POV's color_map can hold more, and this is the shape of the common case.
///
/// Held as its own table rather than inline in Material because a pigment is a much bigger thing
/// than a color and most materials do not have one -- and Scene is the engine's GameMemory, so
/// every byte here is copied on every snapshot (History.hpp).
struct Pigment {
    std::uint8_t type;        ///< PigmentType
    std::uint8_t _pad0[3];
    float        a[3];        ///< the color at map position 0
    float        b[3];        ///< the color at map position 1
    /// Scale applied to the point before the pattern reads it, POV's `scale`.
    float        scale[3];
    float        translate[3];
    /// Which axis a Gradient runs along, as a unit vector. POV takes a vector, not a name.
    float        axis[3];
};
static_assert(sizeof(Pigment) == 64);
static_assert(std::is_trivially_copyable_v<Pigment>);

/// One light, in POV-Ray's terms.
///
/// The scene format did not carry lighting at all: every renderer made one up, and the POV export
/// took its lamp from a caller-supplied preamble. That was fine while the only comparison was a
/// silhouette. It stops being fine the moment the two renderers are asked to agree on colors,
/// because then the lamp is part of the answer.
///
/// Directional is not a POV concept -- POV has point lights and nothing else -- so a directional
/// light exports as a point light far enough away that the rays are parallel to within a pixel.
/// Keeping the distinction here rather than only in the exporter means the shader can skip the
/// per-pixel distance work for the common case.
struct Light {
    float position[3];   ///< where it is, or which way it points when directional
    float _pad0;
    float color[3];
    /// How wide the penumbra is, as a fraction of the distance marched.
    ///
    /// Zero is a hard shadow, which is the only kind POV casts from a point light and therefore
    /// the only kind the two renderers can be held to agreeing on. Anything above zero is this
    /// renderer doing something a ray tracer needs an area light and many samples for.
    float softness;
    /// POV's fade_distance and fade_power. Zero power means no falloff, which is POV's default.
    float fadeDistance;
    float fadePower;
    /// Whole words, not bytes. HLSL has no uint8, so a byte here is read as a uint there and every
    /// field after it moves -- which is not a crash, it is a light that quietly stops casting.
    std::uint32_t directional;   ///< 1 when `position` is a direction rather than a place
    std::uint32_t shadowless;
};
static_assert(sizeof(Light) == 48);
static_assert(std::is_trivially_copyable_v<Light>);

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
    /// One per material at most; a pigment belongs to exactly one.
    static constexpr std::size_t kMaxPigments = MaxMaterials;
    /// Grasp3D's scene format has eight light slots, and no Grasp3D file uses more.
    static constexpr std::size_t kMaxLights = 8;

    /// Monotonic. Never hands out an id twice, so a stale reference reads as dangling rather
    /// than as a different node (CSG_NODE.md 6.3).
    std::uint32_t nextId = 1;
    std::uint32_t _pad[3]{};

    FixedArray<CsgNode, MaxNodes>          nodes;
    FixedArray<Material, MaxMaterials>     materials;
    /// Referenced by Material::textureId, which was already the slot for exactly this.
    FixedArray<Pigment, MaxMaterials>      pigments;
    /// Empty means the renderer picks one, which is what every scene did before lights existed.
    FixedArray<Light, kMaxLights>          lights;
    /// Indexed in step with nodes: entry i is the name of node i.
    FixedArray<NameSlot<NameLen>, MaxNodes> names;

    [[nodiscard]] std::uint32_t nodeCount() const { return nodes.count; }
    [[nodiscard]] std::uint32_t materialCount() const { return materials.count; }

    [[nodiscard]] const char* nameOf(const CsgNode& n) const {
        return n.nameId < names.count ? names[n.nameId].text : "";
    }
};

/// 4096 nodes: sized for import, not for hand editing. The Grasp3D corpus tops out at 87 nodes
/// (pettobotoru.gsf, SCENE_FORMAT.md 5), but a real internet scene arrives far bigger -- the
/// measured example is scene.pov, whose ice macros expand to 2285 nodes. 4096 is the next power
/// of two above that measurement. Materials stay at 64: the same scene uses 7.
///
/// This makes a Scene about 390 KB, which changes what it is allowed to be: a heap or static
/// citizen, never a stack local. Two copies in one call chain overflow a default 1 MB Windows
/// stack, so every executable here links with /STACK:16MB -- measured, not guessed: the import
/// pipeline holds 3-5 live copies and needed between 2 and 4 MB at the 8192-node trial size.
///
/// The engine is not bound to this number. SceneStorage is a template precisely so a rewind
/// ring that snapshots per frame can instantiate a smaller storage and convert at the border;
/// this alias is the authoring and interchange capacity, not a promise about GameMemory.
using Scene = SceneStorage<4096, 64, 32>;

/// The instantiation a per-frame rewind ring snapshots (about 28 KB; CSG_NODE.md 2.1). Held to
/// the plan's 32 MB ring budget by tests/roundtrip.cpp. 256 nodes covers the largest hand-made
/// scene by a wide margin: pettobotoru.gsf, the biggest in the Grasp3D corpus, is 87 nodes.
/// A scene must fit this to ride in GameMemory; an imported giant stays an asset the engine
/// references instead of snapshotting.
using RewindScene = SceneStorage<256, 64, 32>;

static_assert(std::is_trivially_copyable_v<Scene>,
              "Scene must be memcpy-able or the engine's rewind and replay cannot hold it");

}  // namespace makina
