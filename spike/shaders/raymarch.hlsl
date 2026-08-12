// Interpreter variant: the tree arrives as a post-order (RPN) sequence in a StructuredBuffer and
// is walked with an explicit stack, because a GPU cannot recurse. This is decision D-04, now
// provisional pending the comparison against the generated variant.

#include "prelude.hlsl"

// Sized from the deepest tree the flattener will emit. The array is dynamically indexed, so the
// compiler cannot promote it to registers: DXIL shows `alloca [N x float]`, i.e. scratch.
//
// Measured: dropping this from 16 to 4 changed nothing beyond run-to-run noise (+-4%), so the
// *size* of the scratch array is not the lever. Whether its *existence* costs is what the
// generated variant answers, since there the stack becomes SSA values.
#ifndef MAX_STACK
#define MAX_STACK 16
#endif

// cullRadius: primitives whose bounding box is farther away than this substitute a cheap bound.
// Any value is correct; larger values trade distance accuracy (hence march steps) for speed.
//
// Measured in Phase S: culling gives no speedup at all, because the AABB test costs about as much
// as the primitive it skips. Kept behind gEnableCull so the result stays reproducible.
float evalCsg(float3 wp, float cullRadius) {
    float stack[MAX_STACK];
    int sp = 0;
    float4 wp1 = float4(wp, 1.0);

    for (uint i = 0; i < gNodeCount; ++i) {
        NodeHeader h = gHeaders[i];
        uint op = h.opFlags & OP_MASK;

        if (op >= OP_FIRST_BOOLEAN) {
            float b = stack[--sp];
            float a = stack[--sp];

            if      (op == OP_UNION)      stack[sp++] = min(a, b);
            else if (op == OP_DIFFERENCE) stack[sp++] = max(a, -b);
            else                          stack[sp++] = max(a, b);
            continue;
        }

        if (gEnableCull != 0u) {
            // Kept inside the branch so the no-cull baseline does not pay for the test.
            float dBox = sdAabb(wp, h.center, h.halfExtent);
            if (dBox > cullRadius) {
                // Far enough away that the exact shape cannot matter at this scale.
                if ((h.opFlags & NEGATED_FLAG) != 0u) {
                    // Subtracted: an upper bound keeps max(a, -b) a lower bound.
                    stack[sp++] = length(wp - h.center) + h.boundRadius;
                } else {
                    stack[sp++] = dBox;
                }
                continue;
            }
        }

        // Transforms are baked into each primitive on the CPU, so the tree the GPU walks holds
        // primitives and booleans only (see D-01).
        NodePayload pl = gPayloads[i];
        float3 p = float3(dot(pl.invRow0, wp1), dot(pl.invRow1, wp1), dot(pl.invRow2, wp1));

        float d;
        if      (op == OP_SPHERE)   d = sdSphere(p, pl.params.x);
        else if (op == OP_BOX)      d = sdBox(p, pl.params.xyz);
        else if (op == OP_CYLINDER) d = sdCylinder(p, pl.params.x, pl.params.y);
        else                        d = sdTorus(p, pl.params.x, pl.params.y);

        stack[sp++] = d * pl.params.w;
    }

    return stack[0];
}

#include "shading.hlsl"
