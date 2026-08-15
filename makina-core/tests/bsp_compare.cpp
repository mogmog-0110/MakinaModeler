// Checks the SDF evaluator against an independent boundary representation.
//
// Two implementations that share no code: Eval.hpp answers "how far to the surface" from formulas
// combined by min/max, Bsp.hpp answers "inside or outside" by counting ray crossings through
// polygons that a BSP tree cut against each other. They fail differently -- an SDF gets a boolean
// wrong by combining numbers that stopped being distances, a B-rep gets it wrong by losing a
// polygon at a coplanar face -- so agreement is evidence, not tautology.
//
// Three kinds of sample are deliberately not counted, and each for a reason that would otherwise
// turn a known approximation into a false alarm:
//
//   near the surface   the tessellation is faceted, so it sits up to one sagitta inside the true
//                      surface. `tessellationError` bounds that; inside the band the two are
//                      allowed to disagree and the sample says nothing
//   grazing rays       a ray that clips a polygon edge cannot be counted by parity
//   incomplete meshes  a scene holding a Plane has no boundary representation at all. That is
//                      reported as a skip, not passed over in silence
//
// No reference dump: there is nothing on the Java side to compare against, because this asks
// whether two of *our* paths agree.

#include <makina/Eval.hpp>
#include <makina/SceneJson.hpp>
#include <makina/TessellateScene.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;
long checks = 0;

/// Lattice resolution per axis. 24^3 is 13,824 points per scene, which the BSP can answer for in
/// about a second; the polygon count, not the lattice, is what costs.
constexpr int kLattice = 24;

/// A disagreement rate this small is the tessellation being coarse where the error bound is
/// optimistic -- a torus tube is drawn with kTubeBands, not kSegments, so its sagitta is larger
/// than the bound assumes. Above it, something is actually wrong.
constexpr double kAllowedDisagreement = 0.01;

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("could not open '" + path + "'");
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// Whether any primitive in the scene encloses a volume. Disc, Triangle and Plane do not.
bool hasSolidPrimitive(const makina::Scene& s) {
    for (std::uint32_t i = 0; i < s.nodes.count; ++i) {
        const makina::Op op = static_cast<makina::Op>(s.nodes[i].op);
        if (op == makina::Op::Box || op == makina::Op::Sphere || op == makina::Op::Cylinder ||
            op == makina::Op::Cone || op == makina::Op::Torus) {
            return true;
        }
    }
    return false;
}

void compareOne(const std::string& jsonPath) {
    std::printf("%s\n", jsonPath.c_str());

    const makina::Scene scene = makina::parseScene(readFile(jsonPath));
    const makina::TessellationResult mesh = makina::tessellate(scene);

    std::size_t polys = 0;
    for (const makina::BspSolid& s : mesh.solids) {
        polys += s.size();
    }

    if (!mesh.complete) {
        std::printf("    skipped: %s has no solid form (%zu solids, %zu polygons built)\n",
                    mesh.missing.c_str(), mesh.solids.size(), polys);
        return;
    }
    if (mesh.solids.empty()) {
        std::printf("    skipped: no solids\n");
        return;
    }

    const makina::BoundsResult b = makina::worldBounds(scene);
    if (!b.box.valid) {
        std::printf("    skipped: no bounds\n");
        return;
    }

    double radius = 0.0;
    for (int a = 0; a < 3; ++a) {
        radius = std::max(radius, 0.5 * (b.box.hi[a] - b.box.lo[a]));
    }
    const double band = makina::tessellationError(radius);

    long tested = 0, skippedBand = 0, grazing = 0, disagree = 0, interior = 0;
    double worst = 0.0;
    double worstP[3] = {0, 0, 0};

    for (int ix = 0; ix < kLattice; ++ix) {
        for (int iy = 0; iy < kLattice; ++iy) {
            for (int iz = 0; iz < kLattice; ++iz) {
                // Offset by half a cell so the lattice misses the planes of an axis-aligned box,
                // where every sample would land exactly on a face.
                const double u[3] = {(ix + 0.5) / kLattice, (iy + 0.5) / kLattice,
                                     (iz + 0.5) / kLattice};
                double p[3];
                for (int a = 0; a < 3; ++a) {
                    // Widen by a tenth so the outside is sampled too, not just the interior.
                    const double lo = b.box.lo[a] - 0.1 * radius;
                    const double hi = b.box.hi[a] + 0.1 * radius;
                    p[a] = lo + u[a] * (hi - lo);
                }

                const double d = makina::eval(scene, p);
                if (makina::isEmpty(d) || std::fabs(d) <= band) {
                    ++skippedBand;
                    continue;
                }
                const makina::Containment c = makina::containedBySolids(mesh.solids, p);
                if (c == makina::Containment::Grazing) {
                    ++grazing;
                    continue;
                }
                ++tested;
                ++checks;
                const bool sdfInside = d < 0.0;
                const bool meshInside = c == makina::Containment::Inside;
                if (sdfInside) {
                    ++interior;
                }
                if (sdfInside != meshInside) {
                    ++disagree;
                    if (std::fabs(d) > worst) {
                        worst = std::fabs(d);
                        worstP[0] = p[0];
                        worstP[1] = p[1];
                        worstP[2] = p[2];
                    }
                }
            }
        }
    }

    if (tested == 0) {
        std::printf("    skipped: every sample fell in the %.4g band or grazed\n", band);
        return;
    }

    const double rate = static_cast<double>(disagree) / static_cast<double>(tested);
    std::printf("    %zu solids, %zu polygons; %ld tested, %ld interior (%ld in band, %ld grazing)\n",
                mesh.solids.size(), polys, tested, interior, skippedBand, grazing);

    // A run where nothing landed inside would report a perfect score without having tested the one
    // thing that matters: both sides agree trivially that empty space is empty.
    //
    // A scene made only of Disc, Triangle and Plane has no interior to find. The SDF says so by
    // contract -- a face has no thickness, so its distance is never negative (Eval.hpp) -- while
    // the mesh gives it the PatchSolid slab. There is nothing to compare, and saying so is not the
    // same as saying it passed.
    if (interior == 0) {
        if (hasSolidPrimitive(scene)) {
            std::printf("    FAIL  no sample fell inside the solid; the comparison proved "
                        "nothing\n");
            ++failures;
        } else {
            std::printf("    skipped: only zero-thickness faces, so the SDF has no interior\n");
        }
        return;
    }

    if (rate > kAllowedDisagreement) {
        std::printf("    FAIL  %ld of %ld disagree (%.2f%%), worst |d| %.4g at (%.4g, %.4g, %.4g)\n",
                    disagree, tested, rate * 100.0, worst, worstP[0], worstP[1], worstP[2]);
        ++failures;
    } else {
        std::printf("    the SDF and the boundary agree (%ld disagree, %.3f%%)\n", disagree,
                    rate * 100.0);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: bsp_compare <scene.json> ...\n");
        return 2;
    }

    std::printf("makina-core SDF vs boundary representation\n\n");

    for (int i = 1; i < argc; ++i) {
        try {
            compareOne(argv[i]);
        } catch (const std::exception& e) {
            std::printf("    FAIL  %s\n", e.what());
            ++failures;
        }
    }

    if (failures == 0) {
        std::printf("\nthe two representations agree (%ld samples)\n", checks);
        return 0;
    }
    std::printf("\n%d scene(s) FAILED over %ld samples\n", failures, checks);
    return 1;
}
