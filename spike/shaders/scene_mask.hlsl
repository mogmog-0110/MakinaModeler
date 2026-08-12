// Silhouette only: white where the ray hits the solid, black where it does not.
//
// Made to be compared against POV-Ray's render of the same scene from the same camera
// (Pov.hpp, PovOptions::silhouette). A silhouette is the part two independent renderers can be
// held to agree on pixel for pixel -- it depends on the geometry, the transforms, the camera and
// the handedness, and on nothing else. Shading, tone mapping and light units are not shared and
// comparing them would only measure how differently they were tuned.
//
// No anti-aliasing on purpose. A hard mask makes the intersection-over-union count what it says it
// counts; a soft edge would blur the disagreement into the tolerance.
//
// Requires a prior definition of:  float evalCsg(float3 wp);

#ifndef MAKINA_SCENE_MASK_HLSL
#define MAKINA_SCENE_MASK_HLSL

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

    float t = 0.0;
    for (uint s = 0; s < gMaxSteps; ++s) {
        float d = evalCsg(gEye + rd * t);
        if (d < hitEps) {
            return float4(1.0, 1.0, 1.0, 1.0);
        }
        // Difference is a lower bound on the distance, so a full step can tunnel a seam
        // (PLAN.md R-03). Undershooting matters more here than anywhere: a tunnelled ray puts a
        // black pixel inside the silhouette and the comparison reports a hole that is not there.
        t += d * gStepScale;
        if (t > gFarDist) {
            break;
        }
    }
    return float4(0.0, 0.0, 0.0, 1.0);
}

#endif  // MAKINA_SCENE_MASK_HLSL
