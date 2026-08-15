// POV-Ray pigment patterns, as arithmetic.
//
// Only the patterns that have no noise in them. POV's marble, wood, granite and bozo all read the
// same permutation table, and without that exact table a pattern is "something that looks like
// marble" -- which cannot be compared against the oracle at all. The comparison is the reason this
// renderer is worth anything, so a pattern that breaks it is not a feature.
//
// Everything here is a pure function of the point, so a hit gives one lookup and no state. That is
// the property that makes procedural texture and sphere tracing suit each other: there is nowhere
// to put a UV even if we wanted one.
//
// Requires: StructuredBuffer<MkPigment> gPigments and uint gPigmentCount (scene_finish.hlsl).

#ifndef MAKINA_SCENE_PIGMENT_HLSL
#define MAKINA_SCENE_PIGMENT_HLSL

/// Must match makina::GpuPigment byte for byte (Flatten.hpp asserts the size).
///
/// Not makina::Pigment: the table is built by the flatten, not copied from the scene, because
/// a pattern needs the space of the object wearing it and the same pattern on two objects in
/// different places is two entries.
struct MkPigment {
    /// PigmentType in the low byte, stopCount in the next: the C++ side packs both into one word.
    uint   typeAndStops;
    float3 scale;
    float3 translate;
    float3 axis;
    float3 _pad1;
    float3 _pad2;
    /// The color_map, {r, g, b, position}, ascending; only the first stop count are read.
    float4 stop[8];
    /// world -> the space the pattern was authored in, three rows of four.
    float4 inv0;
    float4 inv1;
    float4 inv2;
};

StructuredBuffer<MkPigment> gPigments : register(t2);

#define MK_PIGMENT_NONE     0
#define MK_PIGMENT_CHECKER  1
#define MK_PIGMENT_GRADIENT 2
#define MK_PIGMENT_RADIAL   3

/// Where in the pattern a world point falls, 0..1.
///
/// Two transforms, in POV's order. First the object's: POV carries a texture along with the
/// solid it is on, so the point is taken out of world space and into the space the pattern was
/// authored in -- without this a wall that was moved slides through a pattern pinned to the
/// world, and a 0.45 checker comes out a whole square off. Then the pigment's own: POV applies
/// that one to the *pattern*, so a `scale 2` makes the squares twice as big and the point is
/// divided rather than multiplied. Getting the second backwards reads as a units mistake
/// rather than as an inverted transform.
float mkPatternAt(MkPigment g, float3 wp) {
    const float4 w = float4(wp, 1.0);
    const float3 op = float3(dot(g.inv0, w), dot(g.inv1, w), dot(g.inv2, w));
    const float3 p = (op - g.translate) / g.scale;

    const uint type = g.typeAndStops & 0xffu;
    if (type == MK_PIGMENT_CHECKER) {
        // POV's checker is the parity of the three floors, and it is a hard edge: there is no
        // blend between the two colors, which is what makes it a checker and not a gradient.
        const float3 f = floor(p);
        return fmod(abs(f.x + f.y + f.z), 2.0) < 1.0 ? 0.0 : 1.0;
    }

    if (type == MK_PIGMENT_GRADIENT) {
        // The fractional part along the axis, so the ramp repeats every unit and runs 0 -> 1 -> 0.
        // POV's gradient is a sawtooth, not a triangle: it snaps back rather than reversing.
        const float3 a = normalize(g.axis);
        return frac(dot(p, a));
    }

    if (type == MK_PIGMENT_RADIAL) {
        // POV measures the angle about Y starting from -Z, counting the same way its left-handed
        // scene does. Written out rather than left to atan2's own zero, which is +X.
        const float ang = atan2(p.x, -p.z);
        return frac(ang / 6.28318530717958647692 + 1.0);
    }

    return 0.0;
}

/// The color_map, read the way POV reads it -- measured, spike/pov_colormap_probe.py: linear in
/// the pattern value between neighbouring stops, clamped to the end colors outside them.
///
/// Checker's pattern value is exactly 0 or 1 and its two stops sit there, so its hard edge
/// survives this unchanged. The loop is over a fixed 8 so it unrolls; the stop count only
/// decides where the clamp at the far end lands.
float3 mkPigmentColorAt(MkPigment g, float3 wp) {
    const float v = mkPatternAt(g, wp);
    const uint count = (g.typeAndStops >> 8u) & 0xffu;
    float3 col = g.stop[0].rgb;
    [unroll]
    for (uint i = 1; i < 8; ++i) {
        if (i < count) {
            const float4 lo = g.stop[i - 1];
            const float4 hi = g.stop[i];
            const float t = saturate((v - lo.w) / max(hi.w - lo.w, 1e-6));
            // Past this stop the next iteration overwrites; before the first, t is 0 and the
            // first color stands, which is the clamp POV applies below the map.
            if (v >= lo.w) {
                col = lerp(lo.rgb, hi.rgb, t);
            }
        }
    }
    return col;
}

/// The pigment this surface wears, or the material's flat color when it wears none.
///
/// The index comes from the surface rather than from the material, because it selects a
/// (pattern, object space) pair and the material only knows the pattern.
///
/// An index past the end falls back rather than clamping, for the reason mkMaterialAt does: a
/// clamp would paint the surface with pigment 0 and look like an authoring choice.
float3 mkSurfaceColor(MkMaterial m, float pigmentIndex, float3 wp) {
    const int i = (int)pigmentIndex;
    if (i < 0 || (uint)i >= gPigmentCount) {
        return m.diffuseColor;
    }
    return mkPigmentColorAt(gPigments[i], wp);
}

#endif  // MAKINA_SCENE_PIGMENT_HLSL
