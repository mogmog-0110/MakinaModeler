// POV-Ray's finish{}, as the renderer computes it.
//
// Why bother matching POV-Ray rather than writing whatever looks good: the same tree is exported
// to a .pov file and ray traced, and that render is what this one is checked against. Until now
// the check could only compare **silhouettes**, because the two used different lighting and the
// colours had nothing to say to each other. Sharing the shading model is what raises the
// comparison from "the outline agrees" to "the pixels agree".
//
// The model is POV's, deliberately, including the parts a modern renderer would do differently:
//
//   ambient       flat self-illumination, not an occlusion term. POV adds it unconditionally.
//   diffuse       Lambert, scaled by `brilliance` as an exponent on N.L rather than on the result.
//   specular      Blinn-Phong with `roughness`; POV's roughness is 1/shininess, not a GGX alpha.
//   phong         a second, separate highlight with its own size. POV really does have both.
//   metallic      tints the highlight by the pigment instead of leaving it white.
//
// Not physically based and not trying to be. Matching an oracle is worth more here than being
// right by a standard the oracle does not follow.

#ifndef MAKINA_SCENE_FINISH_HLSL
#define MAKINA_SCENE_FINISH_HLSL

/// Must match makina::GpuMaterial byte for byte (RenderMaterial.hpp asserts the size).
struct MkMaterial {
    float3 diffuseColor;  float alpha;
    float  ambient;       float diffuse;
    float  brilliance;    float specular;
    float  roughness;     float phong;
    float  phongSize;     float metallic;
    float  emission;      float textureIndex;
    float  reflection;    float ior;
    float2 _pad;
};

StructuredBuffer<MkMaterial> gMaterials : register(t1);

/// What a surface with no material of its own wears.
///
/// POV's default texture, not a grey picked to look nice. Most Grasp3D scenes are untextured, so
/// this is the common case, and it is exactly the case where the two renderers would otherwise
/// disagree everywhere. Kept in step with makina::defaultGpuMaterial (RenderMaterial.hpp).
MkMaterial mkDefaultMaterial() {
    MkMaterial m;
    m.diffuseColor = float3(1.0, 1.0, 1.0);
    m.alpha = 1.0;
    m.ambient = 0.1;
    m.diffuse = 0.6;
    m.brilliance = 1.0;
    m.specular = 0.0;
    m.roughness = 0.05;
    m.phong = 0.0;
    m.phongSize = 40.0;
    m.metallic = 0.0;
    m.emission = 0.0;
    m.textureIndex = -1.0;
    m.reflection = 0.0;
    m.ior = 1.0;
    m._pad = float2(0, 0);
    return m;
}

/// 255 means "no material", which is what an untextured surface carries all the way from
/// Flatten.hpp. Anything past the end of the buffer is treated the same rather than clamped -- a
/// clamp would silently paint the surface with material 0 and look like an authoring choice.
MkMaterial mkMaterialAt(float index, uint count) {
    const uint i = (uint)(index + 0.5);
    if (i >= count) {
        return mkDefaultMaterial();
    }
    return gMaterials[i];
}

// Declared after MkMaterial because the pigment lookup takes one.
#include "scene_pigment.hlsl"

/// One light, POV's way.
///
/// `n` and `l` are unit and both point away from the surface; `v` points back at the eye.
/// `highlights` is false for a light that casts none: POV's `shadowless` light lights the
/// diffuse and nothing else -- measured (pov_specular_probe.py: the same plane reads its
/// full highlight from a plain light and exactly 0 from a shadowless one). The comparison's
/// default sun is written shadowless, which is why a highlight model could never be right
/// against it and why the checker scene tolerated a wrong one.
float3 mkFinishLit(MkMaterial m, float3 n, float3 l, float3 v, float3 lightColor,
                   bool highlights) {
    const float ndl = saturate(dot(n, l));

    // brilliance sharpens the falloff of the diffuse term itself. At 1.0 this is plain Lambert,
    // which is what every Grasp3D material means.
    const float lambert = (m.brilliance == 1.0) ? ndl : pow(ndl, m.brilliance);
    float3 col = m.diffuseColor * m.diffuse * lambert * lightColor;

    // POV's highlight colour is white unless metallic, in which case it takes the pigment. That is
    // the whole of POV's metal, and it is a tint, not a BRDF.
    const float3 highlight = lerp(float3(1, 1, 1), m.diffuseColor, m.metallic);

    if (highlights && m.specular > 0.0 && ndl > 0.0) {
        const float3 h = normalize(l + v);
        const float ca = dot(n, h);
        if (ca > 0.0) {
            // POV's specular is Blinn-Phong: (N.H)^(1/roughness), no N.L factor, dropped only
            // when the light is behind the surface. Measured, not read: pov_specular_probe.py
            // sweeps the light over a plane for six roughnesses and POV lands within 0.02 of
            // this power at every angle, and 0.3-0.9 away from the Gaussian in the half-angle
            // that stood here before. The Gaussian had been fitted on one checker scene whose
            // specular was small enough to hide it; the arm fixture (specular 0.15, roughness
            // 0.77) put the two renderers a mean of 12 levels apart.
            col += highlight * m.specular * pow(saturate(ca), 1.0 / max(m.roughness, 1e-4)) *
                   lightColor;
        }
    }

    if (highlights && m.phong > 0.0 && ndl > 0.0) {
        // The other highlight. Reflection direction rather than half vector -- POV computes phong
        // and specular differently, and collapsing them into one would change every scene that
        // uses phong.
        const float3 r = reflect(-l, n);
        col += highlight * m.phong * pow(saturate(dot(r, v)), m.phongSize) * lightColor;
    }

    return col;
}

/// A light that casts highlights, which is every light but POV's shadowless one.
float3 mkFinish(MkMaterial m, float3 n, float3 l, float3 v, float3 lightColor) {
    return mkFinishLit(m, n, l, v, lightColor, true);
}

/// The part that does not depend on any light.
float3 mkAmbientTerm(MkMaterial m, float occlusion) {
    return m.diffuseColor * m.ambient * occlusion + m.diffuseColor * m.emission;
}

#endif  // MAKINA_SCENE_FINISH_HLSL
