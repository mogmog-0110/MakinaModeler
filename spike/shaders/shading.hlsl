// Sphere tracing and shading. Identical for both evaluation strategies, so any measured
// difference between them is the cost of evaluating the tree and nothing else.
//
// Requires a prior definition of:  float evalCsg(float3 wp, float cullRadius);

#ifndef MAKINA_SHADING_HLSL
#define MAKINA_SHADING_HLSL

// Near the surface the exact field is what matters, so the cull radius stays tight here rather
// than tracking the march. The generated variant ignores it.
static const float kShadingCullRadius = 0.15;

// Tetrahedron sampling: four evaluations instead of the six a central difference needs.
float3 calcNormal(float3 p) {
    const float h = 0.0008;
    const float2 k = float2(1.0, -1.0);
    return normalize(k.xyy * evalCsg(p + k.xyy * h, kShadingCullRadius) +
                     k.yyx * evalCsg(p + k.yyx * h, kShadingCullRadius) +
                     k.yxy * evalCsg(p + k.yxy * h, kShadingCullRadius) +
                     k.xxx * evalCsg(p + k.xxx * h, kShadingCullRadius));
}

// Cone marching along the normal. This is the same field Phase 4 uses for cavity dirt, so its
// cost is measured here rather than discovered later.
float calcAO(float3 p, float3 n) {
    float occ = 0.0;
    float sca = 1.0;
    for (int i = 0; i < 5; ++i) {
        float h = 0.01 + 0.12 * float(i) / 4.0;
        float d = evalCsg(p + n * h, kShadingCullRadius);
        occ += (h - d) * sca;
        sca *= 0.95;
    }
    return saturate(1.0 - 3.0 * occ);
}

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID) {
    // Fullscreen triangle; no vertex buffer.
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

    float t = 0.0;
    // First step is coarse on purpose: everything is far, so everything culls.
    float cull = gFarDist;
    bool hit = false;

    for (uint s = 0; s < gMaxSteps; ++s) {
        float3 p = gEye + rd * t;
        float d = evalCsg(p, cull);
        if (d < 0.0008) { hit = true; break; }
        // Difference is max(a,-b), which is only a conservative lower bound on the true
        // distance, so a full step can tunnel through a seam. gStepScale < 1 is the guard (R-03).
        t += d * gStepScale;
        if (t > gFarDist) break;
        cull = d;
    }

    if (!hit) {
        float sky = 0.28 + 0.32 * (1.0 - i.uv.y);
        return float4(sky * 0.55, sky * 0.62, sky * 0.78, 1.0);
    }

    float3 p = gEye + rd * t;
    float3 n = calcNormal(p);

    float ao = gEnableAO != 0u ? calcAO(p, n) : 1.0;
    float ndl = saturate(dot(n, -gLightDir));
    float3 base = float3(0.62, 0.64, 0.68);

    float3 col = base * (0.16 * ao + 0.9 * ndl * ao);

    // Rim, purely so the silhouette reads in a still frame.
    float rim = pow(1.0 - saturate(dot(n, -rd)), 3.0);
    col += rim * 0.16;

    col = pow(saturate(col), 1.0 / 2.2);
    return float4(col, 1.0);
}

#endif  // MAKINA_SHADING_HLSL
