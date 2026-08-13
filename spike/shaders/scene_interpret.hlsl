// evalCsg as a loop over the evaluation program, instead of straight-line generated code.
//
// One shader for every scene. The program arrives in a StructuredBuffer, so changing the model is
// a buffer upload rather than a shader compile -- which is the whole reason this exists. The
// modeller can afford to call DXC on every edit (a few hundred milliseconds, once, while the user
// is still dragging); a game cannot ship a shader compiler to load a prop.
//
// Phase S measured a version of this against generated code on a synthetic scene and it lost by
// 4.9x, which is why codegen is the modeller's path (PLAN.md D-04). What that measurement could
// not say is what it costs on the models people actually make, and that is what this is for.
//
// The stack is why it is slower. Generated code is SSA -- every intermediate is a register. Here
// the compiler sees an array indexed by a runtime value and puts it in scratch memory, and scratch
// is off-chip.
//
// Requires: StructuredBuffer<EvalNode> gProgram, uint gProgramCount (scene_prelude.hlsl).

#ifndef MAKINA_SCENE_INTERPRET_HLSL
#define MAKINA_SCENE_INTERPRET_HLSL

#include "Sdf.hpp"

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

/// Deepest stack any program can need. Balanced folding keeps this logarithmic in the node count
/// (Flatten.hpp), so 24 covers a scene far larger than the node array can hold.
#define MK_STACK_MAX 24

float evalCsg(float3 wp) {
    if (gProgramCount == 0u) {
        return 1.0e30;   // nothing renderable in this scene
    }

    float stack[MK_STACK_MAX];
    int sp = 0;

    for (uint i = 0u; i < gProgramCount; ++i) {
        EvalNode n = gProgram[i];

        if (n.op >= 16u) {
            // Booleans. A malformed program would underflow here; the flattener guarantees it
            // cannot, and checking per node in the inner loop would cost more than it protects.
            float b = stack[sp - 1];
            float a = stack[sp - 2];
            sp -= 2;
            float r;
            if (n.op == 16u) {
                r = min(a, b);
            } else if (n.op == 17u) {
                r = max(a, -b);
            } else {
                r = max(a, b);
            }
            stack[sp++] = r;
            continue;
        }

        const float4 w = float4(wp, 1.0);
        const float3 p = float3(dot(n.inv0, w), dot(n.inv1, w), dot(n.inv2, w));

        float d;
        if (n.op == 0u) {
            d = mkSdSphere(p.x, p.y, p.z, n.params.x);
        } else if (n.op == 1u) {
            d = mkSdBoxCentered(p.x, p.y, p.z, n.params.x, n.params.y, n.params.z);
        } else if (n.op == 2u) {
            d = mkSdCylinderCentered(p.x, p.y, p.z, n.params.x, n.params.y);
        } else if (n.op == 3u) {
            // params.z carries the height sign, so a cone pointing down works without a branch.
            d = mkSdConeCentered(p.x, p.y * n.params.z, p.z, n.params.x, n.params.y);
        } else if (n.op == 4u) {
            d = mkSdTorus(p.x, p.y, p.z, n.params.x, n.params.y);
        } else {
            d = mkSdPlane(p.y, 0.0);
        }

        stack[sp++] = d * n.params.w;
    }

    return stack[0];
}

/// The same walk, carrying which material won.
///
/// Separate from evalCsg for the reason the generated shader has two functions: the march calls
/// the first thousands of times per pixel and this one once. Here the cost is a second stack,
/// which is a second `alloca` -- so calling this per step would be worse than in generated code,
/// not better.
///
/// The boolean rules match scene_codegen.hpp exactly, including Difference keeping the left
/// operand's material for the cut surface. Two implementations of the same rule is the risk this
/// project is built to catch, and the interpreted and generated pictures are compared for it.
float3 evalCsgMaterial(float3 wp) {
    if (gProgramCount == 0u) {
        return float3(1.0e30, 255.0, -1.0);
    }

    float stack[MK_STACK_MAX];
    float ids[MK_STACK_MAX];
    // The pattern travels with the material and by the same rule, because a surface that took
    // one operand's color and the other's pattern would be a mix neither renderer draws.
    float pigs[MK_STACK_MAX];
    int sp = 0;

    for (uint i = 0u; i < gProgramCount; ++i) {
        EvalNode n = gProgram[i];

        if (n.op >= 16u) {
            float b = stack[sp - 1];
            float a = stack[sp - 2];
            float ib = ids[sp - 1];
            float ia = ids[sp - 2];
            float gb = pigs[sp - 1];
            float ga = pigs[sp - 2];
            sp -= 2;
            float r;
            float id;
            float pg;
            if (n.op == 16u) {
                r = min(a, b);
                id = a < b ? ia : ib;
                pg = a < b ? ga : gb;
            } else if (n.op == 17u) {
                r = max(a, -b);
                id = ia;
                pg = ga;
            } else {
                r = max(a, b);
                id = a > b ? ia : ib;
                pg = a > b ? ga : gb;
            }
            stack[sp] = r;
            ids[sp] = id;
            pigs[sp] = pg;
            ++sp;
            continue;
        }

        const float4 w = float4(wp, 1.0);
        const float3 p = float3(dot(n.inv0, w), dot(n.inv1, w), dot(n.inv2, w));

        float d;
        if (n.op == 0u) {
            d = mkSdSphere(p.x, p.y, p.z, n.params.x);
        } else if (n.op == 1u) {
            d = mkSdBoxCentered(p.x, p.y, p.z, n.params.x, n.params.y, n.params.z);
        } else if (n.op == 2u) {
            d = mkSdCylinderCentered(p.x, p.y, p.z, n.params.x, n.params.y);
        } else if (n.op == 3u) {
            d = mkSdConeCentered(p.x, p.y * n.params.z, p.z, n.params.x, n.params.y);
        } else if (n.op == 4u) {
            d = mkSdTorus(p.x, p.y, p.z, n.params.x, n.params.y);
        } else {
            d = mkSdPlane(p.y, 0.0);
        }

        stack[sp] = d * n.params.w;
        ids[sp] = (float)n.materialId;
        // 0xFFFFFFFF as a float is not -1, so the sentinel is turned into one here rather
        // than left to a cast that would hand the lookup a very large index.
        pigs[sp] = n.pigmentId == 0xFFFFFFFFu ? -1.0 : (float)n.pigmentId;
        ++sp;
    }

    return float3(stack[0], ids[0], pigs[0]);
}

#endif  // MAKINA_SCENE_INTERPRET_HLSL
