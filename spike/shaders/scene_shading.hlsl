// Sphere tracing and shading for a generated scene shader.
//
// Requires a prior definition of:  float evalCsg(float3 wp);

#ifndef MAKINA_SCENE_SHADING_HLSL
#define MAKINA_SCENE_SHADING_HLSL

#include "scene_finish.hlsl"
// After scene_finish, because the shadow march needs evalCsg and the light table needs somewhere
// to sit that both this and the engine wrapper can reach.
#include "scene_lights.hlsl"
// After both, because the march needs calcNormal and the shader-wide constants.
#include "scene_march.hlsl"

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

/// Where the ray for this pixel starts and which way it goes.
///
/// The only thing a camera model changes in a ray marcher. A rasteriser needs a projection matrix
/// and everything downstream has to agree with it; here a fisheye is four lines, and the cost of
/// having five cameras instead of one is five lines rather than five pipelines.
///
/// POV's formulas, because the whole point of matching POV's shading was to make the pictures
/// comparable, and a camera that framed the scene differently would make every pixel differ.
void mkCameraRay(float2 ndc, out float3 ro, out float3 rd) {
    ro = gEye;

    if (gCameraKind == 1u) {
        // Orthographic: every ray is parallel and the pixel moves the origin instead. gCameraAngle
        // is a half-width in world units here, not an angle -- POV sizes this camera by the
        // lengths of its right and up vectors and ignores its angle entirely.
        ro = gEye + gRight * (ndc.x * gAspect * gCameraAngle) + gUp * (-ndc.y * gCameraAngle);
        rd = gForward;
        return;
    }

    if (gCameraKind == 2u || gCameraKind == 3u) {
        // Fisheye and ultra wide angle differ only in whether the image is clipped to the circle
        // the projection actually covers. POV draws the fisheye as an inscribed circle and leaves
        // the corners black; ultra_wide_angle fills the frame by carrying on past 1.
        // No aspect here, deliberately. POV normalises a fisheye by the lengths of its right and
        // up vectors, so with right<-1,0,0> and up<0,1,0> the projection spans -1..1 across the
        // frame in both axes -- an ellipse in pixels on a wide image, not a circle. Multiplying
        // by the aspect gives the rounder picture and disagrees with the oracle everywhere.
        const float2 p = float2(ndc.x, -ndc.y);
        const float r = length(p);
        if (gCameraKind == 2u && r > 1.0) {
            rd = float3(0, 0, 0);   // outside the circle: the caller reads this as no ray
            return;
        }
        const float theta = r * gCameraAngle * 0.5;
        const float2 dir = r > 1e-6 ? p / r : float2(1, 0);
        rd = normalize(gForward * cos(theta) +
                       (gRight * dir.x + gUp * dir.y) * sin(theta));
        return;
    }

    if (gCameraKind == 4u) {
        // Panoramic: both axes are the angle itself, a quarter turn from the centre to each edge,
        // and `angle` does not enter at all.
        //
        // Measured, not read (spike/pov_camera_probe.py puts one bright marker at a known
        // direction and reports the pixel it lands on). On a square film POV maps yaw and
        // elevation to the frame exactly linearly -- 10 degrees to 0.1111, 40 to 0.4444 -- and
        // renders the identical image for `angle 60`, `angle 90` and `angle 180`. Both earlier
        // attempts at this camera failed on those two facts: they scaled by an angle POV ignores,
        // and they made the vertical a tangent when it is the angle.
        //
        // A frame that is not square is a different question and is not answered here. POV's
        // horizontal there is neither this nor this divided by the aspect: it agrees with the
        // divide near the centre and drifts to 76% away from it by the edge. render_scene.cpp
        // keeps the comparison square and says so rather than fitting a curve to a table.
        const float yaw = ndc.x * 1.57079632679489661923;
        const float pitch = -ndc.y * 1.57079632679489661923;
        rd = normalize(gForward * (cos(pitch) * cos(yaw)) + gRight * (cos(pitch) * sin(yaw)) +
                       gUp * sin(pitch));
        return;
    }

    rd = normalize(gForward
                 + gRight * (ndc.x * gAspect * gTanHalfFov)
                 + gUp    * (-ndc.y * gTanHalfFov));
}

/// Everything a surface owes to the lights, and to nothing else.
///
/// Lifted out of the frame so a reflected ray can be shaded by the same code. Two copies of the
/// light loop is exactly how a mirror ends up lit differently from the surface it mirrors, and
/// there would be no way to see it except by noticing the picture looks wrong.
///
/// The occlusion multiplies the light, not the material: POV has no ambient occlusion at all, so
/// folding it into the ambient term would invent a difference from the oracle in the one place the
/// two are supposed to be comparable.
float3 mkLighting(MkMaterial mat, float3 p, float3 n, float3 v, float ao, float eps) {
    float3 col = mkAmbientTerm(mat, ao);
    if (gLightCount == 0u) {
        // No lights in the scene: the one the renderer has always used, unshadowed. Every scene
        // rendered this way before lights were a thing the format carried, so keeping it means
        // nothing that used to draw suddenly goes black.
        return col + mkFinish(mat, n, -gLightDir, v, float3(1, 1, 1)) * ao;
    }
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
            shade = mkShadow(p, toLight, reach, lg.softness, eps);
        }
        if (shade <= 0.0) {
            continue;
        }
        col += mkFinish(mat, n, toLight, v, lg.color * mkFalloff(lg, reach)) * shade * ao;
    }
    return col;
}

/// What the mirrored ray brings back, or black when it leaves the scene.
///
/// One bounce. POV traces up to max_trace_level of them, so a mirror facing a mirror differs --
/// and that is the honest limit rather than a hidden one: a second bounce costs another march of
/// the whole field, and the first is what makes a surface read as metal at all.
///
/// The reflected surface is shaded without its own reflection, which is what makes one bounce a
/// bounce rather than an unbounded recursion HLSL would refuse to compile.
float3 mkReflected(float3 p, float3 n, float3 rd, float eps, float normalEps) {
    const float3 dir = reflect(rd, n);
    // Started off the surface, or the first sample reads the surface itself and every mirror comes
    // back black -- the same self-hit the shadow march has to avoid.
    float t = eps * 4.0;
    for (uint s = 0u; s < gMaxSteps; ++s) {
        const float d = evalCsg(p + dir * t);
        if (d < eps) {
            const float3 q = p + dir * t;
            const float3 qn = calcNormal(q, normalEps);
            const float3 qs = evalCsgMaterial(q);
            MkMaterial qm = mkMaterialAt(qs.y, gMaterialCount);
            qm.diffuseColor = mkSurfaceColor(qm, qs.z, q);
            return mkLighting(qm, q, qn, -dir, 1.0, eps);
        }
        t += d * gStepScale;
        if (t > gFarDist) {
            break;
        }
    }
    return float3(0, 0, 0);
}

float4 PSMain(VSOut i) : SV_Target {
    float2 ndc = i.uv * 2.0 - 1.0;
    float3 rayOrigin;
    float3 rd;
    mkCameraRay(ndc, rayOrigin, rd);
    if (dot(rd, rd) < 0.5) {
        // Outside a fisheye's circle. POV leaves this black rather than stretching the projection
        // to fill the frame, and matching that is what keeps the two images comparable.
        return float4(0, 0, 0, 1);
    }

    // Scaled off the scene size rather than fixed, so a model authored in millimetres and one in
    // metres both resolve: a hit threshold that suits a 2-unit box misses a 0.02-unit one entirely.
    float hitEps = gFarDist * 3.0e-5;
    float normalEps = gFarDist * 2.0e-4;
    float aoReach = gFarDist * 0.02;

    // Surfaces along the ray, front to back, until the light left to spend runs out.
    //
    // A ray marcher stops at the first surface because that is all an opaque model needs. POV
    // traces on through anything with a filter, so stopping here would not be a slightly different
    // picture -- it would be the transparent object drawn as if it were solid.
    //
    // Bounded rather than "until it leaves the scene": each layer is another march of the whole
    // field, and POV's own max_trace_level is 5. Four is the same order and is stated rather than
    // discovered as a slowdown.
    const uint kMaxLayers = 5u;

    float3 col = float3(0, 0, 0);
    // What fraction of each channel still reaches the eye. Per channel, because POV's filter is
    // tinted by the pigment -- red glass passes red.
    float3 through = float3(1, 1, 1);
    float3 origin = rayOrigin;
    float3 p = rayOrigin;
    float3 n = float3(0, 1, 0);
    bool   anyHit = false;

    for (uint layer = 0u; layer < kMaxLayers; ++layer) {
        const MkSurfaceHit surface = mkNextSurface(origin, rd, hitEps, normalEps);
        const bool hit = surface.hit;

        if (!hit) {
            if (gPovMatch == 0u) {
                float sky = 0.28 + 0.32 * (1.0 - i.uv.y);
                col += through * float3(sky * 0.55, sky * 0.62, sky * 0.78);
            }
            break;   // POV's background is black, so nothing is added in match mode
        }

        anyHit = true;
        p = surface.p;
        // The normal already faces the ray, and `entering` already says which side of the
        // interface this is. scene_march.hlsl carries the reason, shared with the other pass.
        n = surface.n;
        const bool entering = surface.entering;

        float ao = gEnableAO != 0u ? calcAO(p, n, aoReach) : 1.0;

        // One more evaluation, at the hit point only, to learn which surface this is. The march
        // and the normal never pay for it (scene_codegen.hpp explains why it is a separate
        // function).
        const float3 surf = evalCsgMaterial(p);
        MkMaterial mat = mkMaterialAt(surf.y, gMaterialCount);
        // The pattern is read at the hit point, in the space the march runs in -- POV transforms a
        // pigment with its object, so a moved solid takes its texture with it rather than sliding
        // through a pattern fixed to the world.
        mat.diffuseColor = mkSurfaceColor(mat, surf.z, p);

        float3 here = mkLighting(mat, p, n, -rd, ao, hitEps);

        if (mat.reflection > 0.0) {
            // Added on top rather than traded against the diffuse. That is POV's default: it only
            // takes the difference out of the diffuse when the finish asks for conserve_energy,
            // and matching the default is what keeps the two pictures comparable.
            here += mkReflected(p, n, rd, hitEps, normalEps) * mat.reflection;
        }

        if (gPovMatch == 0u) {
            // Edge lift, so a silhouette stays legible against the sky. Pure presentation -- POV
            // has no such term, so it goes when the two are being compared.
            float rim = pow(1.0 - saturate(dot(n, -rd)), 3.0);
            here += rim * 0.16;
        }

        // POV's filter, which is not alpha: what passes through is tinted by the pigment on the
        // way. `alpha` is stored as opacity and the exported file inverts it, so the filter is
        // what is left over.
        const float filt = saturate(1.0 - mat.alpha);

        // How much of this surface's own shading survives. Not (1 - filter): POV scales by the
        // *brightest channel of the pigment*, so a dark filtering colour hides less of what is
        // behind it than a bright one at the same filter value.
        //
        // Read off POV rather than assumed. A sphere with finish{ambient 1 diffuse 0} renders to
        // exactly its pigment times this weight, so sweeping filter and colour reports the weight
        // in the pixel itself: 0.4117/0.4115/0.4117 against 1 - 0.75 * 0.784, and the same to four
        // places at filter 0.25 and 0.5. A white pigment cannot tell the two rules apart, which is
        // why the first attempt at this matched one scene and no other.
        const float weight = 1.0 - filt * max(mat.diffuseColor.r,
                                              max(mat.diffuseColor.g, mat.diffuseColor.b));
        col += through * here * weight;
        if (filt <= 0.0) {
            break;
        }
        through *= filt * mat.diffuseColor;
        if (max(through.r, max(through.g, through.b)) < 1.0 / 512.0) {
            // Below half a level of an 8-bit channel. Carrying on would cost another march of the
            // field to change nothing that can be written to the file.
            break;
        }

        if (mat.ior > 1.0) {
            // POV's interior{ior}. The ratio is the one the ray is crossing into over the one it
            // is leaving, and only air is on the other side here: this model has no notion of one
            // solid nested inside another with its own interior, so a ray that enters two glasses
            // in a row is bent as though it had surfaced in between. Naming that is better than a
            // stack the field cannot tell it how to unwind.
            const float eta = entering ? 1.0 / mat.ior : mat.ior;
            const float3 bent = refract(rd, n, eta);
            // refract() returns zero past the critical angle. POV mirrors there rather than
            // dropping the ray, and so does the far wall of a glass sphere near its rim, which is
            // where the two renderers would otherwise differ most visibly.
            rd = dot(bent, bent) > 0.0 ? bent : reflect(rd, n);
        }

        // Off the surface far enough that the next march cannot find it again -- the reason
        // that is not a fixed nudge is in scene_march.hlsl, where it is shared with the
        // weathered pass.
        origin = p + rd * mkStepOff(p, rd, hitEps);
    }

    if (!anyHit) {
        if (gPovMatch != 0u) {
            return float4(0, 0, 0, 1);   // POV's default background
        }
        float sky = 0.28 + 0.32 * (1.0 - i.uv.y);
        return float4(sky * 0.55, sky * 0.62, sky * 0.78, 1.0);
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
