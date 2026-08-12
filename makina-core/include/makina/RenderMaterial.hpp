// The authoring material, turned into what a shader needs -- by POV-Ray's rules.
//
// `Material` (Scene.hpp) is Grasp3D's: six numbers, unchanged since the port, and the thing every
// .gsf file and every reference dump agrees on. It is deliberately not extended here. What is
// missing for shading is not more storage but the rest of POV's finish{}, and POV supplies those
// itself as defaults for whatever the exported file does not mention.
//
// So the conversion is: read Pov.hpp, see which terms it writes, and fill in POV's documented
// defaults for the rest. That is not a stylistic choice -- the .pov file this scene exports to is
// what the render is checked against, so any term where the two disagree is a difference the
// comparison would blame on the renderer.
//
//   written by Pov.hpp    ambient, specular, roughness, the pigment rgb, filter, emission
//   POV's own defaults    diffuse 0.6, brilliance 1.0, phong 0, metallic 0
//
// Keeping this derivation in one place is the point. Two renderers (the modeller and the engine)
// and one exporter all have to mean the same thing by a material.

#pragma once

#include "Scene.hpp"

#include <cstdint>
#include <type_traits>
#include <vector>

namespace makina {

/// Shader-side material. 56 bytes, matching the MkMaterial declared in scene_finish.hlsl.
struct GpuMaterial {
    float diffuseColor[3];  float alpha;
    float ambient;          float diffuse;
    float brilliance;       float specular;
    float roughness;        float phong;
    float phongSize;        float metallic;
    /// Which pigment paints this surface, or -1. A float because it rides in a float struct and
    /// the shader compares it against a count; the values are small integers either way.
    float emission;         float textureIndex;
};
static_assert(sizeof(GpuMaterial) == 56, "GpuMaterial must match the HLSL declaration");
static_assert(std::is_trivially_copyable_v<GpuMaterial>);

namespace povDefaults {
/// POV-Ray's finish defaults, for the terms Pov.hpp does not write.
inline constexpr float kDiffuse = 0.6f;
inline constexpr float kBrilliance = 1.0f;
inline constexpr float kPhong = 0.0f;
inline constexpr float kPhongSize = 40.0f;
inline constexpr float kMetallic = 0.0f;
/// POV's default_texture when an object names none: white, barely self-lit.
inline constexpr float kUntexturedAmbient = 0.1f;
}  // namespace povDefaults

/// What a surface with no material of its own wears.
///
/// POV's default texture rather than a grey chosen to look nice. An object with no texture in the
/// exported file is drawn by POV with these numbers, so drawing it differently here would put a
/// difference into every untextured scene -- which is most of the Grasp3D ones.
inline GpuMaterial defaultGpuMaterial() {
    GpuMaterial g{};
    g.diffuseColor[0] = 1.0f;
    g.diffuseColor[1] = 1.0f;
    g.diffuseColor[2] = 1.0f;
    g.alpha = 1.0f;
    g.ambient = povDefaults::kUntexturedAmbient;
    g.diffuse = povDefaults::kDiffuse;
    g.brilliance = povDefaults::kBrilliance;
    g.specular = 0.0f;
    g.roughness = 0.05f;
    g.phong = povDefaults::kPhong;
    g.phongSize = povDefaults::kPhongSize;
    g.metallic = povDefaults::kMetallic;
    g.emission = 0.0f;
    g.textureIndex = -1.0f;
    return g;
}

/// Converts one authoring material.
///
/// The two conversions that are not identity are Grasp3D's and are what its .pov files have always
/// meant, so they are spelled the same way here as in Pov.hpp:
///
///   roughness   1 - shininess/128. POV's roughness runs the opposite way to a shininess exponent.
///   filter      POV's rgbf carries transmission, so the stored alpha is inverted on export. The
///               shader wants opacity, so it keeps alpha as stored -- the inversion belongs to the
///               file format, not to the material.
inline GpuMaterial toGpuMaterial(const Material& m) {
    GpuMaterial g = defaultGpuMaterial();
    g.diffuseColor[0] = m.diffuse[0];
    g.diffuseColor[1] = m.diffuse[1];
    g.diffuseColor[2] = m.diffuse[2];
    g.alpha = m.alpha;
    g.ambient = m.ambient;
    g.specular = m.specular;
    g.roughness = 1.0f - m.shininess / 128.0f;
    // A shininess past 128 would give a negative roughness, and POV reads that as a division by a
    // negative number rather than as a mirror. Clamped to the smallest POV itself accepts.
    if (g.roughness < 1.0e-4f) {
        g.roughness = 1.0e-4f;
    }
    g.emission = m.emission;
    g.textureIndex = static_cast<float>(m.textureId);
    return g;
}

/// Every pigment in the scene, ready to upload beside the materials.
inline std::vector<Pigment> gpuPigments(const Scene& s) {
    std::vector<Pigment> out;
    out.reserve(s.pigments.count);
    for (std::uint32_t i = 0; i < s.pigments.count; ++i) {
        out.push_back(s.pigments[i]);
    }
    return out;
}

/// Every material in the scene, in index order, ready to upload.
///
/// Returned by value: the caller uploads it once per rebuild, not per frame, and a scene has at
/// most a few dozen.
inline std::vector<GpuMaterial> gpuMaterials(const Scene& s) {
    std::vector<GpuMaterial> out;
    out.reserve(s.materials.count);
    for (std::uint32_t i = 0; i < s.materials.count; ++i) {
        out.push_back(toGpuMaterial(s.materials[i]));
    }
    return out;
}

}  // namespace makina
