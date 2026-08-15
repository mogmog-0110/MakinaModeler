// GPU side of the evaluation program.
//
// The primitive distance functions are not redefined here: this includes makina-core's Sdf.hpp,
// the same file the CPU evaluator compiles. That is decision D-03 made real -- there is one
// spelling of "what is a torus", so the picture and the measurement cannot disagree about it.

#ifndef MAKINA_SCENE_PRELUDE_HLSL
#define MAKINA_SCENE_PRELUDE_HLSL

#include "Sdf.hpp"
// Domain warps (D-14): the same inverse maps the CPU evaluator applies, so a twisted solid is
// one twist in two compilers rather than two twists that mostly agree.
#include "Warp.hpp"

// The smallest distance correction any leaf carries, from the generated header when the scene
// has one. The field is a lower bound shrunk by it (a warp's 1/L, a non-uniform Scale's smallest
// axis), so every test of the form "is the field smaller than this length" -- the shadow and
// mirror rays leaving the surface, ambient occlusion -- has to be made in the field's units, or
// a shrunk field reads as solid shadow: a bent bar came out black under a POV-matched light.
#ifndef MK_MIN_CORRECTION
#define MK_MIN_CORRECTION 1.0
#endif

cbuffer Params : register(b0) {
    float3 gEye;        float gTanHalfFov;
    float3 gForward;    float gAspect;
    float3 gRight;      uint  gNodeCount;
    float3 gUp;         uint  gMaxSteps;
    float3 gLightDir;   float gStepScale;
    float  gFarDist;    uint  gEnableAO;  uint gDebugMode;  float gGroundY;
    float3 gCenter;     float gSceneRadius;
    uint   gProgramCount; uint gMaterialCount; uint gPigmentCount;
    /// Drops everything POV-Ray has no equivalent for -- the rim term and the sky background.
    ///
    /// Set only by the pass that is about to be compared against a POV render. Those terms are
    /// there because they make a modelling viewport readable, not because they are physics, and
    /// leaving them in would put a difference in every pixel that the comparison would then have
    /// to be loosened to forgive.
    uint   gPovMatch;
    // Scalars, not an array. An array in a constant buffer puts every element on its own 16-byte
    // boundary, so `uint pad[3]` is 48 bytes here and 12 on the CPU -- and everything declared
    // after it reads from the wrong place, silently, with plausible values.
    uint   gLightCount;
    /// 0 perspective, 1 orthographic, 2 fisheye, 3 ultra wide angle, 4 panoramic.
    uint   gCameraKind;
    /// The full field of view in radians for the wide cameras, or the half-width in world units
    /// for the orthographic one. One slot because no camera needs both.
    float  gCameraAngle;  uint _cbPadA;
    // Selection highlight, as a world box. The viewport sets it; the offscreen renderer leaves
    // gSelValid at zero, so both use the same layout and the same generated shaders.
    float3 gSelMin;     float gSelValid;
    float3 gSelMax;     float _cbPad2;
};

#endif  // MAKINA_SCENE_PRELUDE_HLSL
