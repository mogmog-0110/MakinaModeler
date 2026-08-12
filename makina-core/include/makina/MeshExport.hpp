// Writing the solid out as a mesh -- the boundary representation, not the distance field.
//
// PLAN.md D-09 refuses Marching Cubes and Dual Contouring, and that refusal stands. Sampling a
// field on a grid rounds every sharp edge off, makes the whole model resolution-dependent, and
// then needs UVs to be worth anything -- which contradicts the one sentence the project is built
// to say. None of that applies here, because this does not sample anything.
//
// `tessellate()` (TessellateScene.hpp) already produces polygons: the csg.js BSP boolean, the same
// one whose agreement with the distance field is checked at 156,932 samples. So the mesh is not
// extracted, it is **already there**, and this file is a serializer over it.
//
// What that buys, and what it costs:
//
//   sharp edges    exact. An edge is the intersection of two planes, so a box exports as six
//                  quads, not as a few thousand triangles that nearly form one.
//   curved faces   faceted at kSegments = 24 around a circle (Tessellate.hpp). This is the one
//                  place resolution shows, and it is the honest limit of the format, not of the
//                  model -- the model is still a field.
//   UVs            none, deliberately. Adding them would be the self-contradiction D-09 names.
//
// Formats: STL for handing a solid to a printer or a mesh tool, OBJ when the faces need to keep
// their materials. Both are text or trivially packed binary and neither pulls in a dependency.

#pragma once

// Flatten.hpp for resolveMaterial: the mesh has to inherit materials the same way the picture
// does, or a face would be one material in the viewport and another in the exported file.
#include "Flatten.hpp"
#include "RenderMaterial.hpp"
#include "TessellateScene.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace makina {

/// One triangle, ready to write.
struct MeshTriangle {
    double        p[3][3];
    double        n[3];      ///< the polygon's plane normal, not an averaged vertex normal
    std::uint16_t node;      ///< Scene node index the surface came from; carries the material
};

namespace detail {

/// Fans a convex polygon into triangles.
///
/// A fan is only correct for a convex polygon, and every polygon here is one: csg.js splits
/// against planes and a plane cut of a convex face leaves convex faces. Using a general
/// triangulator instead would be slower and would hide that invariant rather than rely on it.
inline void fanTriangles(const BspPoly& poly, std::vector<MeshTriangle>& out) {
    if (poly.v.size() < 3) {
        return;
    }
    for (std::size_t i = 1; i + 1 < poly.v.size(); ++i) {
        MeshTriangle t{};
        for (int c = 0; c < 3; ++c) {
            t.p[0][c] = poly.v[0].p[c];
            t.p[1][c] = poly.v[i].p[c];
            t.p[2][c] = poly.v[i + 1].p[c];
        }
        t.n[0] = poly.nx;
        t.n[1] = poly.ny;
        t.n[2] = poly.nz;
        t.node = poly.shared;
        out.push_back(t);
    }
}

}  // namespace detail

/// Every triangle of every solid in the result.
inline std::vector<MeshTriangle> meshTriangles(const TessellationResult& r) {
    std::vector<MeshTriangle> out;
    for (const BspSolid& solid : r.solids) {
        for (const BspPoly& poly : solid) {
            detail::fanTriangles(poly, out);
        }
    }
    return out;
}

/// Binary STL.
///
/// Binary rather than ASCII because an ASCII STL of a tessellated model is roughly seven times the
/// size and no more readable in practice. The 80-byte header is free text by the format's own
/// rules; it says where the file came from, since an STL carries nothing else -- no units, no
/// materials, no names.
inline std::vector<char> writeStl(const TessellationResult& r, const std::string& name) {
    const std::vector<MeshTriangle> tris = meshTriangles(r);

    std::vector<char> out;
    out.resize(84 + tris.size() * 50);
    std::memset(out.data(), 0, 84);

    const std::string header = "makina " + name;
    std::memcpy(out.data(), header.data(), header.size() < 79 ? header.size() : 79);

    const std::uint32_t count = static_cast<std::uint32_t>(tris.size());
    std::memcpy(out.data() + 80, &count, 4);

    char* at = out.data() + 84;
    for (const MeshTriangle& t : tris) {
        // STL is float32 and little-endian by universal convention; the format has no field to
        // say otherwise, so a double model has to be narrowed here rather than anywhere else.
        const float n[3] = {static_cast<float>(t.n[0]), static_cast<float>(t.n[1]),
                            static_cast<float>(t.n[2])};
        std::memcpy(at, n, 12);
        at += 12;
        for (int v = 0; v < 3; ++v) {
            const float p[3] = {static_cast<float>(t.p[v][0]), static_cast<float>(t.p[v][1]),
                                static_cast<float>(t.p[v][2])};
            std::memcpy(at, p, 12);
            at += 12;
        }
        // The attribute word. Zero: the per-face tint extensions that use it are not
        // interoperable, and a reader that does not know the convention gets a garbled model
        // rather than a plain one.
        at += 2;
    }
    return out;
}

namespace detail {

/// Not `num`: Pov.hpp already has one in this namespace, with a different precision. Two spellings
/// of the same idea in one namespace is an ambiguity the compiler reports somewhere unhelpful.
inline std::string meshNum(double v) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    return buf;
}

}  // namespace detail

/// What an OBJ export produced: the model, and the material library it names.
struct ObjExport {
    std::string obj;
    std::string mtl;
};

/// Wavefront OBJ, with the faces grouped by material.
///
/// Grouped rather than one face at a time: `usemtl` is a mode, so emitting it per face makes a
/// file several times larger that means exactly the same thing. Faces keep the order they were
/// tessellated in within each group, so a diff between two exports stays readable.
///
/// No texture coordinates. See the file header -- their absence is the point, not an omission.
inline ObjExport writeObj(const TessellationResult& r, const Scene& s, const std::string& name,
                          const std::string& mtlFileName) {
    const std::vector<MeshTriangle> tris = meshTriangles(r);

    ObjExport out;
    std::string body;
    body.reserve(tris.size() * 64);

    out.obj = "# makina " + name + "\n# boundary representation, not an extracted isosurface\n";
    if (!r.complete) {
        // Said in the file rather than only on the console: an OBJ that quietly lacks a shape is
        // indistinguishable from one that was authored that way.
        out.obj += "# INCOMPLETE: no solid form for '" + r.missing + "'\n";
    }
    if (!mtlFileName.empty() && s.materials.count > 0) {
        out.obj += "mtllib " + mtlFileName + "\n";
    }

    for (const MeshTriangle& t : tris) {
        for (int v = 0; v < 3; ++v) {
            out.obj += "v " + detail::meshNum(t.p[v][0]) + " " + detail::meshNum(t.p[v][1]) + " " +
                       detail::meshNum(t.p[v][2]) + "\n";
        }
    }
    for (const MeshTriangle& t : tris) {
        out.obj += "vn " + detail::meshNum(t.n[0]) + " " + detail::meshNum(t.n[1]) + " " +
                   detail::meshNum(t.n[2]) + "\n";
    }

    // Faces, gathered per material so usemtl is written once per group.
    std::vector<std::uint8_t> order;
    std::vector<std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < tris.size(); ++i) {
        const std::uint16_t node = tris[i].node;
        const std::uint8_t mat = node < s.nodes.count
                                     ? detail::resolveMaterial(s, s.nodes[node])
                                     : kNoMaterial;
        std::size_t g = 0;
        for (; g < order.size(); ++g) {
            if (order[g] == mat) {
                break;
            }
        }
        if (g == order.size()) {
            order.push_back(mat);
            groups.emplace_back();
        }
        groups[g].push_back(i);
    }

    for (std::size_t g = 0; g < groups.size(); ++g) {
        if (order[g] != kNoMaterial) {
            out.obj += "usemtl mk" + std::to_string(order[g]) + "\n";
        }
        for (const std::size_t i : groups[g]) {
            // OBJ indices are 1-based, and every triangle owns its three vertices: welding would
            // merge the two sides of a sharp edge and hand the reader a smoothed model.
            const std::size_t base = i * 3 + 1;
            const std::size_t vn = i + 1;
            out.obj += "f " + std::to_string(base) + "//" + std::to_string(vn) + " " +
                       std::to_string(base + 1) + "//" + std::to_string(vn) + " " +
                       std::to_string(base + 2) + "//" + std::to_string(vn) + "\n";
        }
    }

    for (std::uint32_t i = 0; i < s.materials.count; ++i) {
        const GpuMaterial m = toGpuMaterial(s.materials[i]);
        out.mtl += "newmtl mk" + std::to_string(i) + "\n";
        out.mtl += "Kd " + detail::meshNum(m.diffuseColor[0]) + " " + detail::meshNum(m.diffuseColor[1]) +
                   " " + detail::meshNum(m.diffuseColor[2]) + "\n";
        out.mtl += "Ka " + detail::meshNum(m.ambient) + " " + detail::meshNum(m.ambient) + " " +
                   detail::meshNum(m.ambient) + "\n";
        out.mtl += "Ks " + detail::meshNum(m.specular) + " " + detail::meshNum(m.specular) + " " +
                   detail::meshNum(m.specular) + "\n";
        // OBJ's Ns is a Phong exponent, the opposite sense to POV's roughness. Converted here
        // rather than stored, the same way Pov.hpp converts in the other direction.
        out.mtl += "Ns " + detail::meshNum(2.0 / (m.roughness * m.roughness) - 2.0) + "\n";
        out.mtl += "d " + detail::meshNum(m.alpha) + "\n\n";
    }

    return out;
}

}  // namespace makina
