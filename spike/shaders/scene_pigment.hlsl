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

/// Must match makina::Pigment byte for byte (Scene.hpp asserts the size on the CPU side).
struct MkPigment {
    uint   type;
    float3 colorA;
    float3 colorB;
    float3 scale;
    float3 translate;
    float3 axis;
};

StructuredBuffer<MkPigment> gPigments : register(t2);

#define MK_PIGMENT_NONE     0
#define MK_PIGMENT_CHECKER  1
#define MK_PIGMENT_GRADIENT 2
#define MK_PIGMENT_RADIAL   3

/// Where in the pattern a world point falls, 0..1.
///
/// POV applies a pigment's transform to the *pattern*, so a `scale 2` makes the squares twice as
/// big -- the point is divided, not multiplied. Getting this backwards produces a texture that
/// responds to the scale in the wrong direction, which reads as a units mistake rather than as an
/// inverted transform.
float mkPatternAt(MkPigment g, float3 wp) {
    const float3 p = (wp - g.translate) / g.scale;

    if (g.type == MK_PIGMENT_CHECKER) {
        // POV's checker is the parity of the three floors, and it is a hard edge: there is no
        // blend between the two colors, which is what makes it a checker and not a gradient.
        const float3 f = floor(p);
        return fmod(abs(f.x + f.y + f.z), 2.0) < 1.0 ? 0.0 : 1.0;
    }

    if (g.type == MK_PIGMENT_GRADIENT) {
        // The fractional part along the axis, so the ramp repeats every unit and runs 0 -> 1 -> 0.
        // POV's gradient is a sawtooth, not a triangle: it snaps back rather than reversing.
        const float3 a = normalize(g.axis);
        return frac(dot(p, a));
    }

    if (g.type == MK_PIGMENT_RADIAL) {
        // POV measures the angle about Y starting from -Z, counting the same way its left-handed
        // scene does. Written out rather than left to atan2's own zero, which is +X.
        const float ang = atan2(p.x, -p.z);
        return frac(ang / 6.28318530717958647692 + 1.0);
    }

    return 0.0;
}

/// The two-stop color_map POV would interpolate.
///
/// Linear between the stops, which is what POV does between two entries of a color_map. Checker
/// never lands between them, so its hard edge survives this unchanged.
float3 mkPigmentColorAt(MkPigment g, float3 wp) {
    return lerp(g.colorA, g.colorB, saturate(mkPatternAt(g, wp)));
}

/// The pigment a material names, or its flat color when it names none.
///
/// An index past the end falls back rather than clamping, for the reason mkMaterialAt does: a
/// clamp would paint the surface with pigment 0 and look like an authoring choice.
float3 mkSurfaceColor(MkMaterial m, float textureIndex, float3 wp) {
    const int i = (int)textureIndex;
    if (i < 0 || (uint)i >= gPigmentCount) {
        return m.diffuseColor;
    }
    return mkPigmentColorAt(gPigments[i], wp);
}

#endif  // MAKINA_SCENE_PIGMENT_HLSL
