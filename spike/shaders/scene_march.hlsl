// Finding the next surface along a ray, and leaving it again.
//
// Two shading passes live in this repository and they are deliberately different pictures: one is
// held to POV-Ray pixel for pixel, the other is the weathered look Phase 4 is about. What they must
// not differ on is *where the surfaces are*. Two copies of a sphere-tracing loop drift the way two
// copies of anything drift, and the symptom would be one pass showing a seam the other does not --
// with no way to say which had it right.
//
// So the loop lives here once. Each pass keeps its own compositing, its own materials and its own
// tone curve, and asks this for the geometry.
//
// Requires a prior definition of:  float evalCsg(float3 wp);

#ifndef MAKINA_SCENE_MARCH_HLSL
#define MAKINA_SCENE_MARCH_HLSL

/// The surface normal, as the gradient of the field.
///
/// Here rather than beside the shading because the march is what needs it: a hit is not a surface
/// until it has a direction, and both passes ask this for the same points.
float3 calcNormal(float3 p, float h) {
    const float2 k = float2(1.0, -1.0);
    return normalize(k.xyy * evalCsg(p + k.xyy * h) +
                     k.yyx * evalCsg(p + k.yyx * h) +
                     k.yxy * evalCsg(p + k.yxy * h) +
                     k.xxx * evalCsg(p + k.xxx * h));
}

/// One surface along the ray.
struct MkSurfaceHit {
    bool   hit;
    float3 p;
    /// Turned to face the ray, POV's way -- there is no back face, and the far wall of a
    /// transparent solid must not light as though it were in shadow.
    float3 n;
    /// False when the ray is on its way out of a solid rather than into one, which is what tells
    /// refraction which side of the interface it is crossing.
    bool   entering;
    /// Distance travelled from the origin.
    float  t;
};

/// Marches from `origin` along `rd` to the next surface.
///
/// By the magnitude of the distance, not its value. After the first surface the ray carries on from
/// inside the solid, where the field is negative and a signed test reads every step as a hit;
/// marching by |d| walks the interior and stops on the way out.
MkSurfaceHit mkNextSurface(float3 origin, float3 rd, float hitEps, float normalEps) {
    MkSurfaceHit h;
    h.hit = false;
    h.p = origin;
    h.n = float3(0, 1, 0);
    h.entering = true;
    h.t = 0.0;

    float t = 0.0;
    for (uint s = 0u; s < gMaxSteps; ++s) {
        const float d = evalCsg(origin + rd * t);
        if (abs(d) < hitEps) {
            h.hit = true;
            break;
        }
        // Difference is max(a,-b), only a lower bound on the true distance, so a full step can
        // tunnel through a seam. Backing off is the guard (PLAN.md R-03).
        t += abs(d) * gStepScale;
        if (t > gFarDist) {
            break;
        }
    }
    if (!h.hit) {
        return h;
    }

    h.t = t;
    h.p = origin + rd * t;
    h.n = calcNormal(h.p, normalEps);
    h.entering = dot(h.n, rd) < 0.0;
    if (!h.entering) {
        h.n = -h.n;
    }
    return h;
}

/// How far past a surface a ray has to start so the next march cannot find the same one again.
///
/// A fixed nudge is not enough at a grazing angle: the field stays inside the hit threshold for a
/// long stretch there, so the next march reports an immediate hit on the surface just shaded and
/// composites it over itself once per layer. That showed as a bright fringe on the silhouette --
/// 134 where POV-Ray had 66 -- while the interior of the same solid agreed level for level.
float mkStepOff(float3 p, float3 rd, float hitEps) {
    float off = hitEps * 4.0;
    for (uint g = 0u; g < 64u; ++g) {
        if (abs(evalCsg(p + rd * off)) > hitEps) {
            break;
        }
        off += hitEps * 2.0;
    }
    return off;
}

#endif  // MAKINA_SCENE_MARCH_HLSL
