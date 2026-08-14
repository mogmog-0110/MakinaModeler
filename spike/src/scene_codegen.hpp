// Generates a scene-specialised HLSL evalCsg from a makina-core evaluation program.
//
// Straight-line SSA: the RPN stack is consumed while generating, so each node becomes a named
// value and the shader has no stack at all. Phase S measured that as the difference between 4.9x
// and nothing -- an interpreted stack becomes an `alloca` in DXIL, which is scratch memory.

#pragma once

#include <makina/Flatten.hpp>

#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace spike {

namespace detail {

/// Full round-trip precision, always with an exponent so a value can never be read as an integer.
inline std::string flt(float v) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.9ef", static_cast<double>(v));
    return buf;
}

/// Writes the two lines a primitive needs: the point in its local frame, and its distance.
///
/// Shared by both generated functions rather than copied, so the distance a material is attached
/// to is the same expression the march walked. `prefix` keeps the two functions' locals apart.
inline void emitPrimitiveLines(std::ostringstream& o, const makina::EvalNode& n, std::size_t i,
                               const char* prefix) {
    const std::string p = std::string(prefix) + "p" + std::to_string(i);
    const std::string var = std::string(prefix) + "t" + std::to_string(i);

    o << "    float3 " << p << " = float3(";
    for (int r = 0; r < 3; ++r) {
        o << "dot(float4(" << flt(n.inv[r * 4 + 0]) << ", " << flt(n.inv[r * 4 + 1]) << ", "
          << flt(n.inv[r * 4 + 2]) << ", " << flt(n.inv[r * 4 + 3]) << "), float4(wp, 1.0))"
          << (r < 2 ? ", " : "");
    }
    o << ");\n";

    o << "    float " << var << " = (";
    switch (static_cast<makina::EvalOp>(n.op)) {
        case makina::EvalOp::Sphere:
            o << "mkSdSphere(" << p << ".x, " << p << ".y, " << p << ".z, " << flt(n.params[0])
              << ")";
            break;
        case makina::EvalOp::BoxCentered:
            o << "mkSdBoxCentered(" << p << ".x, " << p << ".y, " << p << ".z, "
              << flt(n.params[0]) << ", " << flt(n.params[1]) << ", " << flt(n.params[2]) << ")";
            break;
        case makina::EvalOp::CylinderCentered:
            o << "mkSdCylinderCentered(" << p << ".x, " << p << ".y, " << p << ".z, "
              << flt(n.params[0]) << ", " << flt(n.params[1]) << ")";
            break;
        case makina::EvalOp::ConeCentered:
            o << "mkSdConeCentered(" << p << ".x, " << p << ".y * " << flt(n.params[2]) << ", "
              << p << ".z, " << flt(n.params[0]) << ", " << flt(n.params[1]) << ")";
            break;
        case makina::EvalOp::Torus:
            o << "mkSdTorus(" << p << ".x, " << p << ".y, " << p << ".z, " << flt(n.params[0])
              << ", " << flt(n.params[1]) << ")";
            break;
        case makina::EvalOp::Sor:
            // The helper carries this node's inlined polyline; the correction multiply below
            // applies like any primitive's.
            o << "sorDist" << i << "(length(" << p << ".xz), " << p << ".y)";
            break;
        case makina::EvalOp::SphereSweep:
            o << "sweepDist" << i << "(" << p << ")";
            break;
        // Densities, not distances; their params[3] is 1, so the correction multiply below is
        // the identity and the shared tail stays shared.
        case makina::EvalOp::BlobSphere:
            o << "mkBlobSphereDensity(" << p << ".x, " << p << ".y, " << p << ".z, "
              << flt(n.params[0]) << ", " << flt(n.params[1]) << ")";
            break;
        case makina::EvalOp::BlobCylinder:
            o << "mkBlobCylinderDensity(" << p << ".x, " << p << ".y, " << p << ".z, "
              << flt(n.params[0]) << ", " << flt(n.params[1]) << ", " << flt(n.params[2]) << ")";
            break;
        default:
            o << "mkSdPlane(" << p << ".y, 0.0)";
            break;
    }
    o << ") * " << flt(n.params[3]) << ";\n";
}

/// BlobFinish's lines: the field bound, and when the node carries a support box (params[2] > 0)
/// the distance to it, larger wins -- the same two bounds evalBlobFinish takes on the CPU.
inline void emitBlobFinishLines(std::ostringstream& o, const makina::EvalNode& n, std::size_t i,
                                const char* prefix, const std::string& field,
                                const std::string& var) {
    const std::string d = "(" + flt(n.params[0]) + " - " + field + ") / " + flt(n.params[1]) +
                          " * " + flt(n.params[3]);
    if (n.params[2] <= 0.0f) {
        o << "    float " << var << " = " << d << ";\n";
        return;
    }
    const std::string q = std::string(prefix) + "q" + std::to_string(i);
    o << "    float3 " << q << " = float3(";
    for (int r = 0; r < 3; ++r) {
        o << "dot(float4(" << flt(n.inv[r * 4 + 0]) << ", " << flt(n.inv[r * 4 + 1]) << ", "
          << flt(n.inv[r * 4 + 2]) << ", " << flt(n.inv[r * 4 + 3]) << "), float4(wp, 1.0))"
          << (r < 2 ? ", " : "");
    }
    o << ");\n";
    // Strictly outside the box only, same guard as evalBlobFinish: within it the box term is
    // zero or negative and must not pass through the max.
    const std::string b = q + "b";
    o << "    float " << b << " = (max(max(abs(" << q << ".x), abs(" << q << ".y)), abs(" << q
      << ".z)) - 1.0) * " << flt(n.params[2]) << ";\n";
    o << "    float " << var << " = " << b << " > 0.0 ? max(" << d << ", " << b << ") : (" << d
      << ");\n";
}

}  // namespace detail

/// Whether scene_interpret.hlsl can run this program.
///
/// A sor or a sphere_sweep carries its samples in a side table the interpreted pipeline has no
/// buffer for, so a program holding one must use the generated shader -- the caller decides
/// what that costs it (the viewport falls back to the committed picture during a drag). Blob
/// travels whole in its nodes and interprets fine.
inline bool interpretable(const makina::EvalProgram& prog) {
    for (const makina::EvalNode& n : prog.nodes) {
        const makina::EvalOp op = static_cast<makina::EvalOp>(n.op);
        if (op == makina::EvalOp::Sor || op == makina::EvalOp::SphereSweep) {
            return false;
        }
    }
    return true;
}

/// One helper per Sor node: its polyline as inlined constants and the cross-section distance,
/// the same walk SorProfile.hpp does on the CPU. The mirror side is taken by index so only the
/// right side is stored. Shared by both generated functions through the node index in the name.
inline std::string generateSorHelpers(const makina::EvalProgram& prog) {
    std::ostringstream o;
    for (std::size_t i = 0; i < prog.nodes.size(); ++i) {
        const makina::EvalNode& n = prog.nodes[i];
        if (static_cast<makina::EvalOp>(n.op) != makina::EvalOp::Sor) {
            continue;
        }
        const int offset = static_cast<int>(n.params[0]);
        const int num = static_cast<int>(n.params[1]);
        const int last = 2 * num - 1;

        o << "static const float2 kSorSide" << i << "[" << num << "] = {\n";
        for (int k = 0; k < num; ++k) {
            o << "    float2(" << detail::flt(prog.sorProfiles[offset + 2 * k]) << ", "
              << detail::flt(prog.sorProfiles[offset + 2 * k + 1]) << ")"
              << (k + 1 < num ? ",\n" : "\n");
        }
        o << "};\n";
        o << "float2 sorVert" << i << "(int j) {\n"
          << "    return j < " << num << " ? kSorSide" << i << "[j]\n"
          << "        : float2(-kSorSide" << i << "[" << last << " - j].x, kSorSide" << i << "["
          << last << " - j].y);\n}\n";
        o << "float sorDist" << i << "(float px, float py) {\n"
          << "    float d = 1.0e30;\n"
          << "    float sgn = 1.0;\n"
          << "    int j = " << last << ";\n"
          << "    [loop] for (int k = 0; k <= " << last << "; ++k) {\n"
          << "        float2 vi = sorVert" << i << "(k);\n"
          << "        float2 vj = sorVert" << i << "(j);\n"
          << "        float2 e = vj - vi;\n"
          << "        float2 w = float2(px, py) - vi;\n"
          << "        float ee = dot(e, e);\n"
             // A zero-length edge appears when the profile ends at radius zero and meets its
             // mirror; skipping the projection rather than dividing keeps the distance finite.
          << "        float t = ee > 0.0 ? saturate(dot(w, e) / ee) : 0.0;\n"
          << "        float2 b = w - e * t;\n"
          << "        d = min(d, dot(b, b));\n"
          << "        bool c1 = py >= vi.y;\n"
          << "        bool c2 = py < vj.y;\n"
          << "        bool c3 = e.x * w.y > e.y * w.x;\n"
          << "        if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) { sgn = -sgn; }\n"
          << "        j = k;\n"
          << "    }\n"
          << "    return sgn * sqrt(d);\n}\n\n";
    }
    return o.str();
}

/// One helper per SphereSweep node: its samples as inlined constants and the least distance to
/// any round-cone link, the same chain Eval.hpp walks on the CPU.
inline std::string generateSweepHelpers(const makina::EvalProgram& prog) {
    std::ostringstream o;
    for (std::size_t i = 0; i < prog.nodes.size(); ++i) {
        const makina::EvalNode& n = prog.nodes[i];
        if (static_cast<makina::EvalOp>(n.op) != makina::EvalOp::SphereSweep) {
            continue;
        }
        const int offset = static_cast<int>(n.params[0]);
        const int num = static_cast<int>(n.params[1]);

        o << "static const float4 kSweep" << i << "[" << num << "] = {\n";
        for (int k = 0; k < num; ++k) {
            const float* q = prog.sweepProfiles.data() + offset + 4 * k;
            o << "    float4(" << detail::flt(q[0]) << ", " << detail::flt(q[1]) << ", "
              << detail::flt(q[2]) << ", " << detail::flt(q[3]) << ")"
              << (k + 1 < num ? ",\n" : "\n");
        }
        o << "};\n";
        o << "float sweepDist" << i << "(float3 p) {\n"
          << "    float d = 1.0e30;\n"
          << "    [loop] for (int k = 0; k + 1 < " << num << "; ++k) {\n"
          << "        float4 a = kSweep" << i << "[k];\n"
          << "        float4 b = kSweep" << i << "[k + 1];\n"
          << "        d = min(d, mkSdRoundCone(p.x, p.y, p.z, a.x, a.y, a.z, b.x, b.y, b.z, "
             "a.w, b.w));\n"
          << "    }\n"
          << "    return d;\n}\n\n";
    }
    return o.str();
}

inline std::string generateEvalCsg(const makina::EvalProgram& prog) {
    std::ostringstream o;
    o << "// Generated for this scene. Do not edit.\n"
      << "float evalCsg(float3 wp) {\n";

    if (prog.nodes.empty()) {
        // An empty program is a scene with nothing renderable in it. Saying so in the shader beats
        // emitting something that silently draws a void.
        o << "    return 1.0e30;   // nothing renderable in this scene\n}\n";
        return o.str();
    }

    std::vector<std::string> stack;
    stack.reserve(prog.nodes.size());

    for (std::size_t i = 0; i < prog.nodes.size(); ++i) {
        const makina::EvalNode& n = prog.nodes[i];
        const std::string var = "t" + std::to_string(i);

        if (n.op == static_cast<std::uint32_t>(makina::EvalOp::BlobFinish)) {
            if (stack.empty()) {
                throw std::runtime_error("evaluation program is malformed: a blob finish has no "
                                         "field to close");
            }
            const std::string field = stack.back();  stack.pop_back();
            detail::emitBlobFinishLines(o, n, i, "", field, var);
            stack.push_back(var);
            continue;
        }
        if (n.op >= static_cast<std::uint32_t>(makina::EvalOp::Union)) {
            if (stack.size() < 2) {
                throw std::runtime_error("evaluation program is malformed: a boolean has fewer "
                                         "than two operands");
            }
            const std::string b = stack.back();  stack.pop_back();
            const std::string a = stack.back();  stack.pop_back();

            o << "    float " << var << " = ";
            switch (static_cast<makina::EvalOp>(n.op)) {
                case makina::EvalOp::Union:      o << "min(" << a << ", " << b << ");\n"; break;
                case makina::EvalOp::Difference: o << "max(" << a << ", -" << b << ");\n"; break;
                case makina::EvalOp::BlobSum:    o << a << " + " << b << ";\n"; break;
                default:                         o << "max(" << a << ", " << b << ");\n"; break;
            }
            stack.push_back(var);
            continue;
        }

        detail::emitPrimitiveLines(o, n, i, "");
        stack.push_back(var);
    }

    if (stack.size() != 1) {
        throw std::runtime_error("evaluation program does not reduce to a single value");
    }

    o << "    return " << stack.front() << ";\n}\n";
    return o.str();
}

/// The same program again, carrying which material won.
///
/// A second function rather than widening the first, and the reason is measured: the march calls
/// evalCsg once per step and the normal calls it four more times, none of which want a material.
/// Threading an id through all of that would pay for it thousands of times per pixel to use it
/// once. This one is called at the hit point and nowhere else, so the loop stays exactly as fast
/// as it was (docs/SPIKE_PERF.md).
///
/// Which operand's material survives a boolean:
///
///   Union         the nearer surface. That is the one you are looking at.
///   Intersection  the limiting surface, for the same reason.
///   Difference    **always the left operand's**, even where the visible surface is the cut.
///                 This is POV-Ray's cutaway_textures answer and Grasp3D's, and it has to match
///                 or the export and the picture would paint the inside of a hole differently.
inline std::string generateEvalCsgMaterial(const makina::EvalProgram& prog) {
    std::ostringstream o;
    o << "// Generated for this scene. Do not edit.\n"
      << "// x = distance, y = material (255 = none), z = pigment (-1 = none)\n"
      << "float3 evalCsgMaterial(float3 wp) {\n";

    if (prog.nodes.empty()) {
        o << "    return float3(1.0e30, 255.0, -1.0);   // nothing renderable in this scene\n}\n";
        return o.str();
    }

    std::vector<std::string> stack;
    stack.reserve(prog.nodes.size());

    for (std::size_t i = 0; i < prog.nodes.size(); ++i) {
        const makina::EvalNode& n = prog.nodes[i];
        const std::string var = "m" + std::to_string(i);

        if (n.op == static_cast<std::uint32_t>(makina::EvalOp::BlobFinish)) {
            if (stack.empty()) {
                throw std::runtime_error("evaluation program is malformed: a blob finish has no "
                                         "field to close");
            }
            const std::string field = stack.back();  stack.pop_back();
            // The finish node is the blob's one surface, so it carries the material; the
            // density leaves below it never win a comparison.
            const std::string dist = var + "d";
            detail::emitBlobFinishLines(o, n, i, "m", field + ".x", dist);
            o << "    float3 " << var << " = float3(" << dist << ", "
              << detail::flt(static_cast<float>(n.materialId)) << ", "
              << detail::flt(n.pigmentId == makina::kNoPigment
                                 ? -1.0f
                                 : static_cast<float>(n.pigmentId)) << ");\n";
            stack.push_back(var);
            continue;
        }
        if (n.op >= static_cast<std::uint32_t>(makina::EvalOp::Union)) {
            if (stack.size() < 2) {
                throw std::runtime_error("evaluation program is malformed: a boolean has fewer "
                                         "than two operands");
            }
            const std::string b = stack.back();  stack.pop_back();
            const std::string a = stack.back();  stack.pop_back();

            o << "    float3 " << var << " = ";
            switch (static_cast<makina::EvalOp>(n.op)) {
                case makina::EvalOp::Union:
                    o << a << ".x < " << b << ".x ? " << a << " : " << b << ";\n";
                    break;
                case makina::EvalOp::Difference:
                    o << "float3(max(" << a << ".x, -" << b << ".x), " << a << ".yz);\n";
                    break;
                case makina::EvalOp::BlobSum:
                    o << "float3(" << a << ".x + " << b << ".x, " << a << ".yz);\n";
                    break;
                default:
                    o << a << ".x > " << b << ".x ? " << a << " : " << b << ";\n";
                    break;
            }
            stack.push_back(var);
            continue;
        }

        // Same emitter as evalCsg, so the distance a material is attached to is the one the march
        // actually walked. Copying the expressions here instead would let the two drift.
        detail::emitPrimitiveLines(o, n, i, "m");
        // The pigment index rides beside the material so a surface can find both the pattern
        // it wears and the space that pattern is nailed to. The material alone cannot say the
        // second: two walls sharing one checker but standing apart need different entries.
        o << "    float3 " << var << " = float3(mt" << i << ", "
          << detail::flt(static_cast<float>(n.materialId)) << ", "
          << detail::flt(n.pigmentId == makina::kNoPigment
                             ? -1.0f
                             : static_cast<float>(n.pigmentId)) << ");\n";
        stack.push_back(var);
    }

    if (stack.size() != 1) {
        throw std::runtime_error("evaluation program does not reduce to a single value");
    }

    o << "    return " << stack.front() << ";\n}\n";
    return o.str();
}

/// shadingInclude selects the look: "scene_shading.hlsl" is the plain clay pass used to check the
/// geometry, "scene_weathered.hlsl" the procedural wear the demo is about. Both consume the same
/// generated evalCsg, so a difference between the two images can only be shading.
inline std::string generateShader(const makina::EvalProgram& prog,
                                  const std::string& shadingInclude, bool interpret = false) {
    std::ostringstream o;
    o << "#include \"scene_prelude.hlsl\"\n\n";
    if (interpret) {
        // The program travels in a buffer instead of the source, so this shader is the same for
        // every scene and nothing needs compiling when the model changes.
        o << "#include \"scene_interpret.hlsl\"\n\n";
    } else {
        o << generateSorHelpers(prog) << generateSweepHelpers(prog) << generateEvalCsg(prog)
          << "\n" << generateEvalCsgMaterial(prog) << "\n";
    }
    o << "#include \"" << shadingInclude << "\"\n";
    return o.str();
}

}  // namespace spike
