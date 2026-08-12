// Sphere tracing for a solid placed inside somebody else's 3D scene.
//
// The modeller's wrapper (scene_shading.hlsl) owns the whole frame: the ray starts at the camera,
// the background is its own sky, and there is no depth buffer because nothing else is drawn. None
// of that holds in a game. A prop shares the frame with meshes, has to be somewhere in the world
// rather than at the origin, and has to occlude and be occluded correctly.
//
// So this wrapper differs in exactly three ways, and no others -- evalCsg is the same generated
// function, so a difference between the two pictures can only be the wrapper:
//
//   placement   the march runs in object space. The ray is brought there by gWorldToObject, so
//               the baked evalCsg never learns where the prop was put.
//   bounds      the march is clipped to the solid's own box. Without it every pixel of the screen
//               would run the full step budget for a prop that covers a tenth of it.
//   depth       the hit is projected and written to SV_Depth, which is what lets a mesh in front
//               of the prop stay in front of it.
//
// Rigid placement with uniform scale only. A non-uniform scale is not a distance any more -- the
// field would read short along the squashed axis and the march would step through the surface.
// CsgRenderPass refuses one rather than drawing it wrong.
//
// Requires a prior definition of:  float evalCsg(float3 p);   // object space

#ifndef MAKINA_SCENE_ENGINE_HLSL
#define MAKINA_SCENE_ENGINE_HLSL

#include "scene_finish.hlsl"

/// What the engine knows that the modeller does not.
///
/// b1, so the camera basis in b0 keeps the layout every other Makina shader uses and one struct
/// serves both. gViewProj is here rather than derived from b0 because the engine already has it
/// and a second derivation is a second chance to disagree about handedness.
// row_major, deliberately. HLSL packs a cbuffer float4x4 column-major by default, so a matrix
// memcpy'd from the engine's row-major sgc::Mat4f would arrive transposed -- and a transposed
// rotation is still a plausible-looking matrix, so the prop would simply be in the wrong place
// with nothing to say why. Spelling the storage here costs nothing and removes the trap.
cbuffer EngineParams : register(b1) {
    row_major float4x4 gWorldToObject;
    row_major float4x4 gObjectToWorld;
    row_major float4x4 gViewProj;
    float3   gBoxMin;       float gObjectScale;   ///< uniform scale, object -> world
    float3   gBoxMax;       float gDepthBias;
    float3   gBaseColor;    float gAmbient;
};

/// Slab test against the solid's object-space box.
///
/// Returns false when the ray misses, which discards the pixel outright. tNear is clamped to zero
/// so a camera inside the box still starts at the camera rather than behind it.
bool intersectBox(float3 ro, float3 rd, out float tNear, out float tFar) {
    // A component of rd that is exactly zero gives inf here, and the min/max below handle that
    // correctly: the slab is either entirely in or entirely out, and inf/-inf says which.
    const float3 inv = 1.0 / rd;
    const float3 a = (gBoxMin - ro) * inv;
    const float3 b = (gBoxMax - ro) * inv;
    const float3 lo = min(a, b);
    const float3 hi = max(a, b);
    tNear = max(max(lo.x, lo.y), lo.z);
    tFar  = min(min(hi.x, hi.y), hi.z);
    tNear = max(tNear, 0.0);
    return tFar >= tNear;
}

float3 calcNormal(float3 p, float h) {
    const float2 k = float2(1.0, -1.0);
    return normalize(k.xyy * evalCsg(p + k.xyy * h) +
                     k.yyx * evalCsg(p + k.yyx * h) +
                     k.yxy * evalCsg(p + k.yxy * h) +
                     k.xxx * evalCsg(p + k.xxx * h));
}

float calcAO(float3 p, float3 n, float reach) {
    float occ = 0.0;
    float sca = 1.0;
    for (int i = 0; i < 5; ++i) {
        float h = reach * (0.08 + 0.92 * float(i) / 4.0);
        occ += (h - evalCsg(p + n * h)) * sca;
        sca *= 0.95;
    }
    return saturate(1.0 - 3.0 * occ / max(reach, 1e-6));
}

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID) {
    float2 uv = float2((vid << 1) & 2, vid & 2);
    VSOut o;
    o.uv  = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

struct PSOut {
    float4 color : SV_Target;
    float  depth : SV_Depth;
};

PSOut PSMain(VSOut i) {
    PSOut o;
    o.color = float4(0, 0, 0, 0);
    o.depth = 1.0;

    const float2 ndc = i.uv * 2.0 - 1.0;
    const float3 rdWorld = normalize(gForward
                                   + gRight * (ndc.x * gAspect * gTanHalfFov)
                                   + gUp    * (-ndc.y * gTanHalfFov));

    // Into object space. The direction is normalised again because a uniform scale changes its
    // length, and the march measures progress in object units.
    const float3 ro = mul(gWorldToObject, float4(gEye, 1.0)).xyz;
    const float3 rd = normalize(mul(gWorldToObject, float4(rdWorld, 0.0)).xyz);

    float tNear, tFar;
    if (!intersectBox(ro, rd, tNear, tFar)) {
        discard;
    }

    // Off the box rather than off the whole scene, so the same thresholds suit a prop that is a
    // tenth of the world and one that is all of it.
    const float3 span = gBoxMax - gBoxMin;
    const float extent = max(max(span.x, span.y), span.z);
    const float hitEps = extent * 3.0e-5;
    const float normalEps = extent * 2.0e-4;
    const float aoReach = extent * 0.05;

    float t = tNear;
    bool hit = false;
    for (uint s = 0; s < gMaxSteps; ++s) {
        const float d = evalCsg(ro + rd * t);
        if (d < hitEps) { hit = true; break; }
        // Difference is max(a,-b), a lower bound only, so a full step can tunnel a thin seam
        // (PLAN.md R-03). The same 0.85 the modeller and the CPU raycast use.
        t += d * gStepScale;
        if (t > tFar) break;
    }

    if (!hit) {
        discard;
    }

    const float3 p = ro + rd * t;
    const float3 n = calcNormal(p, normalEps);

    // Back to world for depth and for the light, which is given in world space.
    const float3 pw = mul(gObjectToWorld, float4(p, 1.0)).xyz;
    const float3 nw = normalize(mul((float3x3)gObjectToWorld, n));

    const float4 clip = mul(gViewProj, float4(pw, 1.0));
    if (clip.w <= 0.0) {
        discard;   // behind the eye; there is no depth to write
    }
    o.depth = saturate(clip.z / clip.w + gDepthBias);

    const float ao = gEnableAO != 0u ? calcAO(p, n, aoReach) : 1.0;

    // The material the solid was authored with. gBaseColor still has a job: it tints the whole
    // prop, so a game can reuse one baked shader for several coloured copies without re-baking.
    // A solid that names no material takes gBaseColor outright, which is what CsgDrawDesc means.
    MkMaterial mat = mkMaterialAt(evalCsgMaterial(p).y, gMaterialCount);
    // Object space, which is where the march already is -- the texture travels with the prop.
    mat.diffuseColor = mkSurfaceColor(mat, mat.textureIndex, p) * gBaseColor;
    mat.ambient = gAmbient;

    float3 col = mkAmbientTerm(mat, ao) + mkFinish(mat, nw, -gLightDir, -rdWorld, float3(1, 1, 1)) * ao;
    const float rim = pow(1.0 - saturate(dot(nw, -rdWorld)), 3.0);
    col += rim * 0.16;

    // Linear, not gamma-encoded. The modeller's wrapper writes straight to an 8-bit swap chain and
    // has to encode; the engine's colour target is R16G16B16A16_FLOAT and a tonemap runs later.
    // Encoding here would be encoding twice, and the prop would come out pale next to every mesh.
    o.color = float4(col, 1.0);
    return o;
}

#endif  // MAKINA_SCENE_ENGINE_HLSL
