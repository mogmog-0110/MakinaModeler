// Renders the geometry fields on their own, so they can be looked at before a material is tuned
// against them.
//
// Tuning a weathering mask by adjusting numbers until the picture improves is guessing. If the
// curvature field is saturated or the occlusion never leaves the top of its range, no amount of
// material tweaking fixes it, and the tweaks accumulate as compensation for a problem one layer
// down. Looking at the fields first turns that into a measurement.
//
// gDebugMode picks the field: 1 curvature, 2 occlusion, 3 thickness, 4 up-facing, 5 normal.

#ifndef MAKINA_SCENE_FIELDS_DEBUG_HLSL
#define MAKINA_SCENE_FIELDS_DEBUG_HLSL

#include "fields.hlsl"

/// Blue for negative, red for positive, black at zero. A signed field shown as plain greyscale
/// hides its sign, which is the one thing curvature is read for.
float3 mkSignedRamp(float v) {
    float pos = saturate(v);
    float neg = saturate(-v);
    return float3(pos, 0.12 * (pos + neg), neg);
}

/// Blue-green-yellow-red. Monotone and roughly uniform in perceived lightness, so a plateau in the
/// field is visible as a plateau rather than hidden in the dark end of a grey ramp.
float3 mkHeatRamp(float v) {
    v = saturate(v);
    float3 c = lerp(float3(0.05, 0.06, 0.35), float3(0.10, 0.70, 0.55), saturate(v * 3.0));
    c = lerp(c, float3(0.95, 0.85, 0.20), saturate((v - 0.33) * 3.0));
    c = lerp(c, float3(0.90, 0.20, 0.12), saturate((v - 0.66) * 3.0));
    return c;
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

    float hitEps = gFarDist * 3.0e-5;
    FieldScales scales = mkFieldScales(gSceneRadius);

    float t = 0.0;
    bool hit = false;
    for (uint s = 0; s < gMaxSteps; ++s) {
        float d = evalCsg(gEye + rd * t);
        if (d < hitEps) { hit = true; break; }
        t += d * gStepScale;
        if (t > gFarDist) break;
    }

    if (!hit) {
        return float4(0.02, 0.02, 0.025, 1.0);
    }

    float3 p = gEye + rd * t;
    GeoFields f = mkGeoFields(p, scales.normalEps, scales.curvatureEps, scales.aoReach,
                                  scales.thicknessReach);

    float3 col;
    if (gDebugMode == 1u)      col = mkSignedRamp(f.curvature);
    else if (gDebugMode == 2u) col = mkHeatRamp(f.ao);
    else if (gDebugMode == 3u) col = mkHeatRamp(f.thickness);
    else if (gDebugMode == 4u) col = mkHeatRamp(f.upFacing);
    else                       col = f.normal * 0.5 + 0.5;

    return float4(pow(saturate(col), 1.0 / 2.2), 1.0);
}

#endif  // MAKINA_SCENE_FIELDS_DEBUG_HLSL
