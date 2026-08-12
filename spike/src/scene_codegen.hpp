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
                default:                         o << "max(" << a << ", " << b << ");\n"; break;
            }
            stack.push_back(var);
            continue;
        }

        const std::string p = "p" + std::to_string(i);
        o << "    float3 " << p << " = float3(";
        for (int r = 0; r < 3; ++r) {
            o << "dot(float4(" << detail::flt(n.inv[r * 4 + 0]) << ", "
              << detail::flt(n.inv[r * 4 + 1]) << ", " << detail::flt(n.inv[r * 4 + 2]) << ", "
              << detail::flt(n.inv[r * 4 + 3]) << "), float4(wp, 1.0))" << (r < 2 ? ", " : "");
        }
        o << ");\n";

        o << "    float " << var << " = (";
        switch (static_cast<makina::EvalOp>(n.op)) {
            case makina::EvalOp::Sphere:
                o << "mkSdSphere(" << p << ".x, " << p << ".y, " << p << ".z, "
                  << detail::flt(n.params[0]) << ")";
                break;
            case makina::EvalOp::BoxCentered:
                o << "mkSdBoxCentered(" << p << ".x, " << p << ".y, " << p << ".z, "
                  << detail::flt(n.params[0]) << ", " << detail::flt(n.params[1]) << ", "
                  << detail::flt(n.params[2]) << ")";
                break;
            case makina::EvalOp::CylinderCentered:
                o << "mkSdCylinderCentered(" << p << ".x, " << p << ".y, " << p << ".z, "
                  << detail::flt(n.params[0]) << ", " << detail::flt(n.params[1]) << ")";
                break;
            case makina::EvalOp::ConeCentered:
                o << "mkSdConeCentered(" << p << ".x, " << p << ".y * "
                  << detail::flt(n.params[2]) << ", " << p << ".z, "
                  << detail::flt(n.params[0]) << ", " << detail::flt(n.params[1]) << ")";
                break;
            case makina::EvalOp::Torus:
                o << "mkSdTorus(" << p << ".x, " << p << ".y, " << p << ".z, "
                  << detail::flt(n.params[0]) << ", " << detail::flt(n.params[1]) << ")";
                break;
            default:
                o << "mkSdPlane(" << p << ".y, 0.0)";
                break;
        }
        o << ") * " << detail::flt(n.params[3]) << ";\n";
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
        o << generateEvalCsg(prog) << "\n";
    }
    o << "#include \"" << shadingInclude << "\"\n";
    return o.str();
}

}  // namespace spike
