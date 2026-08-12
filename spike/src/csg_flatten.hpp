// Phase S spike: authoring nodes -> evaluation program (decision D-01).
//
// Three things happen here that the GPU cannot do for itself:
//   1. transforms are baked into each primitive, so the shader sees primitives and booleans only
//   2. a world AABB is computed per subtree, which is what makes node-level culling possible
//   3. each node's sign polarity is resolved
//
// Polarity matters and is easy to get wrong. Sphere tracing needs the evaluated distance to be a
// conservative *lower* bound, or a step can overshoot the surface. Difference is max(a, -b), so a
// lower bound on the result needs a lower bound on a and an *upper* bound on b. The AABB distance
// is a lower bound, so substituting it on a subtracted branch produces an overestimate and the ray
// tunnels. Subtracted branches therefore fall back to a bounding-sphere upper bound instead.

#pragma once

#include "csg_scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace spike {

// Must match struct NodeHeader in raymarch.hlsl. 32 bytes.
struct NodeHeader {
    float         center[3];
    std::uint32_t opFlags;      // bits 0-7: Op, bit 8: negated
    float         halfExtent[3];
    float         boundRadius;  // bounding-sphere radius about center
};
static_assert(sizeof(NodeHeader) == 32, "NodeHeader must stay in step with the HLSL declaration");

// Must match struct NodePayload in raymarch.hlsl. 64 bytes. Only read when a node survives
// culling, which is the point of keeping it in a separate buffer.
struct NodePayload {
    float params[4];   // dimensions; [3] carries the uniform scale correction
    float invRow0[4];  // world -> local
    float invRow1[4];
    float invRow2[4];
};
static_assert(sizeof(NodePayload) == 64, "NodePayload must stay in step with the HLSL declaration");

constexpr std::uint32_t kNegatedFlag = 0x100u;

struct FlatProgram {
    std::vector<NodeHeader>  headers;
    std::vector<NodePayload> payloads;
};

namespace detail {

struct Aabb {
    float lo[3]{ std::numeric_limits<float>::max(),  std::numeric_limits<float>::max(),  std::numeric_limits<float>::max()};
    float hi[3]{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};

    void include(float x, float y, float z) {
        lo[0] = std::min(lo[0], x);  hi[0] = std::max(hi[0], x);
        lo[1] = std::min(lo[1], y);  hi[1] = std::max(hi[1], y);
        lo[2] = std::min(lo[2], z);  hi[2] = std::max(hi[2], z);
    }
};

inline Aabb unite(const Aabb& a, const Aabb& b) {
    Aabb r;
    for (int i = 0; i < 3; ++i) {
        r.lo[i] = std::min(a.lo[i], b.lo[i]);
        r.hi[i] = std::max(a.hi[i], b.hi[i]);
    }
    return r;
}

inline Aabb intersect(const Aabb& a, const Aabb& b) {
    Aabb r;
    for (int i = 0; i < 3; ++i) {
        r.lo[i] = std::max(a.lo[i], b.lo[i]);
        r.hi[i] = std::min(a.hi[i], b.hi[i]);
        if (r.lo[i] > r.hi[i]) {  // disjoint: collapse rather than leave it inverted
            const float mid = 0.5f * (r.lo[i] + r.hi[i]);
            r.lo[i] = r.hi[i] = mid;
        }
    }
    return r;
}

inline void rotationMatrix(const Vec3& deg, float m[9]) {
    const float k = 3.14159265358979323846f / 180.0f;
    const float cx = std::cos(deg.x * k), sx = std::sin(deg.x * k);
    const float cy = std::cos(deg.y * k), sy = std::sin(deg.y * k);
    const float cz = std::cos(deg.z * k), sz = std::sin(deg.z * k);

    // R = Rz * Ry * Rx, row-major.
    m[0] = cz * cy;  m[1] = cz * sy * sx - sz * cx;  m[2] = cz * sy * cx + sz * sx;
    m[3] = sz * cy;  m[4] = sz * sy * sx + cz * cx;  m[5] = sz * sy * cx - cz * sx;
    m[6] = -sy;      m[7] = cy * sx;                 m[8] = cy * cx;
}

// Half extents of the primitive in its own local space.
inline void localHalfExtent(Op op, const float p[3], float out[3]) {
    switch (op) {
        case Op::Sphere:
            out[0] = out[1] = out[2] = p[0];
            break;
        case Op::Box:
            out[0] = p[0];  out[1] = p[1];  out[2] = p[2];
            break;
        case Op::Cylinder:
            out[0] = p[0];  out[1] = p[1];  out[2] = p[0];
            break;
        case Op::Torus:
            out[0] = out[2] = p[0] + p[1];
            out[1] = p[1];
            break;
        default:
            throw std::logic_error("localHalfExtent called on a non-primitive node");
    }
}

// World AABB of a transformed primitive, from the eight corners of its local box.
inline Aabb worldAabb(Op op, const float params[3], const Transform& xf) {
    float h[3];
    localHalfExtent(op, params, h);

    float r[9];
    rotationMatrix(xf.rotationDeg, r);

    Aabb box;
    for (int corner = 0; corner < 8; ++corner) {
        const float lx = ((corner & 1) ? h[0] : -h[0]) * xf.scale;
        const float ly = ((corner & 2) ? h[1] : -h[1]) * xf.scale;
        const float lz = ((corner & 4) ? h[2] : -h[2]) * xf.scale;

        box.include(r[0] * lx + r[1] * ly + r[2] * lz + xf.translation.x,
                    r[3] * lx + r[4] * ly + r[5] * lz + xf.translation.y,
                    r[6] * lx + r[7] * ly + r[8] * lz + xf.translation.z);
    }
    return box;
}

// world -> local is  R^T * (p - t) / s, since local -> world is  R * s * p + t.
inline void bakeInverse(const Transform& xf, NodePayload& out) {
    float r[9];
    rotationMatrix(xf.rotationDeg, r);

    const float inv = 1.0f / xf.scale;
    const Vec3& t = xf.translation;

    // R^T rows are R columns.
    const float rows[3][3] = {
        {r[0] * inv, r[3] * inv, r[6] * inv},
        {r[1] * inv, r[4] * inv, r[7] * inv},
        {r[2] * inv, r[5] * inv, r[8] * inv},
    };

    float* dst[3] = {out.invRow0, out.invRow1, out.invRow2};
    for (int i = 0; i < 3; ++i) {
        dst[i][0] = rows[i][0];
        dst[i][1] = rows[i][1];
        dst[i][2] = rows[i][2];
        dst[i][3] = -(rows[i][0] * t.x + rows[i][1] * t.y + rows[i][2] * t.z);
    }

    // Distances are measured in local space, so scale them back up to world.
    out.params[3] = xf.scale;
}

}  // namespace detail

// Turns the authoring list into the evaluation program. Throws on a malformed RPN sequence.
inline FlatProgram flatten(const std::vector<SceneNode>& scene) {
    const std::size_t n = scene.size();
    if (n == 0) {
        throw std::invalid_argument("flatten: the scene is empty");
    }

    std::vector<detail::Aabb> bounds(n);
    std::vector<int> childA(n, -1);
    std::vector<int> childB(n, -1);

    // Pass 1: bounds bottom-up, and remember each boolean's two operand roots.
    std::vector<int> stack;
    stack.reserve(n);

    for (std::size_t i = 0; i < n; ++i) {
        const SceneNode& s = scene[i];

        if (!isBoolean(s.op)) {
            bounds[i] = detail::worldAabb(s.op, s.params, s.xf);
            stack.push_back(static_cast<int>(i));
            continue;
        }

        if (stack.size() < 2) {
            throw std::invalid_argument("flatten: boolean node with fewer than two operands");
        }
        const int b = stack.back();  stack.pop_back();
        const int a = stack.back();  stack.pop_back();
        childA[i] = a;
        childB[i] = b;

        switch (s.op) {
            case Op::Union:        bounds[i] = detail::unite(bounds[a], bounds[b]);     break;
            case Op::Intersection: bounds[i] = detail::intersect(bounds[a], bounds[b]); break;
            default:               bounds[i] = bounds[a];  // subtracting cannot grow the result
                break;
        }
        stack.push_back(static_cast<int>(i));
    }

    if (stack.size() != 1) {
        throw std::invalid_argument("flatten: the RPN sequence does not reduce to a single root");
    }

    // Pass 2: polarity top-down. Indices in RPN order always place a node after its children, so
    // walking backwards from the root visits every parent before its operands.
    std::vector<std::uint8_t> negated(n, 0);
    for (std::size_t k = n; k-- > 0;) {
        if (childA[k] < 0) {
            continue;
        }
        negated[childA[k]] = negated[k];
        negated[childB[k]] = static_cast<std::uint8_t>(
            scene[k].op == Op::Difference ? (negated[k] ^ 1u) : negated[k]);
    }

    // Pass 3: emit.
    FlatProgram out;
    out.headers.resize(n);
    out.payloads.resize(n);

    for (std::size_t i = 0; i < n; ++i) {
        NodeHeader& h = out.headers[i];
        const detail::Aabb& b = bounds[i];

        float radius = 0.0f;
        for (int axis = 0; axis < 3; ++axis) {
            h.center[axis] = 0.5f * (b.lo[axis] + b.hi[axis]);
            h.halfExtent[axis] = std::max(0.0f, 0.5f * (b.hi[axis] - b.lo[axis]));
            radius += h.halfExtent[axis] * h.halfExtent[axis];
        }
        h.boundRadius = std::sqrt(radius);
        h.opFlags = static_cast<std::uint32_t>(scene[i].op) | (negated[i] ? kNegatedFlag : 0u);

        NodePayload& p = out.payloads[i];
        p.params[0] = scene[i].params[0];
        p.params[1] = scene[i].params[1];
        p.params[2] = scene[i].params[2];
        p.params[3] = 1.0f;
        if (!isBoolean(scene[i].op)) {
            detail::bakeInverse(scene[i].xf, p);
        }
    }

    return out;
}

}  // namespace spike
