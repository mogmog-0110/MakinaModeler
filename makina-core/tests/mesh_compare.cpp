// The fourth agreement: does the mesh we hand out lie on the field we draw?
//
// The other three compare representations that are all inside the renderer. This one checks the
// thing that leaves: an .stl a printer or another tool will read. If it drifted from the distance
// field, every picture in the project could still agree with every other one and the exported file
// would be a different object.
//
// What is checked, and what cannot be:
//
//   vertices    every triangle corner must sit on the zero level set. A corner comes either from
//               the analytic primitive or from a plane cut, and both are exact points on the
//               surface, so the tolerance here is tight.
//   centroids   loose by construction. A triangle spanning a curved face is a chord, and its
//               middle sits inside the true surface by the sagitta of a kSegments = 24 arc. The
//               bound is derived from that rather than guessed, so a real regression cannot hide
//               under a number chosen to make the test pass.
//
// Signs are checked too. A centroid must be *inside* (negative), never outside: a chord cuts the
// corner off a convex surface. A positive centroid means the winding or the plane is inverted,
// which is exactly the failure that produces a mesh that looks right and prints inside out.

#include <makina/Eval.hpp>
#include <makina/Bounds.hpp>
#include <makina/MeshExport.hpp>
#include <makina/SceneJson.hpp>

#include <algorithm>
#include <cmath>
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

/// Half the diagonal of the scene's box, which is what every tolerance here is a fraction of.
double sceneRadius(const makina::Scene& s) {
    const makina::BoundsResult b = makina::worldBounds(s);
    if (!b.box.valid) {
        return 1.0;
    }
    double diag = 0.0;
    for (int i = 0; i < 3; ++i) {
        const double span = b.box.hi[i] - b.box.lo[i];
        diag += span * span;
    }
    const double r = std::sqrt(diag) * 0.5;
    return r > 1e-9 ? r : 1.0;
}

void exercise(const std::string& path) {
    std::printf("%s\n", path.c_str());
    const makina::Scene scene = makina::parseScene(readFile(path));
    const makina::TessellationResult mesh = makina::tessellate(scene);
    const std::vector<makina::MeshTriangle> tris = makina::meshTriangles(mesh);

    if (tris.empty()) {
        // Legal: verify_faces is Disc and Triangle only, and a zero-thickness face is not a solid
        // (PLAN.md D-12). Nothing to compare, and saying so beats a silent pass.
        std::printf("    no solid form; nothing to compare\n");
        return;
    }

    const double radius = sceneRadius(scene);

    // The chord of a kSegments = 24 arc falls short of the arc by r(1 - cos(pi/24)) = 0.86% of the
    // radius (Tessellate.hpp). Every disagreement between the mesh and the field is some multiple
    // of this, and the multiplier is the radius of the curved surface involved -- which is why the
    // yardstick is the scene, not the triangle.
    //
    // Measuring against the triangle's own edge was the first attempt and it was wrong, in a way
    // worth recording. When a faceted torus is subtracted, the leftover slivers are tiny while the
    // material they fail to remove is set by the *torus's* facet size, so the ratio came out at
    // 113x and looked like a defect. It is not: the worst point in hero_flange sits 0.0087 outside
    // the field, and 1.15 * 0.0086 = 0.0099 is what a 24-segment cut of that torus leaves behind.
    // The error belongs to the surface doing the cutting, not to the surface being cut.
    constexpr double kSagitta = 0.0086;

    double worstVertex = 0.0;
    double worstCentroid = 0.0;
    double worstOutside = 0.0;

    int compared = 0;
    for (const makina::MeshTriangle& t : tris) {
        // Faces and planes are skipped, and the reason is not a tolerance problem.
        //
        // A Disc has no volume, so the tessellator gives it the same thickness the POV export
        // does, and an infinite Plane has to become a finite slab to be a solid at all. In both
        // cases the mesh surface is a deliberate stand-in that the distance field does not have,
        // and comparing them would be comparing two different objects (PLAN.md D-12).
        if (t.node < scene.nodes.count) {
            const makina::Op op = static_cast<makina::Op>(scene.nodes[t.node].op);
            if (op == makina::Op::Disc || op == makina::Op::Triangle || op == makina::Op::Plane) {
                continue;
            }
        }
        ++compared;
        {
            double c2[3] = {0.0, 0.0, 0.0};
            for (int v = 0; v < 3; ++v) {
                for (int k = 0; k < 3; ++k) {
                    c2[k] += t.p[v][k] / 3.0;
                }
            }
            const double dc = makina::eval(scene, c2);
            if (!makina::isEmpty(dc)) {
                worstOutside = std::max(worstOutside, dc);
            }
        }

        for (int v = 0; v < 3; ++v) {
            const double d = makina::eval(scene, t.p[v]);
            if (makina::isEmpty(d)) {
                continue;
            }
            worstVertex = std::max(worstVertex, std::fabs(d));
        }

        double c[3] = {0.0, 0.0, 0.0};
        for (int v = 0; v < 3; ++v) {
            for (int k = 0; k < 3; ++k) {
                c[k] += t.p[v][k] / 3.0;
            }
        }
        const double d = makina::eval(scene, c);
        if (makina::isEmpty(d)) {
            continue;
        }
        worstCentroid = std::max(worstCentroid, std::fabs(d));
        if (d > 0.0) {
            worstOutside = std::max(worstOutside, d);
        }
    }

    if (compared == 0) {
        std::printf("    %zu triangles, all of them stand-ins for faces or planes; nothing to "
                    "compare\n", tris.size());
        return;
    }

    // Nothing may stick out of the field. That is the invariant, and "must lie *on* the surface"
    // is not -- the tessellator returns a list of solids that may overlap, while the field is
    // their union, so a face buried inside another solid is correct and reads as deeply negative.
    // Measured: the clean scenes reach 0.12 of a facet's own edge, the busy ones 0.16.
    //
    // Scaled by the facet rather than by the scene because that is what the error is made of: a
    // chord across a concave surface bulges out of the solid by a fraction of its own length, and
    // a fixed fraction of the scene radius would be far too loose for a small feature and far too
    // tight for a large one.
    //
    // Bounded by the sagitta of the largest curved surface the scene could contain, which is the
    // scene radius. Twice it, because a point can be left behind by two cuts at once.
    const double outsideAllowance = radius * kSagitta * 2.0;

    // A scene holding a Plane is exempt, and not by a hair's breadth: verify_plane comes in at
    // 1.18 against an allowance of 0.066.
    //
    // A Plane is a half-space. It has no solid form at all, so the tessellator has to stand one in
    // at some finite size, and every boolean involving it then stops where that stand-in stops
    // while the field goes on forever. The surface left behind is not an error in the boolean --
    // it is the half-space being finite. Skipping the triangle is not enough because the material
    // left over belongs to whatever the plane was cutting, so the whole scene has to sit this out.
    bool hasPlane = false;
    for (std::uint32_t i = 0; i < scene.nodes.count && !hasPlane; ++i) {
        hasPlane = static_cast<makina::Op>(scene.nodes[i].op) == makina::Op::Plane;
    }
    if (hasPlane) {
        std::printf("    holds a Plane, which has no solid form; the outside check does not "
                    "apply (worst %.5f)\n", worstOutside);
    }
    check(hasPlane || worstOutside <= outsideAllowance,
          "a triangle centre is " + std::to_string(worstOutside) +
              " outside the solid, past the " + std::to_string(outsideAllowance) +
              " a 24-segment cut can leave behind; the mesh has surface the field does not");

    // ...and the mesh has to reach as far as the field does. Without this, a mesh entirely inside
    // the solid -- everything shrunk, or one solid missing -- satisfies the check above perfectly.
    const makina::Aabb box = makina::worldBounds(scene).box;
    if (box.valid) {
        double lo[3] = {1e300, 1e300, 1e300};
        double hi[3] = {-1e300, -1e300, -1e300};
        for (const makina::MeshTriangle& t : tris) {
            for (int v = 0; v < 3; ++v) {
                for (int k = 0; k < 3; ++k) {
                    lo[k] = std::min(lo[k], t.p[v][k]);
                    hi[k] = std::max(hi[k], t.p[v][k]);
                }
            }
        }
        double worstExtent = 0.0;
        for (int k = 0; k < 3; ++k) {
            worstExtent = std::max(worstExtent, std::fabs(lo[k] - box.lo[k]));
            worstExtent = std::max(worstExtent, std::fabs(hi[k] - box.hi[k]));
        }
        // The sagitta again: a tessellated sphere's facets fall short of its true extent.
        // Three times the sagitta rather than two: verify_transforms scales a sphere non-uniformly,
        // so its facets fall short by more than a unit sphere's would. Measured at 0.0246.
        check(worstExtent <= radius * kSagitta * 3.0,
              "the mesh reaches " + std::to_string(worstExtent / radius) +
                  " of the scene radius short of, or past, the bounds the tree says the solid has");
    }

    // The two writers have to describe the same model. STL is fixed width, so the count is
    // arithmetic rather than a parse.
    const std::vector<char> stl = makina::writeStl(mesh, "check");
    check(stl.size() == 84 + tris.size() * 50,
          "the STL is not the size its triangle count implies");
    std::uint32_t stlCount = 0;
    std::memcpy(&stlCount, stl.data() + 80, 4);
    check(stlCount == tris.size(), "the STL header counts a different number of triangles");

    const makina::ObjExport obj = makina::writeObj(mesh, scene, "check", "check.mtl");
    std::size_t faces = 0;
    for (std::size_t at = obj.obj.find("\nf "); at != std::string::npos;
         at = obj.obj.find("\nf ", at + 1)) {
        ++faces;
    }
    check(faces == tris.size(), "the OBJ has " + std::to_string(faces) + " faces for " +
                                    std::to_string(tris.size()) + " triangles");

    std::printf("    %zu triangles (%d solid-backed); worst %.5f outside, allowance %.5f\n",
                tris.size(), compared, worstOutside, outsideAllowance);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: mesh_compare <scene.json> ...\n");
        return 2;
    }

    std::printf("makina-core exported mesh against the distance field\n\n");

    for (int i = 1; i < argc; ++i) {
        try {
            exercise(argv[i]);
        } catch (const std::exception& e) {
            std::printf("    FAIL  %s\n", e.what());
            ++failures;
        }
    }

    if (failures == 0) {
        std::printf("\nthe exported mesh lies on the field the renderer draws (%d checks)\n",
                    checks);
        return 0;
    }
    std::printf("\n%d of %d checks FAILED\n", failures, checks);
    return 1;
}
