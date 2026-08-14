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

/// BlobFinish's expression over an already-generated field value: the one unary op.
inline std::string blobFinishExpr(const makina::EvalNode& n, const std::string& field) {
    return "(" + flt(n.params[0]) + " - " + field + ") / " + flt(n.params[1]) + " * " +
           flt(n.params[3]);
}

}  // namespace detail

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
            o << "    float " << var << " = " << detail::blobFinishExpr(n, field) << ";\n";
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
            o << "    float3 " << var << " = float3("
              << detail::blobFinishExpr(n, field + ".x") << ", "
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
        o << generateEvalCsg(prog) << "\n" << generateEvalCsgMaterial(prog) << "\n";
    }
    o << "#include \"" << shadingInclude << "\"\n";
    return o.str();
}

}  // namespace spike
