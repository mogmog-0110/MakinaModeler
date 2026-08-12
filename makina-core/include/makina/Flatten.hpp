// Authoring tree -> evaluation program (D-01).
//
// The tree the user edits keeps Translate/Rotate/Scale as named nodes and lets a boolean take any
// number of operands. Neither survives contact with a GPU: it cannot recurse, and its stack has to
// be a fixed size. So the program emitted here is
//
//   - post-order (RPN), walked with a small explicit stack
//   - primitives only, with each one's world->local transform baked in
//   - strictly binary booleans, balanced rather than left-leaning
//
// Balancing is the point of the last one. Grasp3D's booleans are n-ary and real models use that:
// pettobotoru.gsf has three Merge nodes with nine children each. Folded left, nine operands need a
// stack nine deep and every subtree bound covers "everything so far", which no culling can use.
// Folded balanced, the same nine need a stack four deep and each half has a bound of its own.
//
// The authoring tree is never rebalanced. The order the user put things in is design intent, and
// rearranging it would undo the one thing this modeller claims over a mesh.

#pragma once

#include "Bounds.hpp"
#include "Scene.hpp"
#include "Sdf.hpp"
// Not just for convenience: the flattener has to apply the same distance correction for a
// non-uniform Scale, and the same "this subtree is empty" sentinel, as the tree evaluator. Sharing
// them rather than restating them is what stops the two from drifting apart.
#include "Eval.hpp"

#include <cstdint>
#include <vector>

namespace makina {

/// Ops the GPU program uses. Primitives are in canonical position; the offset the user authored
/// lives in the baked transform instead, which is what keeps params down to four floats.
enum class EvalOp : std::uint32_t {
    Sphere           = 0,
    BoxCentered      = 1,
    CylinderCentered = 2,
    ConeCentered     = 3,
    Torus            = 4,
    Plane            = 5,

    Union        = 16,
    Difference   = 17,
    Intersection = 18,
};

/// 80 bytes, 16-byte aligned throughout so it maps onto an HLSL StructuredBuffer unchanged.
struct EvalNode {
    std::uint32_t op;
    /// Which material this surface wears, resolved at flatten time, or kNoMaterial.
    ///
    /// Resolved here rather than on the GPU because it is a walk up the authoring tree, and the
    /// flat program has no parents. Taking one of the three padding words costs nothing: the
    /// struct still has to be 80 bytes to map onto the HLSL declaration.
    std::uint32_t materialId;
    std::uint32_t _pad[2];
    /// Primitive dimensions. [3] carries the distance correction, never a dimension.
    float params[4];
    /// world -> local, three rows of four.
    float inv[12];
};
static_assert(sizeof(EvalNode) == 80, "EvalNode must match the HLSL declaration");

struct FlattenReport {
    /// Disc and Triangle. A zero-thickness surface has measure zero, so a ray essentially never
    /// hits it: there is nothing for a ray marcher to draw even before CSG is considered. They are
    /// display-only in the authoring tree and simply absent from the program (PLAN.md D-12).
    int skippedFaces = 0;
    /// Faces found underneath a boolean, which is the case D-12 rejects outright.
    int facesUnderBoolean = 0;
    /// Ops this build does not model.
    int skippedUnsupported = 0;
};

struct EvalProgram {
    std::vector<EvalNode> nodes;   ///< RPN order
    int maxStackDepth = 0;
    FlattenReport report;
};

namespace detail {

/// Inverse of an affine 4x4 whose bottom row is (0,0,0,1). The linear part is inverted by
/// cofactors rather than by transposing: a non-uniform or mirroring Scale makes it non-orthogonal,
/// and a transpose would silently produce the wrong answer for exactly those cases.
inline bool invertAffine(const Mat4& m, Mat4& out) {
    const double a = m.m[0], b = m.m[1], c = m.m[2];
    const double d = m.m[4], e = m.m[5], f = m.m[6];
    const double g = m.m[8], h = m.m[9], i = m.m[10];

    const double A =  (e * i - f * h);
    const double B = -(d * i - f * g);
    const double C =  (d * h - e * g);
    const double det = a * A + b * B + c * C;
    if (det > -1e-18 && det < 1e-18) {
        return false;
    }
    const double id = 1.0 / det;

    const double r00 = A * id;
    const double r01 = -(b * i - c * h) * id;
    const double r02 =  (b * f - c * e) * id;
    const double r10 = B * id;
    const double r11 =  (a * i - c * g) * id;
    const double r12 = -(a * f - c * d) * id;
    const double r20 = C * id;
    const double r21 = -(a * h - b * g) * id;
    const double r22 =  (a * e - b * d) * id;

    const double tx = m.m[3], ty = m.m[7], tz = m.m[11];
    out = Mat4{{r00, r01, r02, -(r00 * tx + r01 * ty + r02 * tz),
                r10, r11, r12, -(r10 * tx + r11 * ty + r12 * tz),
                r20, r21, r22, -(r20 * tx + r21 * ty + r22 * tz),
                0, 0, 0, 1}};
    return true;
}

using Fragment = std::vector<EvalNode>;

/// Named rather than passed as a bare true/false: at a call site five arguments deep, a literal
/// bool says nothing about which way round it goes.
constexpr bool kUnderBoolean = true;
constexpr bool kTopLevel = false;

/// Combines fragments pairwise until one remains, so the resulting tree is balanced.
inline Fragment foldBalanced(std::vector<Fragment> parts, EvalOp op) {
    if (parts.empty()) {
        return Fragment{};
    }
    while (parts.size() > 1) {
        std::vector<Fragment> next;
        next.reserve((parts.size() + 1) / 2);
        for (std::size_t i = 0; i < parts.size(); i += 2) {
            if (i + 1 >= parts.size()) {
                next.push_back(std::move(parts[i]));
                continue;
            }
            Fragment merged = std::move(parts[i]);
            merged.insert(merged.end(), parts[i + 1].begin(), parts[i + 1].end());
            EvalNode n{};
            n.op = static_cast<std::uint32_t>(op);
            n.params[3] = 1.0f;
            // A boolean wears no material of its own -- the shader picks the winning operand's.
            // Left zero it would read as "material 0", which is a real index and would look
            // deliberate to whoever read it next.
            n.materialId = kNoMaterial;
            merged.push_back(n);
            next.push_back(std::move(merged));
        }
        parts = std::move(next);
    }
    return std::move(parts.front());
}

struct FlattenContext {
    const Scene*  scene;
    FlattenReport report;
};

/// Emits one primitive, or nothing when the shape cannot be ray marched.
///
/// centerY exists because two of Grasp3D's primitives are authored off-center along Y -- a
/// Cylinder by its cap and base, a Cone standing on y=0 -- and the canonical forms are centered.
/// The offset is folded into the inverse rather than passed as a parameter.
/// The material a surface wears: its own, or the nearest ancestor's.
///
/// This is POV-Ray's rule, and it has to be, because the same tree is written out as a .pov file
/// where an object with no texture takes the enclosing CSG object's. If the two disagreed, the
/// picture and the export would differ for a reason that has nothing to do with geometry -- and
/// the export is what the renderer is checked against.
inline std::uint8_t resolveMaterial(const Scene& s, const CsgNode& start) {
    const CsgNode* n = &start;
    // Bounded rather than while(true): a malformed parent chain would otherwise hang the flatten
    // with no output at all, which is the hardest kind of failure to place.
    for (std::size_t guard = 0; guard < Scene::kMaxNodes; ++guard) {
        if (n->materialId < s.materials.count) {
            return n->materialId;
        }
        if (n->parent == kNoParent) {
            break;
        }
        n = &s.nodes[n->parent];
    }
    return kNoMaterial;
}

inline bool emitPrimitive(FlattenContext& ctx, const CsgNode& n, const Mat4& world,
                          double scaleCorrection, Fragment& out) {
    EvalNode e{};
    e.params[3] = static_cast<float>(scaleCorrection);
    e.materialId = resolveMaterial(*ctx.scene, n);

    double centerX = 0.0, centerY = 0.0, centerZ = 0.0;
    const float* q = n.params;

    switch (static_cast<Op>(n.op)) {
        case Op::Sphere:
            e.op = static_cast<std::uint32_t>(EvalOp::Sphere);
            e.params[0] = q[0];
            break;
        case Op::Box: {
            e.op = static_cast<std::uint32_t>(EvalOp::BoxCentered);
            centerX = (static_cast<double>(q[0]) + q[3]) * 0.5;
            centerY = (static_cast<double>(q[1]) + q[4]) * 0.5;
            centerZ = (static_cast<double>(q[2]) + q[5]) * 0.5;
            e.params[0] = static_cast<float>(std::fabs(q[3] - q[0]) * 0.5);
            e.params[1] = static_cast<float>(std::fabs(q[4] - q[1]) * 0.5);
            e.params[2] = static_cast<float>(std::fabs(q[5] - q[2]) * 0.5);
            break;
        }
        case Op::Cylinder:
            // params are capPoint, basePoint, radius.
            e.op = static_cast<std::uint32_t>(EvalOp::CylinderCentered);
            centerY = (static_cast<double>(q[0]) + q[1]) * 0.5;
            e.params[0] = q[2];
            e.params[1] = static_cast<float>(std::fabs(q[0] - q[1]) * 0.5);
            break;
        case Op::Cone: {
            // Base sits at y=0 and the apex at y=height, so the canonical form is half a height up.
            // A negative height points the cone the other way; the sign has to survive.
            const double height = q[1];
            if (height == 0.0) {
                return false;
            }
            e.op = static_cast<std::uint32_t>(EvalOp::ConeCentered);
            centerY = height * 0.5;
            e.params[0] = q[0];
            e.params[1] = static_cast<float>(std::fabs(height) * 0.5);
            // A negative height flips the cone; mirror the local Y so the canonical form sees the
            // same shape the reference evaluates.
            if (height < 0.0) {
                e.params[2] = -1.0f;
            } else {
                e.params[2] = 1.0f;
            }
            break;
        }
        case Op::Torus:
            e.op = static_cast<std::uint32_t>(EvalOp::Torus);
            e.params[0] = q[0];
            e.params[1] = q[1];
            break;
        case Op::Plane:
            e.op = static_cast<std::uint32_t>(EvalOp::Plane);
            centerY = q[0];
            break;

        case Op::Disc:
        case Op::Triangle:
            ++ctx.report.skippedFaces;
            return false;

        default:
            ++ctx.report.skippedUnsupported;
            return false;
    }

    Mat4 full = world;
    if (centerX != 0.0 || centerY != 0.0 || centerZ != 0.0) {
        const Mat4 toCenter{{1, 0, 0, centerX, 0, 1, 0, centerY, 0, 0, 1, centerZ, 0, 0, 0, 1}};
        full = mulMat(world, toCenter);
    }

    Mat4 inv{};
    if (!invertAffine(full, inv)) {
        // A Scale of zero on any axis collapses the primitive; there is no shape left to draw.
        ++ctx.report.skippedUnsupported;
        return false;
    }
    for (int r = 0; r < 12; ++r) {
        e.inv[r] = static_cast<float>(inv.m[r]);
    }

    out.push_back(e);
    return true;
}

Fragment flattenNode(FlattenContext& ctx, std::uint16_t index, const Mat4& world,
                     double scaleCorrection, bool underBoolean);

/// Flattens every child and folds them together with op.
inline Fragment flattenChildren(FlattenContext& ctx, std::uint16_t index, const Mat4& world,
                                double scaleCorrection, EvalOp op, bool underBoolean) {
    const CsgNode& n = ctx.scene->nodes[index];
    std::vector<Fragment> parts;
    for (std::uint16_t i = 0; i < n.childCount; ++i) {
        Fragment f = flattenNode(ctx, static_cast<std::uint16_t>(n.firstChild + i), world,
                                 scaleCorrection, underBoolean);
        if (!f.empty()) {
            parts.push_back(std::move(f));
        }
    }
    return foldBalanced(std::move(parts), op);
}

inline Fragment flattenNode(FlattenContext& ctx, std::uint16_t index, const Mat4& world,
                            double scaleCorrection, bool underBoolean) {
    const CsgNode& n = ctx.scene->nodes[index];
    const Op op = static_cast<Op>(n.op);

    // Label falls through to the container path: it emits nothing of its own but its children are
    // geometry (Fidelity.hpp). It is still skipped as a boolean *operand*, below.
    if (isTransform(op)) {
        const Mat4 child = mulMat(world, matrixOf(n));
        return flattenChildren(ctx, index, child, scaleCorrection * scaleFactorOf(n),
                               EvalOp::Union, underBoolean);
    }

    if (op == Op::Difference) {
        Fragment body;
        std::vector<Fragment> blades;
        for (std::uint16_t i = 0; i < n.childCount; ++i) {
            const std::uint16_t child = static_cast<std::uint16_t>(n.firstChild + i);
            if (static_cast<Op>(ctx.scene->nodes[child].op) == Op::Label) {
                continue;
            }
            Fragment f = flattenNode(ctx, child, world, scaleCorrection, kUnderBoolean);
            if (f.empty()) {
                continue;
            }
            if (body.empty()) {
                body = std::move(f);
            } else {
                blades.push_back(std::move(f));
            }
        }
        if (body.empty()) {
            return Fragment{};
        }
        if (blades.empty()) {
            return body;
        }
        Fragment cut = foldBalanced(std::move(blades), EvalOp::Union);
        body.insert(body.end(), cut.begin(), cut.end());
        EvalNode d{};
        d.op = static_cast<std::uint32_t>(EvalOp::Difference);
        d.params[3] = 1.0f;
        d.materialId = kNoMaterial;
        body.push_back(d);
        return body;
    }

    if (op == Op::Intersection) {
        return flattenChildren(ctx, index, world, scaleCorrection, EvalOp::Intersection,
                               kUnderBoolean);
    }

    // Merge, SceneRoot, Unsupported, and every primitive. A primitive with children contributes
    // both its own surface and theirs, which is what the reference evaluator does.
    std::vector<Fragment> parts;
    if (isPrimitive(op)) {
        if (underBoolean && (op == Op::Disc || op == Op::Triangle)) {
            ++ctx.report.facesUnderBoolean;
        }
        Fragment self;
        if (emitPrimitive(ctx, n, world, scaleCorrection, self)) {
            parts.push_back(std::move(self));
        }
    } else if (op == Op::Unsupported) {
        ++ctx.report.skippedUnsupported;
    }

    for (std::uint16_t i = 0; i < n.childCount; ++i) {
        Fragment f = flattenNode(ctx, static_cast<std::uint16_t>(n.firstChild + i), world,
                                 scaleCorrection, underBoolean);
        if (!f.empty()) {
            parts.push_back(std::move(f));
        }
    }
    return foldBalanced(std::move(parts), EvalOp::Union);
}

}  // namespace detail

/// Deepest stack a program will ever need. Balanced folding keeps this logarithmic in the number
/// of operands, so 32 covers a boolean with four billion children.
constexpr int kMaxEvalStack = 32;

/// Walks the emitted program on the CPU, with exactly the semantics the shader implements.
///
/// This exists to be compared against Eval.hpp: the two take different routes to the same number,
/// so if they agree on a scene then the flattening -- baked transforms, balanced binarisation,
/// canonical primitives -- did not change the shape. It is not the drawing path.
inline double evalProgram(const EvalProgram& prog, const double wp[3]) {
    if (prog.nodes.empty()) {
        return kEmpty;
    }

    double stack[kMaxEvalStack];
    int sp = 0;

    for (const EvalNode& n : prog.nodes) {
        if (n.op >= static_cast<std::uint32_t>(EvalOp::Union)) {
            if (sp < 2) {
                return kEmpty;   // malformed program; better empty than a silent wrong answer
            }
            const double b = stack[--sp];
            const double a = stack[--sp];
            switch (static_cast<EvalOp>(n.op)) {
                case EvalOp::Union:        stack[sp++] = a < b ? a : b; break;
                case EvalOp::Difference:   stack[sp++] = a > -b ? a : -b; break;
                default:                   stack[sp++] = a > b ? a : b; break;
            }
            continue;
        }

        if (sp >= kMaxEvalStack) {
            return kEmpty;
        }

        const double x = n.inv[0] * wp[0] + n.inv[1] * wp[1] + n.inv[2] * wp[2] + n.inv[3];
        const double y = n.inv[4] * wp[0] + n.inv[5] * wp[1] + n.inv[6] * wp[2] + n.inv[7];
        const double z = n.inv[8] * wp[0] + n.inv[9] * wp[1] + n.inv[10] * wp[2] + n.inv[11];

        double d;
        switch (static_cast<EvalOp>(n.op)) {
            case EvalOp::Sphere:
                d = mkSdSphere(x, y, z, n.params[0]);
                break;
            case EvalOp::BoxCentered:
                d = mkSdBoxCentered(x, y, z, n.params[0], n.params[1], n.params[2]);
                break;
            case EvalOp::CylinderCentered:
                d = mkSdCylinderCentered(x, y, z, n.params[0], n.params[1]);
                break;
            case EvalOp::ConeCentered:
                // params[2] is the height's sign: a cone authored with a negative height points
                // the other way, and mirroring local Y is how that survives centering.
                d = mkSdConeCentered(x, y * n.params[2], z, n.params[0], n.params[1]);
                break;
            case EvalOp::Torus:
                d = mkSdTorus(x, y, z, n.params[0], n.params[1]);
                break;
            default:
                // Plane: the height was folded into the transform, so the half space is y <= 0.
                d = mkSdPlane(y, 0.0);
                break;
        }
        stack[sp++] = d * n.params[3];
    }

    return sp > 0 ? stack[0] : kEmpty;
}

/// Turns the authoring scene into the program the GPU walks.
inline EvalProgram flatten(const Scene& s) {
    EvalProgram out;
    if (s.nodes.count == 0) {
        return out;
    }

    detail::FlattenContext ctx{&s, {}};
    out.nodes = detail::flattenNode(ctx, 0, detail::identityMat(), 1.0, detail::kTopLevel);
    out.report = ctx.report;

    // Stack depth of the emitted sequence, which is what sizes the shader's array.
    int depth = 0;
    for (const EvalNode& n : out.nodes) {
        if (n.op >= static_cast<std::uint32_t>(EvalOp::Union)) {
            --depth;
        } else {
            ++depth;
        }
        if (depth > out.maxStackDepth) {
            out.maxStackDepth = depth;
        }
    }
    return out;
}

}  // namespace makina
