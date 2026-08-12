// Which material a surface wears, and where the answer has to come from.
//
// Two rules, and both of them are answers POV-Ray already gives, because the same tree is exported
// to a .pov file and ray traced as the check on the renderer. If the picture and the export
// disagreed about the inside of a hole, the comparison would blame the geometry.
//
//   inheritance   a node with no material of its own takes the nearest ancestor's. POV does this
//                 by nesting textures; here it is resolved once, at flatten time.
//   the cut       a Difference paints the cut surface with the *body's* material, not the blade's.
//                 That is POV's cutaway_textures answer and Grasp3D's.
//
// The second rule lives in the shader, so what is checked here is the input it needs: every
// primitive in the flattened program carries the material the authoring tree implies.

#include <makina/Flatten.hpp>
#include <makina/RenderMaterial.hpp>
#include <makina/SceneJson.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

int checks = 0;
int failures = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        std::printf("    FAIL  %s\n", what.c_str());
        ++failures;
    }
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("could not open '" + path + "'");
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// The material the authoring tree implies for this node, worked out the slow, obvious way.
///
/// Deliberately not the same code as resolveMaterial: a test that calls the function it is testing
/// only proves the function is deterministic.
std::uint8_t expectedMaterial(const makina::Scene& s, std::uint16_t index) {
    for (std::uint16_t at = index;;) {
        if (s.nodes[at].materialId < s.materials.count) {
            return s.nodes[at].materialId;
        }
        if (s.nodes[at].parent == makina::kNoParent) {
            return makina::kNoMaterial;
        }
        at = s.nodes[at].parent;
    }
}

void exercise(const std::string& path) {
    std::printf("%s\n", path.c_str());
    const makina::Scene scene = makina::parseScene(readFile(path));
    const makina::EvalProgram prog = makina::flatten(scene);

    // Every primitive carries a material that exists, or none at all. An index past the end would
    // read whatever the buffer happened to hold, which is the sort of wrong that looks authored.
    int carried = 0;
    int primitives = 0;
    for (const makina::EvalNode& n : prog.nodes) {
        if (n.op < static_cast<std::uint32_t>(makina::EvalOp::Union)) {
            ++primitives;
        }
        if (n.op >= static_cast<std::uint32_t>(makina::EvalOp::Union)) {
            check(n.materialId == makina::kNoMaterial,
                  "a boolean node carries a material index; the shader picks the winning "
                  "operand's, so a value here would be read as deliberate");
            continue;
        }
        const bool valid = n.materialId == makina::kNoMaterial ||
                           n.materialId < scene.materials.count;
        check(valid, "primitive carries material " + std::to_string(n.materialId) + " but the "
                     "scene has only " + std::to_string(scene.materials.count));
        if (n.materialId != makina::kNoMaterial) {
            ++carried;
        }
    }

    // Inheritance, against an independent walk of the authoring tree.
    for (std::uint32_t i = 0; i < scene.nodes.count; ++i) {
        const makina::CsgNode& n = scene.nodes[i];
        if (!makina::isPrimitive(static_cast<makina::Op>(n.op))) {
            continue;
        }
        const std::uint8_t want = expectedMaterial(scene, static_cast<std::uint16_t>(i));
        // detail:: on purpose. The rule belongs to flattening, not to the public API -- the POV
        // exporter does not call it, because POV inherits textures by nesting instead.
        const std::uint8_t got = makina::detail::resolveMaterial(scene, n);
        check(want == got, "node " + std::to_string(n.id) + " resolves to material " +
                               std::to_string(got) + ", the tree says " + std::to_string(want));
    }

    // A scene that puts a material on a surface has to end up with it on a surface. Without this
    // the two loops above are both satisfied by a program where every primitive carries
    // kNoMaterial -- which is what a broken resolveMaterial would produce.
    //
    // Scoped to scenes that flatten to something. verify_faces is Disc and Triangle only, and a
    // zero-thickness face is not an operand (PLAN.md D-12), so it correctly flattens to nothing
    // and correctly carries no material.
    bool referenced = false;
    for (std::uint32_t i = 0; i < scene.nodes.count && !referenced; ++i) {
        referenced = scene.nodes[i].materialId < scene.materials.count;
    }
    if (referenced && primitives > 0) {
        check(carried > 0, "surfaces survived flattening and the tree names materials for them, "
                           "but not one of them carries a material");
    }

    // The conversion the shader actually reads.
    for (std::uint32_t i = 0; i < scene.materials.count; ++i) {
        const makina::GpuMaterial g = makina::toGpuMaterial(scene.materials[i]);
        check(g.roughness > 0.0f, "material " + std::to_string(i) +
                                      " converts to a roughness of zero, which POV divides by");
        check(g.diffuseColor[0] >= 0.0f && g.diffuseColor[0] <= 1.0f,
              "material " + std::to_string(i) + " has a red outside 0..1; the JSON reader is "
              "supposed to have divided by 255");
    }

    std::printf("    %u materials, %d surfaces carry one\n", scene.materials.count, carried);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: material_check <scene.json> ...\n");
        return 2;
    }

    std::printf("makina-core material resolution\n\n");

    for (int i = 1; i < argc; ++i) {
        try {
            exercise(argv[i]);
        } catch (const std::exception& e) {
            std::printf("    FAIL  %s\n", e.what());
            ++failures;
        }
    }

    if (failures == 0) {
        std::printf("\nmaterials reach the surfaces the tree says they should (%d checks)\n",
                    checks);
        return 0;
    }
    std::printf("\n%d of %d checks FAILED\n", failures, checks);
    return 1;
}
