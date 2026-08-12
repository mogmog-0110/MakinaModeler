// The scene as a POV-Ray file: a port of Grasp3D's export path (GRASP_MAIN.traverse and the
// povray() methods scattered across the element classes).
//
// This is the third independent implementation of the same geometry, and that is its whole point.
// makina-core evaluates an SDF, the GPU marches a generated shader, and POV-Ray ray-traces exact
// surfaces from a text file. Three routes that share no code agreeing on an image is evidence the
// model is right; two of them agreeing only shows they were written by the same hand.
//
// Three rules carry most of the work, and each exists because POV gets something wrong otherwise:
//
//   union wrapping    a POV boolean takes one object per operand. A branch that writes several
//                     objects has to be gathered, or the second one silently becomes the next
//                     operand and the difference cuts the wrong thing.
//   blade materials   POV's cutaway_textures decides per surface and misses a blade that is itself
//                     a CSG (the cut comes out black) and flickers on a torus groove (black
//                     speckle). Writing the body's material onto the blade removes the decision.
//   solid faces       a Disc or a Triangle has no inside, so as a CSG operand it removes nothing.
//                     PovShape.hpp thickens it (PatchSolid).
//
// Transforms are not baked into coordinates. They accumulate in a text block on the way down and
// each leaf writes the block inside its own braces, which is where POV expects them.

#pragma once

#include "Op.hpp"
#include "PovShape.hpp"
#include "Scene.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace makina {

/// Where the camera is and what it can see. Mirrors Grasp3D's PovCamera.
///
/// `fovY` is vertical, as OpenGL and the Makina renderer state it; POV's `angle` is horizontal, so
/// it is converted here. Passing the vertical angle straight through would render a narrower image
/// than the one being compared against -- the classic reason a POV cross-check "nearly" matches.
struct PovCamera {
    double eye[3]{0.0, 0.0, 5.0};
    double lookAt[3]{0.0, 0.0, 0.0};
    double up[3]{0.0, 1.0, 0.0};
    double fovY = 30.0;      ///< degrees
    double aspect = 4.0 / 3.0;  ///< width / height
};

struct PovOptions {
    PovCamera camera;
    /// Written verbatim before the geometry: #include lines, global_settings, light_source blocks.
    /// The caller owns lighting because the scene format does not carry it in a form POV can use.
    std::string preamble;
    /// Emitted as a comment at the top. Empty means none.
    std::string title;
    /// Replace every material with flat white on black, for a silhouette comparison.
    ///
    /// The point of comparing silhouettes rather than pictures is that a silhouette depends only
    /// on geometry, transforms, camera and handedness -- the things two independent renderers can
    /// actually be expected to agree on to the pixel. Shading, tone mapping and light units are
    /// not, and comparing them would only measure how differently they were tuned.
    bool silhouette = false;
};

namespace detail {

/// POV's own coordinate convention is left-handed. `right<-aspect,0,0>` flips it, which is what
/// makes a right-handed scene come out unmirrored (COORDINATES.md).
inline std::string writePovCamera(const PovCamera& c) {
    const double tanHalfV = std::tan(c.fovY * 3.14159265358979323846 / 360.0);
    const double angle = 2.0 * std::atan(tanHalfV * c.aspect) * 180.0 / 3.14159265358979323846;

    std::string s = "camera{\n";
    s += "\tright<" + num(-c.aspect) + ",0,0>\n";
    s += "\tangle " + num(angle) + "\n";
    s += "\tlocation" + vec3(c.eye[0], c.eye[1], c.eye[2]) + "\n";
    s += "\tlook_at" + vec3(c.lookAt[0], c.lookAt[1], c.lookAt[2]) + "\n";
    s += "\tsky" + vec3(c.up[0], c.up[1], c.up[2]) + "\n";
    s += "}\n\n";
    return s;
}

/// Flat, self-lit white: no shading terms at all, so the object is either covered or not.
inline std::string silhouetteMaterial() {
    return "\tpigment{color rgb<1,1,1>}\n\tfinish{ambient 1 diffuse 0 specular 0}\n";
}

inline std::string povMaterial(const Scene& s, const CsgNode& n, bool silhouette) {
    if (silhouette) {
        return silhouetteMaterial();
    }
    if (n.materialId >= s.materials.count) {
        return std::string();
    }
    const Material& m = s.materials[n.materialId];
    // POV's rgbf carries filter, not opacity, so the stored alpha is inverted; roughness is the
    // reciprocal sense of shininess. Both conversions are Grasp3D's and are what its .pov files
    // have always meant.
    std::string t = "\tpigment{color rgbf<" + num(m.diffuse[0]) + "," + num(m.diffuse[1]) + "," +
                    num(m.diffuse[2]) + "," + num(1.0 - m.alpha) + ">}\n";
    if (m.emission != 0.0f) {
        t += "\tinterior{media{emission rgb " + num(m.emission) + "}}\n";
    }
    t += "\tfinish{ambient " + num(m.ambient) + " specular " + num(m.specular) + " roughness " +
         num(1.0 - m.shininess / 128.0) + "}\n";
    return t;
}

/// Transform of a single node, in the form POV takes. Empty for anything that is not a transform.
inline std::string povTransform(const CsgNode& n) {
    const Op op = static_cast<Op>(n.op);
    const float* q = n.params;
    if (op == Op::Translate) {
        return "\ttranslate" + vec3(q[0], q[1], q[2]) + "\n";
    }
    if (op == Op::Scale) {
        return "\tscale" + vec3(q[0], q[1], q[2]) + "\n";
    }
    if (op == Op::Rotate) {
        const std::uint16_t axis = n.flags & flags::kAxisMask;
        const double d = q[0];
        if (axis == flags::kAxisX) return "\trotate" + vec3(d, 0.0, 0.0) + "\n";
        if (axis == flags::kAxisY) return "\trotate" + vec3(0.0, d, 0.0) + "\n";
        return "\trotate" + vec3(0.0, 0.0, d) + "\n";
    }
    return std::string();
}

/// Objects a subtree writes. A boolean is one block however many primitives it holds, which is
/// exactly the question `union` wrapping needs answered.
inline int countPovObjects(const Scene& s, std::uint16_t index) {
    const CsgNode& n = s.nodes[index];
    const Op op = static_cast<Op>(n.op);
    if (isBoolean(op)) {
        return 1;
    }
    int count = isPrimitive(op) ? 1 : 0;
    for (std::uint16_t i = 0; i < n.childCount; ++i) {
        count += countPovObjects(s, static_cast<std::uint16_t>(n.firstChild + i));
    }
    return count;
}

/// Material of the first primitive in a subtree, which is what a blade wears so the cut surface
/// comes out the color of the side being cut. Empty when the subtree has no primitive.
inline std::string bodyMaterial(const Scene& s, std::uint16_t index, bool silhouette) {
    const CsgNode& n = s.nodes[index];
    if (isPrimitive(static_cast<Op>(n.op))) {
        return povMaterial(s, n, silhouette);
    }
    for (std::uint16_t i = 0; i < n.childCount; ++i) {
        const std::string m =
            bodyMaterial(s, static_cast<std::uint16_t>(n.firstChild + i), silhouette);
        if (!m.empty()) {
            return m;
        }
    }
    return std::string();
}

/// Replaces every material block in `block` with `material`, once per object.
///
/// Grasp3D streams this through PovCutterStream, a line filter, because it writes bytes as it
/// goes. Here the block is already a string, so the same rule -- drop material lines, put the
/// replacement where the pigment was -- is a rewrite over lines. Keyed on pigment because
/// povMaterial always writes exactly one, first, per object.
inline std::string swapMaterial(const std::string& block, const std::string& material) {
    static const char* kMaterialKeys[] = {"pigment", "texture", "finish", "interior",
                                          "pigment_map"};
    std::string out;
    out.reserve(block.size());

    std::size_t at = 0;
    while (at < block.size()) {
        std::size_t end = block.find('\n', at);
        if (end == std::string::npos) {
            end = block.size();
        } else {
            ++end;
        }
        const std::string line = block.substr(at, end - at);
        at = end;

        std::size_t first = line.find_first_not_of(" \t");
        const std::string trimmed = first == std::string::npos ? std::string() : line.substr(first);

        bool isMaterial = false;
        bool isPigment = false;
        for (const char* key : kMaterialKeys) {
            const std::string k(key);
            if (trimmed.rfind(k + "{", 0) == 0 || trimmed.rfind(k + " {", 0) == 0) {
                isMaterial = true;
                isPigment = (k == "pigment");
                break;
            }
        }
        if (!isMaterial) {
            out += line;
        } else if (isPigment && !material.empty()) {
            out += material;
        }
    }
    return out;
}

/// One subtree.
///
/// `transform` is the block accumulated from the ancestors, `inCsg` says whether a face has to be
/// written as a solid. Returns the text; the caller decides where it goes, which is what lets a
/// blade's material be swapped after the fact.
inline std::string povSubtree(const Scene& s, std::uint16_t index, const std::string& transform,
                              bool inCsg, bool silhouette) {
    const CsgNode& n = s.nodes[index];
    const Op op = static_cast<Op>(n.op);

    // A transform prepends to the block, so the outermost transform is applied last -- POV reads
    // them top to bottom and the innermost has to act first.
    const std::string childTransform =
        isTransform(op) ? povTransform(n) + transform : transform;

    std::string out;

    if (isPrimitive(op)) {
        const std::string shape = povShape(n, inCsg);
        if (!shape.empty()) {
            out += shape;
            out += povMaterial(s, n, silhouette);
            out += transform;
            out += "}\n\n";
        }
    } else if (isBoolean(op)) {
        out += (op == Op::Merge ? "merge{\n" : op == Op::Difference ? "difference{\n"
                                                                    : "intersection{\n");
    }

    const bool childInCsg = inCsg || op == Op::Difference || op == Op::Intersection;
    const bool group = (op == Op::Difference || op == Op::Intersection);
    const std::string blade =
        op == Op::Difference && n.childCount > 0 ? bodyMaterial(s, n.firstChild, silhouette)
                                                 : std::string();

    for (std::uint16_t i = 0; i < n.childCount; ++i) {
        const std::uint16_t c = static_cast<std::uint16_t>(n.firstChild + i);
        std::string part = povSubtree(s, c, childTransform, childInCsg, silhouette);
        if (part.empty()) {
            continue;
        }
        if (group && countPovObjects(s, c) >= 2) {
            part = "union{\n" + part + "}\n";
        }
        if (op == Op::Difference && i > 0) {
            part = swapMaterial(part, blade);
        }
        out += part;
    }

    if (isBoolean(op)) {
        // Only where no material was written onto the blades: POV then has to decide the cut
        // surface itself, and cutaway_textures is the only way to ask it to.
        if (op == Op::Intersection || (op == Op::Difference && blade.empty())) {
            out += "cutaway_textures\n";
        }
        // No transform on a boolean. Every leaf inside already carries the whole block, this
        // node's ancestors included, so repeating it here would apply those transforms twice.
        out += "}\n\n";
    }

    return out;
}

}  // namespace detail

/// The whole scene as POV-Ray source.
///
/// Lighting is not in the scene format in a form POV can use, so it comes from `opt.preamble`; a
/// file written with an empty preamble parses and renders black, which is a truthful result rather
/// than a silent default that would make a cross-check meaningless.
inline std::string writePov(const Scene& s, const PovOptions& opt) {
    std::string out;
    if (!opt.title.empty()) {
        out += "// " + opt.title + "\n";
    }
    out += "// written by makina-core\n\n";
    if (opt.silhouette) {
        // Nothing behind the object and nothing lighting it: the image is a mask, so a stray
        // background gradient or a light would put grey where the comparison needs a hard edge.
        out += "background{color rgb<0,0,0>}\n\n";
    }
    out += opt.preamble;
    if (!opt.preamble.empty() && opt.preamble.back() != '\n') {
        out += "\n";
    }
    out += detail::writePovCamera(opt.camera);

    if (s.nodes.count == 0) {
        return out;
    }
    const CsgNode& root = s.nodes[0];
    for (std::uint16_t i = 0; i < root.childCount; ++i) {
        out += detail::povSubtree(s, static_cast<std::uint16_t>(root.firstChild + i),
                                  std::string(), false, opt.silhouette);
    }
    return out;
}

}  // namespace makina
