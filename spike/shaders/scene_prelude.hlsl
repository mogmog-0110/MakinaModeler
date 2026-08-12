// GPU side of the evaluation program.
//
// The primitive distance functions are not redefined here: this includes makina-core's Sdf.hpp,
// the same file the CPU evaluator compiles. That is decision D-03 made real -- there is one
// spelling of "what is a torus", so the picture and the measurement cannot disagree about it.

#ifndef MAKINA_SCENE_PRELUDE_HLSL
#define MAKINA_SCENE_PRELUDE_HLSL

#include "Sdf.hpp"

cbuffer Params : register(b0) {
    float3 gEye;        float gTanHalfFov;
    float3 gForward;    float gAspect;
    float3 gRight;      uint  gNodeCount;
    float3 gUp;         uint  gMaxSteps;
    float3 gLightDir;   float gStepScale;
    float  gFarDist;    uint  gEnableAO;  uint gDebugMode;  float gGroundY;
    float3 gCenter;     float gSceneRadius;
    uint   gProgramCount; uint gMaterialCount; uint _cbPad[2];
    // Selection highlight, as a world box. The viewport sets it; the offscreen renderer leaves
    // gSelValid at zero, so both use the same layout and the same generated shaders.
    float3 gSelMin;     float gSelValid;
    float3 gSelMax;     float _cbPad2;
};

#endif  // MAKINA_SCENE_PRELUDE_HLSL
