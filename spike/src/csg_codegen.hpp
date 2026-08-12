// Phase S spike: generate a per-scene HLSL evalCsg instead of interpreting the tree.
//
// Why this exists: the interpreter's stack is a dynamically indexed local array, which DXIL turns
// into `alloca` (scratch memory). Generating straight-line SSA removes the stack entirely, folds
// every transform to a literal, and drops the per-node branch dispatch. Phase S measured that the
// algorithmic fix (bounding-volume culling) does not work here, so a constant-factor win is what
// is needed -- and this is where one could come from.
//
// The generated function keeps the interpreter's signature so shading.hlsl is shared verbatim and
// the comparison measures tree evaluation and nothing else.

#pragma once

#include "csg_flatten.hpp"

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace spike {

namespace detail {

// Full round-trip precision, always with an exponent so the literal can never be mistaken for an
// integer, and an f suffix so HLSL does not widen it.
inline std::string flt(float v) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.9ef", static_cast<double>(v));
    return buf;
}

}  // namespace detail

/// What a generated shader bakes in.
enum class CodegenMode {
    /// Tree shape *and* every dimension and transform become literals. Fastest, but any parameter
    /// change needs a recompile.
    Literals,
    /// Tree shape only; dimensions and transforms are still read from the payload buffer. Loses
    /// constant folding but keeps the stack elimination, the branch elimination and the
    /// straight-line form -- and a parameter drag no longer changes the shader, so nothing has to
    /// be recompiled while the user is dragging. Measuring this against Literals is what says
    /// whether the hybrid scheme is needed at all.
    StructureOnly,
};

// Emits `float evalCsg(float3 wp, float cullRadius)` specialised to one scene.
inline std::string generateEvalCsg(const FlatProgram& program, CodegenMode mode) {
    std::ostringstream o;
    o << "// Generated for this scene. Do not edit.\n"
      << "float evalCsg(float3 wp, float cullRadius) {\n"
      << "    float4 wp1 = float4(wp, 1.0);\n";

    std::vector<std::string> stack;
    stack.reserve(program.headers.size());

    for (std::size_t i = 0; i < program.headers.size(); ++i) {
        const std::uint32_t op = program.headers[i].opFlags & 0xFFu;
        const std::string var = "t" + std::to_string(i);

        if (op >= static_cast<std::uint32_t>(Op::Union)) {
            const std::string b = stack.back();  stack.pop_back();
            const std::string a = stack.back();  stack.pop_back();

            o << "    float " << var << " = ";
            if (op == static_cast<std::uint32_t>(Op::Union)) {
                o << "min(" << a << ", " << b << ");\n";
            } else if (op == static_cast<std::uint32_t>(Op::Difference)) {
                o << "max(" << a << ", -" << b << ");\n";
            } else {
                o << "max(" << a << ", " << b << ");\n";
            }
            stack.push_back(var);
            continue;
        }

        const NodePayload& pl = program.payloads[i];
        const std::string p = "p" + std::to_string(i);
        const std::string idx = std::to_string(i);

        if (mode == CodegenMode::StructureOnly) {
            const std::string pay = "pl" + idx;
            o << "    NodePayload " << pay << " = gPayloads[" << idx << "];\n"
              << "    float3 " << p << " = float3(dot(" << pay << ".invRow0, wp1), dot("
              << pay << ".invRow1, wp1), dot(" << pay << ".invRow2, wp1));\n"
              << "    float " << var << " = (";
            switch (static_cast<Op>(op)) {
                case Op::Sphere:   o << "sdSphere("   << p << ", " << pay << ".params.x)"; break;
                case Op::Box:      o << "sdBox("      << p << ", " << pay << ".params.xyz)"; break;
                case Op::Cylinder: o << "sdCylinder(" << p << ", " << pay << ".params.x, "
                                     << pay << ".params.y)"; break;
                default:           o << "sdTorus("    << p << ", " << pay << ".params.x, "
                                     << pay << ".params.y)"; break;
            }
            o << ") * " << pay << ".params.w;\n";
            stack.push_back(var);
            continue;
        }

        o << "    float3 " << p << " = float3(";
        const float* rows[3] = {pl.invRow0, pl.invRow1, pl.invRow2};
        for (int r = 0; r < 3; ++r) {
            o << "dot(float4(" << detail::flt(rows[r][0]) << ", " << detail::flt(rows[r][1])
              << ", " << detail::flt(rows[r][2]) << ", " << detail::flt(rows[r][3]) << "), wp1)"
              << (r < 2 ? ", " : "");
        }
        o << ");\n";

        o << "    float " << var << " = (";
        switch (static_cast<Op>(op)) {
            case Op::Sphere:
                o << "sdSphere(" << p << ", " << detail::flt(pl.params[0]) << ")";
                break;
            case Op::Box:
                o << "sdBox(" << p << ", float3(" << detail::flt(pl.params[0]) << ", "
                  << detail::flt(pl.params[1]) << ", " << detail::flt(pl.params[2]) << "))";
                break;
            case Op::Cylinder:
                o << "sdCylinder(" << p << ", " << detail::flt(pl.params[0]) << ", "
                  << detail::flt(pl.params[1]) << ")";
                break;
            default:
                o << "sdTorus(" << p << ", " << detail::flt(pl.params[0]) << ", "
                  << detail::flt(pl.params[1]) << ")";
                break;
        }
        o << ") * " << detail::flt(pl.params[3]) << ";\n";
        stack.push_back(var);
    }

    if (stack.size() != 1) {
        throw std::invalid_argument("generateEvalCsg: the program does not reduce to a single root");
    }

    o << "    return " << stack.front() << ";\n"
      << "}\n";
    return o.str();
}

// The whole translation unit: shared prelude, the generated evaluator, shared shading.
inline std::string generateShader(const FlatProgram& program, CodegenMode mode) {
    std::ostringstream o;
    o << "#include \"prelude.hlsl\"\n\n"
      << generateEvalCsg(program, mode) << "\n"
      << "#include \"shading.hlsl\"\n";
    return o.str();
}

}  // namespace spike
