// Phase S spike: authoring-side scene construction.
//
// This file produces the *authoring* representation (D-01): an RPN-ordered list of nodes that
// still carry their transforms as transforms. Turning that into what the GPU walks is
// csg_flatten.hpp's job.

#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace spike {

enum class Op : std::uint32_t {
    Sphere       = 0,
    Box          = 1,
    Cylinder     = 2,
    Torus        = 3,
    Union        = 16,
    Difference   = 17,
    Intersection = 18,
};

inline bool isBoolean(Op op) {
    return static_cast<std::uint32_t>(op) >= static_cast<std::uint32_t>(Op::Union);
}

struct Vec3 {
    float x, y, z;
};

// A rigid transform with one uniform scale. Enough for the spike; the real thing carries the
// full affine from the authoring tree.
struct Transform {
    Vec3  translation{0.0f, 0.0f, 0.0f};
    Vec3  rotationDeg{0.0f, 0.0f, 0.0f};
    float scale = 1.0f;
};

struct SceneNode {
    Op        op;
    Transform xf;
    float     params[3]{0.0f, 0.0f, 0.0f};
};

inline SceneNode primitive(Op op, const Transform& xf, float p0, float p1 = 0.0f, float p2 = 0.0f) {
    SceneNode n{};
    n.op = op;
    n.xf = xf;
    n.params[0] = p0;
    n.params[1] = p1;
    n.params[2] = p2;
    return n;
}

inline SceneNode boolean(Op op) {
    SceneNode n{};
    n.op = op;
    return n;
}

// A stand-in for the hero asset: a plate with holes bored through it, bosses added on top and a
// bead rolled underneath. The operation mix matters — a list of identical ops would understate
// the branch divergence the interpreter actually sees.
//
// primitiveCount counts primitives only; the returned list is primitives + booleans, so its
// size is 2 * primitiveCount - 1.
inline std::vector<SceneNode> buildMechanicalPart(int primitiveCount) {
    std::vector<SceneNode> nodes;
    nodes.reserve(static_cast<std::size_t>(primitiveCount) * 2);

    Transform base{};
    nodes.push_back(primitive(Op::Box, base, 1.15f, 0.28f, 1.15f));

    const int remaining = primitiveCount - 1;
    for (int i = 0; i < remaining; ++i) {
        const float a = 2.399963f * static_cast<float>(i);  // golden-angle spread
        const int span = remaining > 1 ? remaining - 1 : 1;
        const float rad = 0.30f + 0.62f * std::sqrt(static_cast<float>(i) / static_cast<float>(span));

        Transform xf{};
        xf.translation = {rad * std::cos(a), 0.0f, rad * std::sin(a)};

        switch (i % 4) {
            case 0:  // bored hole
                nodes.push_back(primitive(Op::Cylinder, xf, 0.085f, 0.55f));
                nodes.push_back(boolean(Op::Difference));
                break;
            case 1:  // spherical dimple
                xf.translation.y = 0.28f;
                nodes.push_back(primitive(Op::Sphere, xf, 0.13f));
                nodes.push_back(boolean(Op::Difference));
                break;
            case 2:  // raised boss
                xf.translation.y = 0.26f;
                nodes.push_back(primitive(Op::Cylinder, xf, 0.10f, 0.09f));
                nodes.push_back(boolean(Op::Union));
                break;
            default:  // rolled bead
                xf.translation.y = -0.26f;
                xf.rotationDeg = {0.0f, a * 18.0f, 0.0f};
                nodes.push_back(primitive(Op::Torus, xf, 0.11f, 0.035f));
                nodes.push_back(boolean(Op::Union));
                break;
        }
    }

    return nodes;
}

}  // namespace spike
