// Geometry fields derived from the distance field itself.
//
// This is the whole argument for building the modeller on an SDF. A mesh pipeline gets ambient
// occlusion, curvature and thickness by baking them into textures, which needs UVs and takes
// minutes per asset, and which goes stale the moment the geometry changes. Here each one is a few
// extra evaluations of a function the renderer is already calling, so they cost nothing to keep
// current: drag a boolean and the wear follows it in the same frame.
//
// Requires a prior definition of:  float evalCsg(float3 wp);

#ifndef MAKINA_FIELDS_HLSL
#define MAKINA_FIELDS_HLSL

struct GeoFields {
    float3 normal;
    float  ao;         ///< 0 deep in a cavity, 1 out in the open
    float  curvature;  ///< signed and normalised: +1 a sharp convex edge, -1 a tight groove
    float  thickness;  ///< 0 paper thin, 1 as thick as the probe reaches
    float  upFacing;   ///< 0 vertical or downward, 1 facing straight up
};

/// How wide each field samples, in world units.
///
/// Every one of these is a property of the *model*, so they scale off the model's own size. They
/// used to scale off the far plane, which is the camera distance plus a margin -- the wear band
/// therefore widened when the camera pulled back, which is not something a material does. At the
/// default framing the numbers below reproduce the old ones exactly; what changes is that they no
/// longer move when the camera does.
///
/// Gathered in one struct so the material pass and the --fields debug view cannot drift apart.
/// They had: the debug view sampled curvature at 1.2e-3 while the material used 3.2e-3, so the
/// picture people were tuning against was not the picture being tuned.
struct FieldScales {
    float normalEps;
    float curvatureEps;
    float aoReach;
    float thicknessReach;
};

FieldScales mkFieldScales(float sceneRadius) {
    FieldScales s;
    s.normalEps = sceneRadius * 8.7e-4;
    s.curvatureEps = sceneRadius * 1.87e-2;
    s.aoReach = sceneRadius * 0.175;
    s.thicknessReach = sceneRadius * 0.321;
    return s;
}

/// Tetrahedral sampling: four evaluations give the gradient, and the same four plus the centre
/// give the Laplacian, so curvature rides along almost free.
float3 mkNormal4(float3 p, float h, out float4 taps) {
    const float2 k = float2(1.0, -1.0);
    taps.x = evalCsg(p + k.xyy * h);
    taps.y = evalCsg(p + k.yyx * h);
    taps.z = evalCsg(p + k.yxy * h);
    taps.w = evalCsg(p + k.xxx * h);
    return normalize(k.xyy * taps.x + k.yyx * taps.y + k.yxy * taps.z + k.xxx * taps.w);
}

/// Laplacian of the distance field: positive on a convex edge, negative in a groove.
///
/// The normaliser is the sampling distance itself, not a length picked by hand. The Laplacian has
/// units of 1/length, and at a sharp edge the distance field has a kink, so the discrete second
/// difference there grows like 1/h -- multiplying by h leaves an edge at order one whatever h is.
/// A smoothly curved surface of radius R gives h/R instead, which stays near zero. So this
/// separates "crease" from "gently round" by construction, and it does so without a constant that
/// would have to be retuned per model.
///
/// Picking a fixed reference length instead is what produced a first attempt where the flange's
/// entire outer wall registered as a worn edge: a cylinder of radius 2 saturated the same as a
/// 90-degree corner.
float mkCurvature(float3 p, float4 taps, float h) {
    float center = evalCsg(p);
    float lap = (taps.x + taps.y + taps.z + taps.w - 4.0 * center) / (h * h);
    return clamp(lap * h * 0.5, -1.0, 1.0);
}

/// Cone-marched ambient occlusion. reach sets how far the probe looks; anything beyond it is
/// treated as open sky.
float mkAmbientOcclusion(float3 p, float3 n, float reach) {
    float occ = 0.0;
    float norm = 0.0;
    float sca = 1.0;
    [unroll]
    for (int i = 0; i < 5; ++i) {
        float h = reach * (0.08 + 0.92 * float(i) / 4.0);
        float d = evalCsg(p + n * h);
        occ += (h - d) * sca;
        // Accumulating the fully-occluded case alongside is what makes occ/norm land in 0..1 by
        // construction. Dividing by `reach` instead, as a first attempt did, overshoots by the
        // number of samples: occ can reach 2.1 * reach, so everything past a third of the way in
        // clipped to black and every groove came out as a flat dark band with no shape in it.
        norm += h * sca;
        sca *= 0.92;
    }
    return saturate(1.0 - 1.15 * occ / max(norm, 1e-6));
}

/// How far the solid continues below the surface, as a fraction of reach. Walking inward along the
/// normal until the field turns positive is enough: an exact chord length is not needed, only a
/// monotone "is this a thin web or a solid block".
float mkThickness(float3 p, float3 n, float reach) {
    float depth = 0.0;
    [unroll]
    for (int i = 1; i <= 8; ++i) {
        float s = reach * float(i) / 8.0;
        if (evalCsg(p - n * s) > 0.0) {
            break;
        }
        depth = s;
    }
    return saturate(depth / max(reach, 1e-6));
}

/// Everything the material needs, from one surface point.
GeoFields mkGeoFields(float3 p, float normalEps, float curvatureEps, float aoReach,
                      float thicknessReach) {
    GeoFields f;
    float4 taps;
    f.normal = mkNormal4(p, normalEps, taps);

    // Curvature is sampled at its own, wider spacing. At the normal's spacing the second
    // difference is mostly rounding noise, which reads as speckle along every edge.
    float4 wide;
    mkNormal4(p, curvatureEps, wide);
    f.curvature = mkCurvature(p, wide, curvatureEps);

    f.ao = mkAmbientOcclusion(p, f.normal, aoReach);
    f.thickness = mkThickness(p, f.normal, thicknessReach);
    f.upFacing = saturate(f.normal.y);
    return f;
}

#endif  // MAKINA_FIELDS_HLSL
