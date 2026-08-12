// Procedural weathering driven entirely by the geometry fields.
//
// No textures, no UVs, nothing baked. Every mask below is a function of the distance field, so the
// wear is not painted onto the model -- it is a consequence of the model's shape, and it re-derives
// itself the instant the shape changes.
//
//   polished edges   high positive curvature: the places a part gets knocked and handled
//   grime            low occlusion: dirt collects where nothing can wash it out
//   dust             up-facing and open: it settles, and it does not stick to worn edges
//   scatter          thin sections let a little light through and warm up
//
// Requires a prior definition of:  float evalCsg(float3 wp);

#ifndef MAKINA_SCENE_WEATHERED_HLSL
#define MAKINA_SCENE_WEATHERED_HLSL

#include "fields.hlsl"

struct Surface {
    float3 albedo;
    float  roughness;
    float  metallic;
    float3 emissive;
};

// ---------------------------------------------------------------- material

static const float3 kSteel     = float3(0.55, 0.56, 0.58);
static const float3 kPolished  = float3(0.90, 0.91, 0.93);
static const float3 kGrime     = float3(0.055, 0.044, 0.032);
static const float3 kDust      = float3(0.62, 0.60, 0.55);
static const float3 kScatter   = float3(0.55, 0.28, 0.16);

// ---------------------------------------------------------------- shading

float mkDistributionGGX(float ndh, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / max(3.14159265 * d * d, 1e-7);
}

float mkGeometrySmith(float ndv, float ndl, float roughness) {
    float r = roughness + 1.0;
    float k = r * r / 8.0;
    float gv = ndv / (ndv * (1.0 - k) + k);
    float gl = ndl / (ndl * (1.0 - k) + k);
    return gv * gl;
}

float3 mkFresnel(float cosTheta, float3 f0) {
    return f0 + (1.0 - f0) * pow(saturate(1.0 - cosTheta), 5.0);
}

/// Sky above, warm bounce below. Cheap, but a metal with nothing to reflect looks like clay, and
/// the whole point of the polished edges is that they reflect something.
float3 mkEnvironment(float3 dir) {
    float t = saturate(dir.y * 0.5 + 0.5);
    float3 sky = lerp(float3(0.26, 0.30, 0.36), float3(0.52, 0.60, 0.74), t);
    float3 ground = float3(0.10, 0.095, 0.09);
    return lerp(ground, sky, smoothstep(-0.25, 0.15, dir.y));
}

/// What a ray that hits nothing shows.
///
/// Kept apart from mkEnvironment on purpose. The environment is what the metal reflects, and it
/// has to be bright or a polished edge reflects nothing and the part looks like clay. The
/// background is what sits behind the part, and it has to be dark or there is no contrast for the
/// edges to read against -- and, worse, killing the floor beyond its radius changes nothing,
/// because the floor and the background come out the same grey. That is exactly what the first
/// attempt did.
float3 mkBackground(float3 dir) {
    float t = saturate(dir.y * 0.5 + 0.5);
    return lerp(float3(0.020, 0.021, 0.026), float3(0.075, 0.082, 0.10), t * t);
}

float3 mkDirectLight(Surface s, float3 n, float3 v, float3 l, float3 radiance) {
    float3 h = normalize(v + l);
    float ndl = saturate(dot(n, l));
    float ndv = saturate(dot(n, v)) + 1e-5;
    float ndh = saturate(dot(n, h));
    float vdh = saturate(dot(v, h));

    float3 f0 = lerp(float3(0.04, 0.04, 0.04), s.albedo, s.metallic);
    float3 f = mkFresnel(vdh, f0);
    float ndf = mkDistributionGGX(ndh, s.roughness);
    float g = mkGeometrySmith(ndv, ndl, s.roughness);

    float3 spec = ndf * g * f / max(4.0 * ndv * ndl, 1e-5);
    float3 kd = (1.0 - f) * (1.0 - s.metallic);
    return (kd * s.albedo / 3.14159265 + spec) * radiance * ndl;
}

static const float3 kKeyDir  = normalize(float3(0.45, 0.80, 0.40));
static const float3 kFillDir = normalize(float3(-0.55, 0.25, -0.60));

/// Soft shadow by sphere tracing toward the light, penumbra from how close the march came to
/// grazing something.
///
/// This is the one thing a mesh pipeline needs a whole extra pass and a shadow map for, and here
/// it falls out of the same distance field the picture is already made of. It does two jobs at
/// once: the part throws a contact shadow onto the floor, and it shadows itself, which is what
/// finally makes a groove read as a groove rather than as a darker stripe.
///
/// `k` sets the penumbra width -- higher is harder. `tMin` lifts the start off the surface so a
/// point does not shadow itself at distance zero.
///
/// `steps` is the one knob worth spending thought on. Both marches together cost about 40% of the
/// frame on pettobotoru (min of 25 draws, three runs a side, against a run-to-run spread of 1.5%),
/// so the sky term -- which feeds a soft mask nobody looks at directly -- gets far fewer than the
/// cast shadow, whose edge is visible.
float mkSoftShadow(float3 origin, float3 dir, float tMin, float tMax, float k, uint steps) {
    float res = 1.0;
    float t = tMin;
    for (uint i = 0; i < steps; ++i) {
        float d = evalCsg(origin + dir * t);
        // Difference is a lower bound on the distance, so the same tunnelling risk as the primary
        // march applies; the step is scaled back for the same reason (PLAN.md R-03).
        res = min(res, k * d / t);
        t += clamp(d * 0.85, tMax * 2.0e-3, tMax * 0.12);
        if (res < 0.002 || t > tMax) {
            break;
        }
    }
    return saturate(res);
}

/// `sky` is how much of the upward hemisphere this point can see, from 0 (under an overhang) to 1
/// (open). It is what turns grime from a cavity filler into something that runs.
Surface mkWeather(GeoFields f, float sky) {
    Surface s;
    s.albedo = kSteel;
    s.roughness = 0.58;
    s.metallic = 1.0;
    s.emissive = float3(0.0, 0.0, 0.0);

    // Edges the part is handled by. Only the convex half counts, or the inside corner of a groove
    // would polish up as readily as the rim.
    //
    // The threshold sits low deliberately. Wear on a real part is not confined to the geometric
    // crease -- it spreads onto the faces either side, because what rubs the edge rubs its
    // surroundings too. Catching the shoulder of the curvature falloff is what produces a band
    // instead of a hairline, and a hairline is what the first attempt drew.
    float edge = smoothstep(0.03, 0.28, f.curvature);
    // Wear needs exposure as well as an edge: a sharp corner at the bottom of a bore never gets
    // rubbed, and it is the combination that makes the result read as use rather than as a filter.
    edge *= lerp(0.15, 1.0, f.ao);
    s.albedo = lerp(s.albedo, kPolished, edge);
    s.roughness = lerp(s.roughness, 0.10, edge);

    // Grime, in whatever the light cannot reach.
    float grime = saturate((1.0 - f.ao) * 1.45);
    grime *= grime;
    // Concave only, but the gate opens on anything that is not clearly convex:
    // a shallow dish collects dirt just as a tight groove does, and requiring a
    // strong negative curvature confined the effect to creases nobody can see.
    grime *= smoothstep(0.16, -0.08, f.curvature);
    grime = saturate(grime * 2.6);

    // Grime does not only sit in holes, it runs out of them. A face sheltered from above keeps
    // what has washed down onto it, and a face open to the sky does not -- so the streak is the
    // product of "cannot see the sky" and "is not horizontal". On this flange that is the whole
    // outer rim under the top lip, which occlusion alone reports as fully open, because occlusion
    // is not directional and the rim genuinely is open sideways.
    //
    // Still nothing but the distance field: the sky term is one march straight up.
    float streak = (1.0 - sky) * (1.0 - f.upFacing * f.upFacing);
    streak *= lerp(0.35, 1.0, 1.0 - f.ao);
    grime = saturate(max(grime, streak * 0.62));

    s.albedo = lerp(s.albedo, kGrime, grime);
    s.roughness = lerp(s.roughness, 0.94, grime);
    s.metallic = lerp(s.metallic, 0.0, grime);

    // Dust, on open horizontal faces. Worn edges shed it, and so does anything vertical.
    float dust = f.upFacing * f.upFacing * f.upFacing;
    dust *= smoothstep(0.30, 0.85, f.ao);
    dust *= (1.0 - edge);
    dust = saturate(dust * 0.55);
    s.albedo = lerp(s.albedo, kDust, dust);
    s.roughness = lerp(s.roughness, 0.96, dust);
    s.metallic = lerp(s.metallic, 0.0, dust);

    // Thin sections warm slightly, standing in for the light that would pass through them.
    float thin = saturate(1.0 - f.thickness);
    thin *= thin;
    s.emissive = kScatter * thin * 0.20 * (1.0 - grime);

    return s;
}

float3 mkShade(Surface s, GeoFields fields, float3 p, float3 v) {
    float3 n = fields.normal;

    // Key from above and behind the camera, plus a dimmer fill from the opposite side so the
    // shadowed half is readable rather than black.
    float3 keyDir = kKeyDir;
    float3 fillDir = kFillDir;

    float shadowBias = gFarDist * 2.0e-3;
    float keyShadow = mkSoftShadow(p + n * shadowBias, keyDir, shadowBias, gFarDist * 0.9, 12.0, 32u);

    float3 col = mkDirectLight(s, n, v, keyDir, float3(4.2, 4.05, 3.8) * keyShadow);
    col += mkDirectLight(s, n, v, fillDir, float3(0.42, 0.46, 0.55));

    // Ambient, occluded. Metal takes it as a reflection, dielectric as diffuse.
    float3 r = reflect(-v, n);
    float3 env = mkEnvironment(r);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), s.albedo, s.metallic);
    float3 fresnel = mkFresnel(saturate(dot(n, v)), f0);
    float3 specAmbient = env * fresnel * (1.0 - s.roughness * 0.85) * 0.75;
    float3 diffAmbient = mkEnvironment(n) * s.albedo * (1.0 - s.metallic) * 0.40;

    col += (specAmbient + diffAmbient) * fields.ao;
    col += s.emissive;
    return col;
}

// ---------------------------------------------------------------- passes

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

    // Every threshold scales off the scene size, so a model authored in millimetres and one in
    // metres both resolve without a constant tuned per file.
    float hitEps = gFarDist * 3.0e-5;
    FieldScales scales = mkFieldScales(gSceneRadius);

    float t = 0.0;
    bool hit = false;
    for (uint s = 0; s < gMaxSteps; ++s) {
        float d = evalCsg(gEye + rd * t);
        if (d < hitEps) { hit = true; break; }
        // Difference is max(a,-b), only a lower bound on the true distance, so a full step can
        // tunnel through a seam (PLAN.md R-03).
        t += d * gStepScale;
        if (t > gFarDist) break;
    }

    // The floor is analytic, not part of the CSG. Folding it into evalCsg would drag it into every
    // geometry field as well: an infinite plane under the model reads as a huge unoccluded
    // up-facing surface, so the part would collect dust from a floor it is merely standing on.
    float groundT = (gGroundY - gEye.y) / (rd.y - 1e-9);
    bool groundHit = groundT > 0.0 && groundT < gFarDist && (!hit || groundT < t);

    if (groundHit) {
        float3 gp = gEye + rd * groundT;
        float3 gn = float3(0.0, 1.0, 0.0);
        float shadowBias = gFarDist * 2.0e-3;

        Surface floorMat;
        floorMat.albedo = float3(0.115, 0.112, 0.108);
        floorMat.roughness = 0.88;
        floorMat.metallic = 0.0;
        floorMat.emissive = float3(0.0, 0.0, 0.0);

        // Two different shadows, and the part needs both.
        //
        // The cast shadow depends on where the light is, so it falls away from the camera and can
        // be entirely out of frame -- which is exactly what the first attempt produced, a floor
        // with nothing on it.
        //
        // The contact term does not: it is the distance from this patch of floor to the nearest
        // surface of the part, so it darkens the floor *under* the part no matter which way the
        // light points. That is the term that makes an object sit on a surface rather than hover
        // over one, and it is free here because the distance to the part is what the field
        // already is. A mesh renderer has to approximate it with a blurred depth buffer.
        float cast = mkSoftShadow(gp + gn * shadowBias, kKeyDir, shadowBias, gFarDist * 0.9, 9.0, 32u);
        float contact = saturate(evalCsg(gp) / (gSceneRadius * 0.55));
        contact = 0.20 + 0.80 * contact * contact;

        float3 col = mkDirectLight(floorMat, gn, -rd, kKeyDir, float3(4.2, 4.05, 3.8) * cast);
        col += mkDirectLight(floorMat, gn, -rd, kFillDir, float3(0.42, 0.46, 0.55));
        col += mkEnvironment(gn) * floorMat.albedo * 0.40;
        col *= contact;

        // The floor is a plinth, not a landscape. An unbounded plane under a camera that looks
        // down fills the whole frame, and then the part is a small bright thing on a large grey
        // thing. Ending it inside two and a half radii turns it into a pool of light instead, and
        // the falloff doubles as a vignette.
        float radial = length(gp.xz - gCenter.xz) / gSceneRadius;
        float fade = smoothstep(1.4, 2.5, radial);
        col = lerp(col, mkBackground(rd), fade);

        col = col / (col + 0.85);
        return float4(pow(saturate(col), 1.0 / 2.2), 1.0);
    }

    if (!hit) {
        return float4(pow(saturate(mkBackground(rd)), 1.0 / 2.2), 1.0);
    }

    float3 p = gEye + rd * t;
    GeoFields fields = mkGeoFields(p, scales.normalEps, scales.curvatureEps, scales.aoReach,
                                  scales.thicknessReach);
    float skyBias = gFarDist * 2.0e-3;
    float sky = mkSoftShadow(p + fields.normal * skyBias, float3(0.0, 1.0, 0.0), skyBias,
                             gFarDist * 0.35, 4.0, 14u);
    Surface s = mkWeather(fields, sky);
    float3 col = mkShade(s, fields, p, -rd);

    // Filmic-ish curve: a plain clamp blows out every polished edge into a white blob.
    col = col / (col + 0.85);
    return float4(pow(saturate(col), 1.0 / 2.2), 1.0);
}

#endif  // MAKINA_SCENE_WEATHERED_HLSL
