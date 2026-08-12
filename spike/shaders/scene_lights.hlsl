// Lights, and the shadow a distance field can throw almost for free.
//
// This is the one place the march is *better* than the ray trace rather than merely equal to it.
// A ray tracer answers "is this point in shadow" with a yes or a no; a soft edge needs an area
// light and many samples, and the cost is the sample count. Sphere tracing answers with the
// closest the shadow ray ever came to a surface, which is a penumbra in one march. POV-Ray needs
// dozens of rays for what this gets from the same loop it already runs.
//
// The hard case is kept honest on purpose: with softness 0 the result is a plain yes or no, which
// is the only kind of shadow a POV point light casts and therefore the only kind the two
// renderers can be held to agreeing on. Everything softer is this renderer doing something the
// oracle cannot, and the comparison excludes it rather than loosening to fit.
//
// Requires: float evalCsg(float3) and the light table below.

#ifndef MAKINA_SCENE_LIGHTS_HLSL
#define MAKINA_SCENE_LIGHTS_HLSL

/// Must match makina::Light byte for byte (Scene.hpp asserts the size on the CPU side).
struct MkLight {
    float3 position;      float _pad0;
    float3 color;         float softness;
    float  fadeDistance;  float fadePower;
    uint   directional;   uint  shadowless;
};

StructuredBuffer<MkLight> gLights : register(t3);

/// How much of this light reaches p, in 0..1.
///
/// The march runs from just off the surface toward the light. Starting exactly on it would read
/// the surface itself as an occluder and put the object in its own shadow -- the classic acne,
/// and the reason the first step is offset rather than the epsilon being enlarged.
float mkShadow(float3 p, float3 toLight, float distance, float softness, float eps) {
    if (softness < 0.0) {
        return 1.0;
    }
    float shade = 1.0;
    float t = eps * 4.0;
    for (uint i = 0u; i < 64u; ++i) {
        const float d = evalCsg(p + toLight * t);
        if (d < eps) {
            return 0.0;   // fully blocked; nothing softer can come of marching further
        }
        if (softness > 0.0) {
            // The closest approach, scaled by how far along the ray it happened: a blocker near
            // the surface makes a sharp edge, one near the light a broad one. That distance
            // dependence is what makes this a penumbra rather than a blur.
            shade = min(shade, d / (softness * t));
        }
        t += d;
        if (t >= distance) {
            break;
        }
    }
    return saturate(shade);
}

/// POV's fade_distance / fade_power, which is not an inverse square unless you ask for one.
///
/// POV's formula, so the export and the picture agree: at fade_power 2 and fade_distance d the
/// factor is 2 / (1 + (r/d)^2), which is 1 at r = d rather than at r = 0. Writing the physical
/// 1/r^2 instead would be brighter everywhere and the comparison would blame the material.
float mkFalloff(MkLight g, float r) {
    if (g.fadePower <= 0.0 || g.fadeDistance <= 0.0) {
        return 1.0;
    }
    return 2.0 / (1.0 + pow(r / g.fadeDistance, g.fadePower));
}

#endif  // MAKINA_SCENE_LIGHTS_HLSL
