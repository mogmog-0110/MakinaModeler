// Sphere tracing and shading for a generated scene shader.
//
// Requires a prior definition of:  float evalCsg(float3 wp);

#ifndef MAKINA_SCENE_SHADING_HLSL
#define MAKINA_SCENE_SHADING_HLSL

#include "scene_finish.hlsl"
// After scene_finish, because the shadow march needs evalCsg and the light table needs somewhere
// to sit that both this and the engine wrapper can reach.
#include "scene_lights.hlsl"

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

float4 PSMain(VSOut i) : SV_Target {
    float2 ndc = i.uv * 2.0 - 1.0;
    float3 rd = normalize(gForward
                        + gRight * (ndc.x * gAspect * gTanHalfFov)
                        + gUp    * (-ndc.y * gTanHalfFov));

    // Scaled off the scene size rather than fixed, so a model authored in millimetres and one in
    // metres both resolve: a hit threshold that suits a 2-unit box misses a 0.02-unit one entirely.
    float hitEps = gFarDist * 3.0e-5;
    float normalEps = gFarDist * 2.0e-4;
    float aoReach = gFarDist * 0.02;

    float t = 0.0;
    bool hit = false;
    for (uint s = 0; s < gMaxSteps; ++s) {
        float d = evalCsg(gEye + rd * t);
        if (d < hitEps) { hit = true; break; }
        // Difference is max(a,-b), only a lower bound on the true distance, so a full step can
        // tunnel through a seam. Backing off is the guard (PLAN.md R-03).
        t += d * gStepScale;
        if (t > gFarDist) break;
    }

    if (!hit) {
        if (gPovMatch != 0u) {
            return float4(0, 0, 0, 1);   // POV's default background
        }
        float sky = 0.28 + 0.32 * (1.0 - i.uv.y);
        return float4(sky * 0.55, sky * 0.62, sky * 0.78, 1.0);
    }

    float3 p = gEye + rd * t;
    float3 n = calcNormal(p, normalEps);

    float ao = gEnableAO != 0u ? calcAO(p, n, aoReach) : 1.0;

    // One more evaluation, at the hit point only, to learn which surface this is. The march and
    // the normal never pay for it (scene_codegen.hpp explains why it is a separate function).
    MkMaterial mat = mkMaterialAt(evalCsgMaterial(p).y, gMaterialCount);
    // The pattern is read at the hit point, in the space the march runs in -- POV transforms a
    // pigment with its object, so a moved solid takes its texture with it rather than sliding
    // through a pattern fixed to the world.
    mat.diffuseColor = mkSurfaceColor(mat, mat.textureIndex, p);

    // The occlusion multiplies the light, not the material: POV has no ambient occlusion at all,
    // so folding it into the ambient term would be inventing a difference from the oracle in the
    // one place the two are supposed to be comparable.
    float3 col = mkAmbientTerm(mat, ao);
    if (gLightCount == 0u) {
        // No lights in the scene: the one the renderer has always used, unshadowed. Every scene
        // rendered this way before lights were a thing the format carried, so keeping it means
        // nothing that used to draw suddenly goes black.
        col += mkFinish(mat, n, -gLightDir, -rd, float3(1, 1, 1)) * ao;
    } else {
        for (uint li = 0u; li < gLightCount; ++li) {
            const MkLight lg = gLights[li];
            float3 toLight;
            float  reach;
            if (lg.directional != 0u) {
                toLight = normalize(-lg.position);
                reach = gFarDist;
            } else {
                const float3 delta = lg.position - p;
                reach = length(delta);
                toLight = delta / max(reach, 1e-9);
            }

            float shade = 1.0;
            if (lg.shadowless == 0u) {
                shade = mkShadow(p, toLight, reach, lg.softness, hitEps);
            }
            if (shade <= 0.0) {
                continue;
            }
            col += mkFinish(mat, n, toLight, -rd, lg.color * mkFalloff(lg, reach)) * shade * ao;
        }
    }

    if (gPovMatch == 0u) {
        // Edge lift, so a silhouette stays legible against the sky. Pure presentation -- POV has
        // no such term, so it goes when the two are being compared.
        float rim = pow(1.0 - saturate(dot(n, -rd)), 3.0);
        col += rim * 0.16;
    }

    // Selection: tint whatever is inside the selected subtree's world box.
    //
    // The box, not the surface. Marking the exact surface means the shader has to know which
    // nodes belong to the selection, and that is a change to the generated program rather than to
    // a constant. The box answers the question a click actually asks -- "did that land on what I
    // meant" -- and for a subtree it is that subtree's own extent.
    //
    // gSelValid is zero for every offscreen render, so nothing else changes.
    if (gSelValid > 0.5) {
        float3 slack = float3(1.0, 1.0, 1.0) * (gSceneRadius * 1e-3);
        bool inside = all(p >= gSelMin - slack) && all(p <= gSelMax + slack);
        if (inside) {
            col = lerp(col, float3(1.00, 0.62, 0.18), 0.35);
        }
    }

    // sRGB, not a 2.2 power. POV-Ray 3.7 writes 8-bit files sRGB-encoded, so a plain 1/2.2 would
    // put a systematic difference in the darks -- small, everywhere, and exactly the kind of thing
    // a comparison gets loosened to forgive rather than fixed.
    // step/lerp rather than a ternary: HLSL 2021 refuses a vector condition on ?:, and writing it
    // per channel would be three copies of one rule.
    const float3 c = saturate(col);
    const float3 lo = c * 12.92;
    const float3 hi = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return float4(lerp(lo, hi, step(0.0031308, c)), 1.0);
}

#endif  // MAKINA_SCENE_SHADING_HLSL
