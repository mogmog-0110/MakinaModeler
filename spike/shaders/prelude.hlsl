// Shared by both evaluation strategies: the interpreter and the generated per-scene shader.
// Everything here is independent of how the CSG tree is walked.

#ifndef MAKINA_PRELUDE_HLSL
#define MAKINA_PRELUDE_HLSL

#define OP_MASK          0xFFu
#define NEGATED_FLAG     0x100u

#define OP_SPHERE   0u
#define OP_BOX      1u
#define OP_CYLINDER 2u
#define OP_TORUS    3u
#define OP_FIRST_BOOLEAN 16u
#define OP_UNION        16u
#define OP_DIFFERENCE   17u
#define OP_INTERSECTION 18u

struct NodeHeader {
    float3 center;
    uint   opFlags;
    float3 halfExtent;
    float  boundRadius;
};

struct NodePayload {
    float4 params;   // primitive dimensions; .w carries the uniform scale correction
    float4 invRow0;  // world -> local
    float4 invRow1;
    float4 invRow2;
};

StructuredBuffer<NodeHeader>  gHeaders  : register(t0);
StructuredBuffer<NodePayload> gPayloads : register(t1);

cbuffer Params : register(b0) {
    float3 gEye;        float gTanHalfFov;
    float3 gForward;    float gAspect;
    float3 gRight;      uint  gNodeCount;
    float3 gUp;         uint  gMaxSteps;
    float3 gLightDir;   float gStepScale;
    float  gFarDist;    uint  gEnableAO;  uint gEnableCull;  float _cbPad;
};

// ---------------------------------------------------------------- primitive distance functions

float sdSphere(float3 p, float r) {
    return length(p) - r;
}

float sdBox(float3 p, float3 b) {
    float3 q = abs(p) - b;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
}

float sdCylinder(float3 p, float r, float halfHeight) {
    float2 d = float2(length(p.xz) - r, abs(p.y) - halfHeight);
    return min(max(d.x, d.y), 0.0) + length(max(d, 0.0));
}

float sdTorus(float3 p, float major, float minor) {
    float2 q = float2(length(p.xz) - major, p.y);
    return length(q) - minor;
}

float sdAabb(float3 p, float3 center, float3 halfExtent) {
    float3 q = abs(p - center) - halfExtent;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
}

#endif  // MAKINA_PRELUDE_HLSL
