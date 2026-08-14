// The node vocabulary, and the mapping between a scene-JSON object and the flat params array.
//
// Keeping the parameter order in one table means the JSON reader, the JSON writer and the
// flattener cannot drift apart: they all index params[] through the same list.

#pragma once

#include <cstdint>
#include <cstring>

namespace makina {

enum class Op : std::uint8_t {
    // Structural — carry no geometry.
    SceneRoot = 0,
    Label     = 1,
    // Anything Grasp3D can express but Makina does not model. Kept so the node count and the
    // tree shape survive a round trip; skipped during evaluation (SCENE_FORMAT.md 3.5).
    Unsupported = 2,

    // Primitives.
    Box      = 16,
    Sphere   = 17,
    Cylinder = 18,
    Cone     = 19,
    Torus    = 20,
    Plane    = 21,
    Disc     = 22,
    Triangle = 23,

    // Transforms.
    Translate = 48,
    Rotate    = 49,
    Scale     = 50,

    // Booleans. n-ary: Difference takes children[0] minus the union of children[1..].
    Merge        = 64,
    Difference   = 65,
    Intersection = 66,

    // Fields (POV's blob). A Blob's children are components, not union members: each adds a
    // density that falls off with distance, and the surface is where the sum crosses the
    // threshold. Components may sit under transform nodes; a component outside a Blob is inert.
    Blob         = 80,
    BlobSphere   = 81,
    BlobCylinder = 82,

    // Revolved profiles (POV's sor). A Sor's children are SorPoints in file order: <radius,
    // height> control points of a cubic in r-squared over h, revolved about local Y. The first
    // and last points steer the end slopes and are not on the surface. A point outside a Sor is
    // inert, like a blob component outside a Blob.
    Sor      = 96,
    SorPoint = 97,
};

inline bool isPrimitive(Op op) {
    return op >= Op::Box && op <= Op::Triangle;
}

inline bool isTransform(Op op) {
    return op >= Op::Translate && op <= Op::Scale;
}

inline bool isBoolean(Op op) {
    return op >= Op::Merge && op <= Op::Intersection;
}

inline bool isBlobComponent(Op op) {
    return op == Op::BlobSphere || op == Op::BlobCylinder;
}

// ---------------------------------------------------------------- flags

namespace flags {

// Rotate is single-axis in Grasp3D (an axis plus one angle), not Euler — see CSG_NODE.md 4.2.
constexpr std::uint16_t kAxisMask = 0x0003;
constexpr std::uint16_t kAxisX    = 0;
constexpr std::uint16_t kAxisY    = 1;
constexpr std::uint16_t kAxisZ    = 2;

// Cone "Open ": an open cone has no end caps.
constexpr std::uint16_t kConeOpen = 0x0004;

// Muted: this node and everything under it take no part in the solid.
//
// Not "hidden". In a CSG tree there is no such thing as leaving a node out of the picture but
// in the shape: the shape *is* the picture. Mute the cutter of a difference and the hole fills
// in -- that is the operation, and calling it hiding would describe something this model
// cannot do. Edit.hpp's withoutMuted is the one place it is honoured, by handing every
// consumer a tree the muted subtrees have already left.
constexpr std::uint16_t kMuted    = 0x0008;

}  // namespace flags

// ---------------------------------------------------------------- op names

struct OpEntry {
    Op          op;
    const char* name;
    // Parameter names in params[] order. Ends at the first null.
    const char* keys[12];
};

inline const OpEntry* opTable(int& count) {
    static const OpEntry table[] = {
        {Op::SceneRoot,    "SceneRoot",    {nullptr}},
        {Op::Label,        "Label",        {nullptr}},
        {Op::Unsupported,  "Unsupported",  {nullptr}},

        {Op::Box,          "Box",          {"x1", "y1", "z1", "x2", "y2", "z2", nullptr}},
        {Op::Sphere,       "Sphere",       {"radius", nullptr}},
        {Op::Cylinder,     "Cylinder",     {"capPoint", "basePoint", "radius", nullptr}},
        // Grasp3D's Cone carries nine fields but only two of them reach the geometry: Cone.render
        // reads Radius1 and Z2 and nothing else, every other read being commented out. The
        // remaining seven are dead data, so they are not modelled here (PORT_STATUS.md 3.1).
        {Op::Cone,         "Cone",         {"radius", "height", nullptr}},
        {Op::Torus,        "Torus",        {"majorRadius", "minorRadius", nullptr}},
        {Op::Plane,        "Plane",        {"y", nullptr}},
        // Disc and Triangle also declare a "Thickness" that nothing reads -- not render, not
        // SceneSdf, not PatchSolid, which derives the thickness it needs from the primitive's
        // size instead. Dropped for the same reason.
        {Op::Disc,         "Disc",         {"radius", "holeRadius", nullptr}},
        {Op::Triangle,     "Triangle",     {"x1", "y1", "z1", "x2", "y2", "z2",
                                            "x3", "y3", "z3", nullptr}},

        {Op::Translate,    "Translate",    {"x", "y", "z", nullptr}},
        // "axis" travels in flags.
        {Op::Rotate,       "Rotate",       {"degree", nullptr}},
        {Op::Scale,        "Scale",        {"x", "y", "z", nullptr}},

        {Op::Merge,        "Merge",        {nullptr}},
        {Op::Difference,   "Difference",   {nullptr}},
        {Op::Intersection, "Intersection", {nullptr}},

        {Op::Blob,         "Blob",         {"threshold", nullptr}},
        {Op::BlobSphere,   "BlobSphere",   {"x", "y", "z", "radius", "strength", nullptr}},
        {Op::BlobCylinder, "BlobCylinder", {"x1", "y1", "z1", "x2", "y2", "z2",
                                            "radius", "strength", nullptr}},

        {Op::Sor,          "Sor",          {nullptr}},
        {Op::SorPoint,     "SorPoint",     {"radius", "height", nullptr}},
    };
    count = static_cast<int>(sizeof(table) / sizeof(table[0]));
    return table;
}

inline const OpEntry* findOp(Op op) {
    int n = 0;
    const OpEntry* t = opTable(n);
    for (int i = 0; i < n; ++i) {
        if (t[i].op == op) {
            return &t[i];
        }
    }
    return nullptr;
}

inline const OpEntry* findOp(const char* name) {
    int n = 0;
    const OpEntry* t = opTable(n);
    for (int i = 0; i < n; ++i) {
        if (std::strcmp(t[i].name, name) == 0) {
            return &t[i];
        }
    }
    return nullptr;
}

inline const char* opName(Op op) {
    const OpEntry* e = findOp(op);
    return e ? e->name : "Unsupported";
}

/// Number of params[] slots this op uses. 0 for structural and boolean nodes.
inline int paramCount(Op op) {
    const OpEntry* e = findOp(op);
    if (e == nullptr) {
        return 0;
    }
    int n = 0;
    while (n < 12 && e->keys[n] != nullptr) {
        ++n;
    }
    return n;
}

}  // namespace makina
