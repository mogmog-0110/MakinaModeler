// The evaluation program as the GPU sees it: one StructuredBuffer of EvalNodes at t0.
//
// Two readers. scene_interpret.hlsl walks it as an RPN program, one shader for every scene. A
// live generated shader (scene_codegen.hpp, PLAN.md D-15) is specialised to one tree's structure
// and reads only the numbers -- params and the inverse matrices -- from here, so the engine can
// upload a freshly sampled program every frame and a joint moves without a recompile.
//
// Requires: uint gProgramCount (scene_prelude.hlsl).

#ifndef MAKINA_SCENE_PROGRAM_HLSL
#define MAKINA_SCENE_PROGRAM_HLSL

/// Must match makina::EvalNode byte for byte (Flatten.hpp asserts the size on the CPU side).
struct EvalNode {
    uint  op;
    uint  materialId;
    uint  pigmentId;
    uint  _pad;
    float4 params;
    float4 inv0;
    float4 inv1;
    float4 inv2;
};

StructuredBuffer<EvalNode> gProgram : register(t0);

#endif
