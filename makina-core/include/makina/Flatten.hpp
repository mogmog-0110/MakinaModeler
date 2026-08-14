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
#include <cstring>
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
    // Density leaves, not distances: each pushes its component's density at the point. Only a
    // BlobSum/BlobFinish chain may consume them -- the flattener is what guarantees that.
    BlobSphere       = 6,
    BlobCylinder     = 7,

    Union        = 16,
    Difference   = 17,
    Intersection = 18,
    /// Binary like the booleans: pops two densities, pushes their sum.
    BlobSum      = 19,
    /// The one unary op: pops the summed field, pushes (threshold - field) / L * correction.
    /// Every walker dispatches on it before the "op >= Union means binary" test.
    BlobFinish   = 20,
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
    /// Which entry of EvalProgram::pigments paints this surface, or kNoPigment.
    ///
    /// Not the same as the material's own `textureId`. A pattern is fixed in the space of whatever
    /// object wears it, so two solids sharing one material but standing in different places need
    /// two entries -- the index is per node for that reason, and the table is built by the flatten
    /// rather than copied from the scene.
    std::uint32_t pigmentId;
    std::uint32_t _pad;
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

/// A pattern, and the space it is nailed to.
///
/// POV transforms a texture along with the object wearing it, so a checker on a wall that is moved
/// moves its squares too. This renderer reads the pattern at the hit point, which is in world
/// space, so the transform has to arrive with the pattern: `inv` takes a world point into the space
/// the pattern was authored in. Without it a translated solid slides through a pattern pinned to
/// the world -- a whole square out of step on a 0.45 checker, which reads as the wrong texture
/// rather than as a wrong transform.
struct GpuPigment {
    Pigment pattern;
    /// world -> the space of the object that wears this pattern, three rows of four.
    float inv[12];
};
static_assert(sizeof(GpuPigment) == 112, "GpuPigment must match the HLSL declaration");

struct EvalProgram {
    std::vector<EvalNode> nodes;   ///< RPN order
    /// One entry per (pattern, object space) pair the scene actually uses.
    std::vector<GpuPigment> pigments;
    int maxStackDepth = 0;
    FlattenReport report;
};

/// What EvalNode::pigmentId carries when a surface wears no pattern.
///
/// Any index past the end of the table would do, and the shader treats it that way, but a stated
/// constant is what keeps the CPU and the shader from disagreeing about which one that is.
constexpr std::uint32_t kNoPigment = 0xFFFFFFFFu;

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
            // A boolean wears neither material nor pattern of its own -- the shader picks the
            // winning operand's. Left zero they would read as "material 0" and "pigment 0", which
            // are real indices and would look deliberate to whoever read them next.
            n.materialId = kNoMaterial;
            n.pigmentId = kNoPigment;
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
    std::vector<GpuPigment> pigments;
};

/// Interns one (pattern, object space) pair, so two solids wearing the same texture in the same
/// place share an entry and two in different places do not.
inline std::uint32_t internPigment(FlattenContext& ctx, const Pigment& pattern, const Mat4& space) {
    GpuPigment g{};
    g.pattern = pattern;
    Mat4 inv{};
    if (!invertAffine(space, inv)) {
        // A collapsed space has no pattern coordinates to read. The surface keeps its flat color
        // rather than being painted with whatever the identity would have given.
        return kNoPigment;
    }
    for (int r = 0; r < 12; ++r) {
        g.inv[r] = static_cast<float>(inv.m[r]);
    }
    for (std::size_t i = 0; i < ctx.pigments.size(); ++i) {
        if (std::memcmp(&ctx.pigments[i], &g, sizeof(GpuPigment)) == 0) {
            return static_cast<std::uint32_t>(i);
        }
    }
    ctx.pigments.push_back(g);
    return static_cast<std::uint32_t>(ctx.pigments.size() - 1);
}

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
                          const Mat4& textureWorld, double scaleCorrection, Fragment& out) {
    EvalNode e{};
    e.params[3] = static_cast<float>(scaleCorrection);
    e.materialId = resolveMaterial(*ctx.scene, n);
    e.pigmentId = kNoPigment;
    if (e.materialId < ctx.scene->materials.count) {
        const std::int32_t t = ctx.scene->materials[e.materialId].textureId;
        if (t >= 0 && static_cast<std::uint32_t>(t) < ctx.scene->pigments.count) {
            e.pigmentId = internPigment(ctx, ctx.scene->pigments[t], textureWorld);
        }
    }

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

/// One blob component as a density leaf, plus its share of the Lipschitz bound.
///
/// `corr` is the running distance correction at this node and `blobCorr` the one at the Blob
/// itself, so corr/blobCorr is the smallest axis factor of the transforms between the two --
/// the same quantity Eval.hpp's field walk tracks as minScale. The leaf's own params[3] stays 1:
/// a density is a scalar composed with the inverse transform and needs no distance correction.
inline void collectBlobLeaves(FlattenContext& ctx, std::uint16_t index, const Mat4& world,
                              double corr, double blobCorr, Fragment& leaves, double& lipschitz) {
    const CsgNode& n = ctx.scene->nodes[index];
    const Op op = static_cast<Op>(n.op);

    if (isTransform(op)) {
        const Mat4 child = mulMat(world, matrixOf(n));
        const double c = corr * scaleFactorOf(n);
        for (std::uint16_t i = 0; i < n.childCount; ++i) {
            collectBlobLeaves(ctx, static_cast<std::uint16_t>(n.firstChild + i), child, c,
                              blobCorr, leaves, lipschitz);
        }
        return;
    }

    if (isBlobComponent(op)) {
        EvalNode e{};
        e.materialId = kNoMaterial;
        e.pigmentId = kNoPigment;
        e.params[3] = 1.0f;
        const float* q = n.params;

        Mat4 full = world;
        double radius;
        double strength;
        if (static_cast<Op>(n.op) == Op::BlobSphere) {
            e.op = static_cast<std::uint32_t>(EvalOp::BlobSphere);
            radius = q[3];
            strength = q[4];
            e.params[0] = q[3];
            e.params[1] = q[4];
            const Mat4 toCenter{{1, 0, 0, q[0], 0, 1, 0, q[1], 0, 0, 1, q[2], 0, 0, 0, 1}};
            full = mulMat(world, toCenter);
        } else {
            radius = q[6];
            strength = q[7];
            const double ux = static_cast<double>(q[3]) - q[0];
            const double uy = static_cast<double>(q[4]) - q[1];
            const double uz = static_cast<double>(q[5]) - q[2];
            const double len = std::sqrt(ux * ux + uy * uy + uz * uz);
            const double mx = (static_cast<double>(q[0]) + q[3]) * 0.5;
            const double my = (static_cast<double>(q[1]) + q[4]) * 0.5;
            const double mz = (static_cast<double>(q[2]) + q[5]) * 0.5;
            if (len < 1e-12) {
                // Both end points coincide: the capsule is a sphere, and emitting it as one
                // avoids a rotation towards a direction that does not exist.
                e.op = static_cast<std::uint32_t>(EvalOp::BlobSphere);
                e.params[0] = q[6];
                e.params[1] = q[7];
                const Mat4 toMid{{1, 0, 0, mx, 0, 1, 0, my, 0, 0, 1, mz, 0, 0, 0, 1}};
                full = mulMat(world, toMid);
            } else {
                e.op = static_cast<std::uint32_t>(EvalOp::BlobCylinder);
                e.params[0] = q[6];
                e.params[1] = static_cast<float>(len * 0.5);
                e.params[2] = q[7];
                // A frame whose local Y is the segment's direction. Any perpendicular pair
                // serves for X and Z: the density is radially symmetric about the axis.
                const double ax = ux / len, ay = uy / len, az = uz / len;
                const double hx = std::fabs(ay) < 0.9 ? 0.0 : 1.0;
                const double hy = std::fabs(ay) < 0.9 ? 1.0 : 0.0;
                double vx = hy * az - 0.0 * ay, vy = 0.0 * ax - hx * az,
                       vz = hx * ay - hy * ax;
                const double vl = std::sqrt(vx * vx + vy * vy + vz * vz);
                vx /= vl; vy /= vl; vz /= vl;
                const double wx = ay * vz - az * vy;
                const double wy = az * vx - ax * vz;
                const double wz = ax * vy - ay * vx;
                const Mat4 place{{vx, ax, wx, mx,
                                  vy, ay, wy, my,
                                  vz, az, wz, mz,
                                  0, 0, 0, 1}};
                full = mulMat(world, place);
            }
        }

        Mat4 inv{};
        if (!invertAffine(full, inv)) {
            ++ctx.report.skippedUnsupported;
            return;
        }
        for (int r = 0; r < 12; ++r) {
            e.inv[r] = static_cast<float>(inv.m[r]);
        }
        lipschitz += blobLipschitz(radius, strength) / nz(corr / blobCorr);
        leaves.push_back(e);
        return;
    }

    for (std::uint16_t i = 0; i < n.childCount; ++i) {
        collectBlobLeaves(ctx, static_cast<std::uint16_t>(n.firstChild + i), world, corr,
                          blobCorr, leaves, lipschitz);
    }
}

/// One Blob as a program fragment: density leaves folded left with BlobSum -- a running sum
/// keeps the stack two deep, so balancing would buy nothing -- then one BlobFinish carrying
/// the threshold, the Lipschitz bound, and the blob's own distance correction.
///
/// The program's blob differs from Eval.hpp's in one measured way: it has no
/// distance-to-support sharpening, so far from the blob it steps by threshold/L instead of by
/// the true clearance. Same sign everywhere, both conservative; the march is slower, not wrong.
inline Fragment emitBlob(FlattenContext& ctx, std::uint16_t index, const Mat4& world,
                         const Mat4& textureWorld, double scaleCorrection) {
    const CsgNode& n = ctx.scene->nodes[index];
    Fragment leaves;
    double lipschitz = 0.0;
    for (std::uint16_t i = 0; i < n.childCount; ++i) {
        collectBlobLeaves(ctx, static_cast<std::uint16_t>(n.firstChild + i), world,
                          scaleCorrection, scaleCorrection, leaves, lipschitz);
    }
    if (leaves.empty() || lipschitz <= 0.0) {
        // No components is no geometry, same as Eval.hpp's kEmpty.
        return Fragment{};
    }

    Fragment out;
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        out.push_back(leaves[i]);
        if (i > 0) {
            EvalNode s{};
            s.op = static_cast<std::uint32_t>(EvalOp::BlobSum);
            s.params[3] = 1.0f;
            s.materialId = kNoMaterial;
            s.pigmentId = kNoPigment;
            out.push_back(s);
        }
    }

    EvalNode f{};
    f.op = static_cast<std::uint32_t>(EvalOp::BlobFinish);
    f.params[0] = n.params[0];
    f.params[1] = static_cast<float>(lipschitz);
    f.params[3] = static_cast<float>(scaleCorrection);
    f.materialId = resolveMaterial(*ctx.scene, n);
    f.pigmentId = kNoPigment;
    if (f.materialId < ctx.scene->materials.count) {
        const std::int32_t t = ctx.scene->materials[f.materialId].textureId;
        if (t >= 0 && static_cast<std::uint32_t>(t) < ctx.scene->pigments.count) {
            f.pigmentId = internPigment(ctx, ctx.scene->pigments[t], textureWorld);
        }
    }
    out.push_back(f);
    return out;
}

Fragment flattenNode(FlattenContext& ctx, std::uint16_t index, const Mat4& world,
                     const Mat4& textureWorld, double scaleCorrection, bool underBoolean);

/// Flattens every child and folds them together with op.
inline Fragment flattenChildren(FlattenContext& ctx, std::uint16_t index, const Mat4& world,
                                const Mat4& textureWorld, double scaleCorrection, EvalOp op,
                                bool underBoolean) {
    const CsgNode& n = ctx.scene->nodes[index];
    std::vector<Fragment> parts;
    for (std::uint16_t i = 0; i < n.childCount; ++i) {
        Fragment f = flattenNode(ctx, static_cast<std::uint16_t>(n.firstChild + i), world,
                                 textureWorld, scaleCorrection, underBoolean);
        if (!f.empty()) {
            parts.push_back(std::move(f));
        }
    }
    return foldBalanced(std::move(parts), op);
}

inline Fragment flattenNode(FlattenContext& ctx, std::uint16_t index, const Mat4& world,
                            const Mat4& textureWorld, double scaleCorrection,
                            bool underBoolean) {
    const CsgNode& n = ctx.scene->nodes[index];
    const Op op = static_cast<Op>(n.op);
    // A node that names a material also fixes the space its pattern lives in, for itself and for
    // everything under it with no material of its own.
    //
    // Which space that is comes from the file, not from taste. Pov.hpp pushes every enclosing
    // transform down onto the primitives, so a `pigment` written on a boolean is followed by no
    // transform at all and POV reads it in world space, while one written on a primitive is
    // followed by that primitive's translate and POV carries the pattern along. The renderer has
    // to agree with the .pov it is compared against, so it splits the same way: a pattern named
    // on a solid moves with the solid, one named on a boolean stays put.
    const bool carriesTexture = n.materialId < ctx.scene->materials.count;
    const Mat4 textureHere = isPrimitive(op) ? world : identityMat();

    // Label falls through to the container path: it emits nothing of its own but its children are
    // geometry (Fidelity.hpp). It is still skipped as a boolean *operand*, below.
    if (isTransform(op)) {
        const Mat4 child = mulMat(world, matrixOf(n));
        return flattenChildren(ctx, index, child, carriesTexture ? identityMat() : textureWorld,
                               scaleCorrection * scaleFactorOf(n), EvalOp::Union, underBoolean);
    }

    if (op == Op::Difference) {
        Fragment body;
        std::vector<Fragment> blades;
        for (std::uint16_t i = 0; i < n.childCount; ++i) {
            const std::uint16_t child = static_cast<std::uint16_t>(n.firstChild + i);
            if (static_cast<Op>(ctx.scene->nodes[child].op) == Op::Label) {
                continue;
            }
            Fragment f = flattenNode(ctx, child, world,
                                     carriesTexture ? textureHere : textureWorld,
                                     scaleCorrection, kUnderBoolean);
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
        d.pigmentId = kNoPigment;
        body.push_back(d);
        return body;
    }

    if (op == Op::Intersection) {
        return flattenChildren(ctx, index, world, carriesTexture ? textureHere : textureWorld,
                               scaleCorrection, EvalOp::Intersection, kUnderBoolean);
    }

    if (op == Op::Blob) {
        // Like a primitive, a blob's pattern is nailed to its own space and moves with it.
        return emitBlob(ctx, index, world, carriesTexture ? world : textureWorld,
                        scaleCorrection);
    }
    if (isBlobComponent(op)) {
        // Outside a Blob a component has no field to join; Eval.hpp returns kEmpty for the same
        // reason.
        return Fragment{};
    }

    // Merge, SceneRoot, Unsupported, and every primitive. A primitive with children contributes
    // both its own surface and theirs, which is what the reference evaluator does.
    std::vector<Fragment> parts;
    if (isPrimitive(op)) {
        if (underBoolean && (op == Op::Disc || op == Op::Triangle)) {
            ++ctx.report.facesUnderBoolean;
        }
        Fragment self;
        if (emitPrimitive(ctx, n, world, carriesTexture ? textureHere : textureWorld,
                          scaleCorrection,
                          self)) {
            parts.push_back(std::move(self));
        }
    } else if (op == Op::Unsupported) {
        ++ctx.report.skippedUnsupported;
    }

    for (std::uint16_t i = 0; i < n.childCount; ++i) {
        Fragment f = flattenNode(ctx, static_cast<std::uint16_t>(n.firstChild + i), world,
                                 carriesTexture ? textureHere : textureWorld, scaleCorrection,
                                 underBoolean);
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

/// One primitive of the program, at a world point.
///
/// Lifted out so the distance walk and the surface walk below cannot drift: two copies of a
/// primitive table is exactly the kind of difference that shows up as one shape being subtly the
/// wrong size in one of the two answers, with nothing to point at.
inline double evalLeaf(const EvalNode& n, const double wp[3]) {
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
            // params[2] is the height's sign: a cone authored with a negative height points the
            // other way, and mirroring local Y is how that survives centering.
            d = mkSdConeCentered(x, y * n.params[2], z, n.params[0], n.params[1]);
            break;
        case EvalOp::Torus:
            d = mkSdTorus(x, y, z, n.params[0], n.params[1]);
            break;
        // Densities, not distances: no correction applies, so these return directly. The value
        // is only meaningful to the BlobSum/BlobFinish chain the flattener emits after them.
        case EvalOp::BlobSphere:
            return mkBlobSphereDensity(x, y, z, n.params[0], n.params[1]);
        case EvalOp::BlobCylinder:
            return mkBlobCylinderDensity(x, y, z, n.params[0], n.params[1], n.params[2]);
        default:
            // Plane: the height was folded into the transform, so the half space is y <= 0.
            d = mkSdPlane(y, 0.0);
            break;
    }
    return d * n.params[3];
}

/// BlobFinish's arithmetic, shared by both walkers so the field/distance bridge cannot drift.
inline double evalBlobFinish(const EvalNode& n, double field) {
    return (static_cast<double>(n.params[0]) - field) / n.params[1] * n.params[3];
}

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
        if (n.op == static_cast<std::uint32_t>(EvalOp::BlobFinish)) {
            if (sp < 1) {
                return kEmpty;
            }
            stack[sp - 1] = evalBlobFinish(n, stack[sp - 1]);
            continue;
        }
        if (n.op >= static_cast<std::uint32_t>(EvalOp::Union)) {
            if (sp < 2) {
                return kEmpty;   // malformed program; better empty than a silent wrong answer
            }
            const double b = stack[--sp];
            const double a = stack[--sp];
            switch (static_cast<EvalOp>(n.op)) {
                case EvalOp::Union:        stack[sp++] = a < b ? a : b; break;
                case EvalOp::Difference:   stack[sp++] = a > -b ? a : -b; break;
                case EvalOp::BlobSum:      stack[sp++] = a + b; break;
                default:                   stack[sp++] = a > b ? a : b; break;
            }
            continue;
        }

        if (sp >= kMaxEvalStack) {
            return kEmpty;
        }

        stack[sp++] = evalLeaf(n, wp);
    }

    return sp > 0 ? stack[0] : kEmpty;
}

/// What the program says about one point: how far, and what the nearest surface wears.
struct ProgramSurface {
    double        distance = kEmpty;
    std::uint32_t materialId = kNoMaterial;
    std::uint32_t pigmentId = kNoPigment;
};

/// The same walk as evalProgram, carrying the material and pattern the shader carries.
///
/// Separate from evalProgram for the reason the generated shader has two functions: the march
/// calls that one thousands of times per pixel and this one once per hit.
///
/// The selection rules are the shader's, Difference keeping the left operand's material included.
/// That one matters more than it looks -- the exporter hands POV a difference whose blade wears
/// the body's texture, so anything that compared the blade's own material would be comparing a
/// value neither renderer ever reads.
inline ProgramSurface evalProgramSurface(const EvalProgram& prog, const double wp[3]) {
    if (prog.nodes.empty()) {
        return ProgramSurface{};
    }

    ProgramSurface stack[kMaxEvalStack];
    int sp = 0;

    for (const EvalNode& n : prog.nodes) {
        if (n.op == static_cast<std::uint32_t>(EvalOp::BlobFinish)) {
            if (sp < 1) {
                return ProgramSurface{};
            }
            // The whole blob is one surface, and the finish node is what wears its material --
            // the density leaves below it never surface on their own.
            ProgramSurface r;
            r.distance = evalBlobFinish(n, stack[sp - 1].distance);
            r.materialId = n.materialId;
            r.pigmentId = n.pigmentId;
            stack[sp - 1] = r;
            continue;
        }
        if (n.op >= static_cast<std::uint32_t>(EvalOp::Union)) {
            if (sp < 2) {
                return ProgramSurface{};
            }
            const ProgramSurface b = stack[--sp];
            const ProgramSurface a = stack[--sp];
            if (static_cast<EvalOp>(n.op) == EvalOp::Union) {
                stack[sp++] = a.distance < b.distance ? a : b;
            } else if (static_cast<EvalOp>(n.op) == EvalOp::Difference) {
                ProgramSurface r = a;
                r.distance = a.distance > -b.distance ? a.distance : -b.distance;
                stack[sp++] = r;
            } else if (static_cast<EvalOp>(n.op) == EvalOp::BlobSum) {
                ProgramSurface r;
                r.distance = a.distance + b.distance;
                stack[sp++] = r;
            } else {
                stack[sp++] = a.distance > b.distance ? a : b;
            }
            continue;
        }

        if (sp >= kMaxEvalStack) {
            return ProgramSurface{};
        }
        ProgramSurface leaf;
        leaf.distance = evalLeaf(n, wp);
        leaf.materialId = n.materialId;
        leaf.pigmentId = n.pigmentId;
        stack[sp++] = leaf;
    }
    return sp > 0 ? stack[0] : ProgramSurface{};
}

/// Turns the authoring scene into the program the GPU walks.
inline EvalProgram flatten(const Scene& s) {
    EvalProgram out;
    if (s.nodes.count == 0) {
        return out;
    }

    detail::FlattenContext ctx{&s, {}, {}};
    out.nodes = detail::flattenNode(ctx, 0, detail::identityMat(), detail::identityMat(), 1.0,
                                    detail::kTopLevel);
    out.report = ctx.report;
    out.pigments = std::move(ctx.pigments);

    // Stack depth of the emitted sequence, which is what sizes the shader's array.
    int depth = 0;
    for (const EvalNode& n : out.nodes) {
        if (n.op == static_cast<std::uint32_t>(EvalOp::BlobFinish)) {
            // Unary: pops one, pushes one.
        } else if (n.op >= static_cast<std::uint32_t>(EvalOp::Union)) {
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
