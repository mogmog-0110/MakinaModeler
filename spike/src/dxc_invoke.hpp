// Running DXC on a generated shader.
//
// Pulled out of render_scene so the bake tool uses the same call. Two things about it are easy to
// get subtly different between two copies, and both are silent when wrong: the include roots, and
// where the compiler's own output goes when it fails.
//
// The second include root is the point of the whole design -- it is makina-core, so the shader
// compiles `makina/Sdf.hpp`, the same file the CPU evaluator compiles. One spelling of "what is a
// torus" (PLAN.md D-03).

#pragma once

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace spike {

struct DxcPaths {
    std::string compiler;      ///< dxc.exe
    std::string shaderDir;     ///< the shader-side glue
    std::string coreInclude;   ///< makina-core, so makina/Sdf.hpp resolves
};

/// Compiles one entry point. Throws with the compiler's own message on failure.
///
/// The message matters more than the exit code: a generated shader that will not compile is
/// almost always a codegen bug, and the line DXC names is the only pointer to it.
inline double compileHlsl(const DxcPaths& paths, const std::string& hlslPath,
                          const std::string& profile, const std::string& entry,
                          const std::string& outPath) {
    const std::string log = outPath + ".log";

    // The extra outer quotes are cmd's rule: with a quoted program *and* quoted arguments it
    // strips the outermost pair, so without them the path with spaces comes apart.
    const std::string cmd = "\"\"" + paths.compiler + "\" -T " + profile + " -E " + entry +
                            " -O3 -I \"" + paths.shaderDir + "\" -I \"" + paths.coreInclude +
                            "\" -Fo \"" + outPath + "\" \"" + hlslPath + "\" > \"" + log +
                            "\" 2>&1\"";

    const auto begin = std::chrono::steady_clock::now();
    const int rc = std::system(cmd.c_str());
    const auto end = std::chrono::steady_clock::now();

    if (rc != 0) {
        std::string detail;
        std::ifstream in(log, std::ios::binary);
        if (in) {
            detail.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }
        if (detail.empty()) {
            detail = "(no compiler output captured)";
        }
        throw std::runtime_error("dxc failed on '" + hlslPath + "':\n" + detail);
    }

    return std::chrono::duration<double, std::milli>(end - begin).count();
}

}  // namespace spike
