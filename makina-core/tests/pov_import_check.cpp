// Reading POV-Ray back, and the two things that make it worth having.
//
// **Round trip.** A scene exported to POV and read back has to be the same solid. Not the same
// file -- the exporter writes transforms as text and the reader turns them into nodes, so the tree
// differs by construction. What must survive is the geometry and the appearance, and the way to
// ask is to sample the distance field of both and to compare, surface by surface, the bytes the
// GPU would be handed.
//
// The appearance half was added after the geometry half had reported an exact match for months
// while the reader was throwing away every `filter` it read. "The round trip is exact" is only
// worth what it names: this one now names both.
//
// **Refusal.** The other half is that anything not represented stops the read or is named. A
// reader that silently drops what it cannot do produces a scene that loaded, rendered, and is not
// the one in the file, and there is no later check that catches it.
//
// The sample scenes under tests/scenes/pov are not read here. They are hand-written files full of
// macros and radiosity, so what they exercise is the refusal path, and a test whose expected
// result is "refuses for one of eleven possible reasons" asserts nothing. tools/povimport reads
// them and prints what it could not take.

#include <makina/Bounds.hpp>
#include <makina/Eval.hpp>
#include <makina/Flatten.hpp>
#include <makina/Pov.hpp>
#include <makina/PovImport.hpp>
#include <makina/PovSurvey.hpp>
#include <makina/RenderMaterial.hpp>
#include <makina/SceneJson.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

/// Samples the field on a lattice through the scene's own bounds.
std::vector<double> sample(const makina::Scene& s) {
    const makina::Aabb box = makina::worldBounds(s).box;
    std::vector<double> out;
    if (!box.valid) {
        return out;
    }
    // Padded, so points outside the solid are sampled too: a reader that produced an empty scene
    // would otherwise agree perfectly with one that produced a solid, on no points at all.
    double lo[3], hi[3];
    for (int i = 0; i < 3; ++i) {
        const double pad = (box.hi[i] - box.lo[i]) * 0.25 + 0.1;
        lo[i] = box.lo[i] - pad;
        hi[i] = box.hi[i] + pad;
    }
    constexpr int kSteps = 11;
    for (int i = 0; i < kSteps; ++i) {
        for (int j = 0; j < kSteps; ++j) {
            for (int k = 0; k < kSteps; ++k) {
                const double p[3] = {
                    lo[0] + (hi[0] - lo[0]) * i / (kSteps - 1),
                    lo[1] + (hi[1] - lo[1]) * j / (kSteps - 1),
                    lo[2] + (hi[2] - lo[2]) * k / (kSteps - 1)};
                out.push_back(makina::eval(s, p));
            }
        }
    }
    return out;
}

/// The largest radius any bound reaches, for scaling a tolerance to the model.
double scaleOf(const makina::Scene& s) {
    const makina::Aabb box = makina::worldBounds(s).box;
    if (!box.valid) {
        return 1.0;
    }
    double r = 0.0;
    for (int i = 0; i < 3; ++i) {
        r = std::fmax(r, box.hi[i] - box.lo[i]);
    }
    return r > 1e-9 ? r : 1.0;
}

/// The lamps, before and after.
///
/// Two of this model's light fields cannot survive POV and are named rather than tolerated. A
/// directional light is exported as a point light ten thousand units away, because POV has no
/// other way to say it, and nothing in the file distinguishes that from a genuinely distant lamp.
/// `softness` has no POV form at all -- a point light there casts a hard shadow.
void compareLights(const makina::Scene& before, const makina::Scene& after) {
    bool exportable = true;
    for (std::uint32_t i = 0; i < before.lights.count; ++i) {
        if (before.lights[i].directional != 0u || before.lights[i].softness != 0.0f) {
            exportable = false;
        }
    }
    if (!exportable) {
        std::printf("    lights not compared: a directional or soft light has no POV form\n");
        return;
    }

    check(before.lights.count == after.lights.count,
          "the scene came back with " + std::to_string(after.lights.count) + " lights instead of " +
              std::to_string(before.lights.count));
    if (before.lights.count != after.lights.count) {
        return;
    }

    float worst = 0.0f;
    std::size_t worstField = 0;
    for (std::uint32_t i = 0; i < before.lights.count; ++i) {
        const makina::Light& x = before.lights[i];
        const makina::Light& y = after.lights[i];
        // The flags are named rather than swept up with the floats. Reading a uint32 through a
        // float pointer turns the value 1 into a denormal around 1e-45, so a lamp that lost its
        // `shadowless` differed by less than any tolerance worth having -- which is what the
        // negative control found when it was written that way and did not fail.
        check(x.directional == y.directional, "a light changed between point and directional");
        check(x.shadowless == y.shadowless, "a light gained or lost its shadowless");

        const float mine[7] = {x.position[0], x.position[1], x.position[2],
                               x.color[0],    x.color[1],    x.color[2], x.fadeDistance};
        const float theirs[7] = {y.position[0], y.position[1], y.position[2],
                                 y.color[0],    y.color[1],    y.color[2], y.fadeDistance};
        for (std::size_t f = 0; f < 7; ++f) {
            const float d = std::fabs(mine[f] - theirs[f]);
            if (d > worst) { worst = d; worstField = f; }
        }
        const float dp = std::fabs(x.fadePower - y.fadePower);
        if (dp > worst) { worst = dp; worstField = 7; }
    }
    check(worst <= 1.0e-5f, "light field " + std::to_string(worstField) + " differs by " +
                                std::to_string(worst) + " after the round trip");
    if (before.lights.count > 0) {
        std::printf("    %u lights, worst difference %.2e\n", before.lights.count,
                    static_cast<double>(worst));
    }
}

/// A lattice through the scene's bounds, the same one the geometry half samples.
std::vector<std::array<double, 3>> latticePoints(const makina::Scene& s) {
    const makina::Aabb box = makina::worldBounds(s).box;
    std::vector<std::array<double, 3>> out;
    if (!box.valid) {
        return out;
    }
    double lo[3], hi[3];
    for (int i = 0; i < 3; ++i) {
        const double pad = (box.hi[i] - box.lo[i]) * 0.25 + 0.1;
        lo[i] = box.lo[i] - pad;
        hi[i] = box.hi[i] + pad;
    }
    constexpr int kSteps = 11;
    for (int i = 0; i < kSteps; ++i) {
        for (int j = 0; j < kSteps; ++j) {
            for (int k = 0; k < kSteps; ++k) {
                out.push_back({lo[0] + (hi[0] - lo[0]) * i / (kSteps - 1),
                               lo[1] + (hi[1] - lo[1]) * j / (kSteps - 1),
                               lo[2] + (hi[2] - lo[2]) * k / (kSteps - 1)});
            }
        }
    }
    return out;
}

/// Every float the surface nearest a point carries, in one array, so a mismatch names an index.
///
/// Read through the program's own selection rules rather than off a node, because a node's
/// material is not always the one that reaches the picture: a difference takes the body's, and
/// the exporter writes the .pov that way too. Comparing per node would report a disagreement
/// about a value neither renderer ever looks at.
std::vector<float> appearanceAt(const makina::Scene& s, const makina::EvalProgram& prog,
                                const std::array<double, 3>& p) {
    const makina::ProgramSurface hit = makina::evalProgramSurface(prog, p.data());
    const makina::GpuMaterial m =
        hit.materialId < s.materials.count ? makina::toGpuMaterial(s.materials[hit.materialId])
                                           : makina::defaultGpuMaterial();
    std::vector<float> v;
    const float* mf = reinterpret_cast<const float*>(&m);
    for (std::size_t i = 0; i < sizeof(makina::GpuMaterial) / sizeof(float); ++i) {
        v.push_back(mf[i]);
    }
    // A pattern and the space it stands in. -1 for "no pattern", so a surface that gained or lost
    // one differs at this entry rather than reading past the table.
    if (hit.pigmentId == makina::kNoPigment || hit.pigmentId >= prog.pigments.size()) {
        v.push_back(-1.0f);
        return v;
    }
    const makina::GpuPigment& g = prog.pigments[hit.pigmentId];
    v.push_back(static_cast<float>(g.pattern.type));
    v.push_back(static_cast<float>(g.pattern.stopCount));
    for (int st = 0; st < g.pattern.stopCount; ++st) {
        for (int i = 0; i < 4; ++i) { v.push_back(g.pattern.stop[st][i]); }
    }
    for (int i = 0; i < 3; ++i) { v.push_back(g.pattern.scale[i]); }
    for (int i = 0; i < 3; ++i) { v.push_back(g.pattern.translate[i]); }
    for (int i = 0; i < 3; ++i) { v.push_back(g.pattern.axis[i]); }
    for (int i = 0; i < 12; ++i) { v.push_back(g.inv[i]); }
    return v;
}

/// The appearance half of the round trip: what paints each sampled point, before and after.
void compareAppearance(const makina::Scene& before, const makina::Scene& after) {
    const makina::EvalProgram pa = makina::flatten(before);
    const makina::EvalProgram pb = makina::flatten(after);
    const std::vector<std::array<double, 3>> pts = latticePoints(before);

    float worst = 0.0f;
    float worstBefore = 0.0f;
    float worstAfter = 0.0f;
    std::size_t worstField = 0;
    std::size_t compared = 0;
    for (const std::array<double, 3>& p : pts) {
        const std::vector<float> a = appearanceAt(before, pa, p);
        const std::vector<float> b = appearanceAt(after, pb, p);
        if (a.size() != b.size()) {
            check(false, "a point gained or lost its pattern in the round trip");
            return;
        }
        ++compared;
        for (std::size_t f = 0; f < a.size(); ++f) {
            const float d = std::fabs(a[f] - b[f]);
            if (d > worst) {
                worst = d;
                worstField = f;
                worstBefore = a[f];
                worstAfter = b[f];
            }
        }
    }
    // Everything compared is a color in 0..1, a small exponent, or a matrix entry of the same
    // order as the scene, and the file carries them as decimal text. A part in a hundred thousand
    // is what that costs; larger is a value that changed, not one that was printed.
    check(worst <= 1.0e-5f,
          "field " + std::to_string(worstField) + " went from " + std::to_string(worstBefore) +
              " to " + std::to_string(worstAfter) + " in the round trip");
    std::printf("    %zu points, worst appearance difference %.2e\n", compared,
                static_cast<double>(worst));
}

/// The survey's promise, held against the reader's behaviour.
///
/// povSurvey() is a table and tables drift: a shape the importer learns stays "unsupported" in
/// the survey, or the survey blesses a word the importer still refuses. Either way the report
/// card lies to whoever is deciding if an internet file is usable. So on every exported fixture
/// -- which the importer reads whole by construction -- the survey must come back clean, and on
/// a probe that names an unsupported shape it must not.
void surveyAgreesWithReader(const std::string& pov) {
    const makina::PovSurveyResult clean = makina::povSurvey(pov);
    if (!clean.clean) {
        for (const makina::PovSurveyItem& item : clean.items) {
            if (item.status != makina::PovStatus::Supported &&
                item.status != makina::PovStatus::Ignored) {
                throw std::runtime_error("the survey flags '" + item.name +
                                         "' in a file the importer reads whole");
            }
        }
    }
    const makina::PovSurveyResult dirty =
        makina::povSurvey(pov + "\nlathe { linear_spline 3, <0,0>, <1,1>, <0,2> }");
    if (dirty.clean) {
        throw std::runtime_error("the survey missed a 'lathe' appended to the file");
    }
}

void roundTrip(const std::string& path) {
    std::printf("%s\n", path.c_str());
    // The solid, not the tree that was authored. A .pov has no way to say "this node is muted",
    // so the only thing a round trip can preserve is the shape the mute leaves behind -- and
    // handing the exporter the authored tree would write out a node the modeller does not draw.
    const makina::Scene original = makina::withoutMuted(makina::parseScene(readFile(path)));

    // A warp (D-14) has no POV form the reader can take back: it goes out as an isosurface
    // function, which is a picture for POV to draw and not a tree to read. One direction only,
    // and named as such; the JSON round trip is where a warp's fidelity is held.
    for (std::uint32_t i = 0; i < original.nodes.count; ++i) {
        if (makina::isWarp(static_cast<makina::Op>(original.nodes[i].op))) {
            std::printf("    holds a warp, which POV cannot hand back; the round trip does not "
                        "apply\n");
            return;
        }
    }

    makina::PovOptions opt;
    opt.silhouette = false;
    // The lamps go in the preamble, which is where the renderer puts them too. Leaving it
    // empty would export a scene with no lights and read back a scene with no lights, and the
    // comparison would agree about nothing.
    opt.preamble = makina::detail::povLights(original);
    const std::string pov = makina::writePov(original, opt);
    surveyAgreesWithReader(pov);

    makina::PovImportResult back;
    try {
        back = makina::importPov(pov);
    } catch (const makina::PovParseError& e) {
        // A refusal here is a finding, not a pass. The exporter wrote this file, so anything it
        // writes that the reader will not take is a gap between two halves of one format.
        check(false, std::string("the reader refused what the exporter wrote: ") + e.what());
        return;
    }

    const std::vector<double> a = sample(original);
    const std::vector<double> b = sample(back.scene);
    check(!a.empty(), "the original scene has no bounds to sample");
    check(a.size() == b.size(), "the two scenes sample to different numbers of points");
    if (a.empty() || a.size() != b.size()) {
        return;
    }

    // Distances, not just signs. Matching signs would pass a scene scaled by a half.
    const double tolerance = scaleOf(original) * 1e-4;
    double worst = 0.0;
    int wrongSide = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (makina::isEmpty(a[i]) || makina::isEmpty(b[i])) {
            continue;
        }
        worst = std::fmax(worst, std::fabs(a[i] - b[i]));
        if ((a[i] < 0.0) != (b[i] < 0.0)) {
            ++wrongSide;
        }
    }
    check(wrongSide == 0, std::to_string(wrongSide) + " sample point(s) are inside one scene and "
                          "outside the other");
    check(worst <= tolerance,
          "the field differs by " + std::to_string(worst) + ", past the " +
              std::to_string(tolerance) + " that float storage accounts for");

    std::printf("    %u nodes -> %u after the round trip, worst field difference %.2e\n",
                original.nodes.count, back.scene.nodes.count, worst);
    compareAppearance(original, back.scene);
    compareLights(original, back.scene);
}

/// Reads a fragment and expects it to be refused, with the reason mentioning `because`.
void refuses(const std::string& source, const std::string& because) {
    try {
        (void)makina::importPov(source);
        check(false, "this was read without complaint and should not have been: " + because);
    } catch (const makina::PovParseError& e) {
        const std::string msg = e.what();
        check(msg.find(because) != std::string::npos,
              "refused, but the reason does not mention '" + because + "': " + msg);
    }
}

/// Reads a fragment that must succeed while naming something it could not represent.
void notes(const std::string& source, const std::string& what) {
    try {
        const makina::PovImportResult r = makina::importPov(source);
        bool found = false;
        for (const std::string& s : r.unsupported) {
            if (s.find(what) != std::string::npos) {
                found = true;
            }
        }
        check(found, "'" + what + "' was not reported as unread");
    } catch (const makina::PovParseError& e) {
        check(false, std::string("refused instead of reporting: ") + e.what());
    }
}

void boundaries() {
    std::printf("what the reader will not take\n");

    // Geometry this model cannot hold. Each of these would move or reshape the solid if guessed.
    refuses("cone{<0,0,0>,1,<0,2,0>,0.5}", "truncated");
    refuses("cylinder{<1,1,1>,<1,1,1>,0.5}", "distinct end points");
    refuses("sphere{<0,0,0>,-1}", "positive radius");
    refuses("plane{<0,0,0>,0}", "a direction");
    refuses("disc{<0,0,0>,<0,0,0>,1}", "a direction");

    // Shapes POV can trace and this model cannot hold. The message has to name the shape: the
    // word would otherwise reach the expression parser and be reported as not being a number,
    // which says where the parser was rather than what the file asked for.
    refuses("#declare S = sor { 3, <0,0>,<1,1>,<0,2> }", "at least four points");
    refuses("isosurface { function { x*x } }", "given by a function");
    refuses("sphere{<0,0,0>,rand(R)}", "not one");
    refuses("box{<0,0,0>,<1,1,1> scale <0,1,1>}", "collapses");
    refuses("object{Nope}", "not a declared object");
    refuses("union{sphere{<0,0,0>,1}", "not closed");

    // Recognised, representable geometry, with something on it that is not.
    notes("sphere{<0,0,0>,1 normal{bumps 0.1}}", "normal");
    notes("sphere{<0,0,0>,1 no_shadow}", "no_shadow");
    notes("global_settings{ radiosity{} } sphere{<0,0,0>,1}", "global_settings");
    // An interior is read for its ior and reports the rest of itself rather than going quiet:
    // a scene whose fog was dropped renders happily and is not the scene in the file.
    notes("sphere{<0,0,0>,1 pigment{color rgbf<1,1,1,0.5>} interior{ior 1.5 fade_power 2}}",
          "fade_power");

    // The index of refraction, and the order it may arrive in. POV hangs it on the object and the
    // texture on the surface, so a file is free to write either first, and the second must not
    // undo the first.
    try {
        const char* const both[2] = {
            "sphere{<0,0,0>,1 pigment{color rgbf<1,1,1,0.5>} interior{ior 1.5}}",
            "sphere{<0,0,0>,1 interior{ior 1.5} pigment{color rgbf<1,1,1,0.5>}}"};
        for (int i = 0; i < 2; ++i) {
            const makina::PovImportResult r = makina::importPov(both[i]);
            check(r.scene.materials.count == 1, "the sphere should carry one material");
            check(r.scene.materials[0].ior == 1.5f, "ior was lost");
            check(r.scene.materials[0].alpha == 0.5f, "the pigment was lost");
        }
    } catch (const makina::PovParseError& e) {
        check(false, std::string("interior{ior} was refused: ") + e.what());
    }

    // A cylinder that is not along an axis, checked against where the solid has to be rather than
    // against the transform the reader built. Stating the expected geometry independently is the
    // point: the reader and a test that recomputed its own rotation would agree with each other
    // and both be wrong.
    try {
        const makina::PovImportResult r =
            makina::importPov("cylinder{<-1,0,-1>,<1,2,1>,0.3}");
        const double from[3] = {-1.0, 0.0, -1.0};
        const double to[3]   = {1.0, 2.0, 1.0};

        int inside = 0;
        for (int i = 1; i < 8; ++i) {
            const double t = i / 8.0;
            const double p[3] = {from[0] + (to[0] - from[0]) * t,
                                 from[1] + (to[1] - from[1]) * t,
                                 from[2] + (to[2] - from[2]) * t};
            if (makina::eval(r.scene, p) < 0.0) {
                ++inside;
            }
        }
        check(inside == 7, std::to_string(inside) + " of 7 points on the axis are inside the "
                           "cylinder; a mis-turned one would miss most of them");

        // Beyond either end, and off to the side: a cylinder that came out longer or fatter than
        // asked would swallow these.
        const double past[3] = {1.6, 2.6, 1.6};
        check(makina::eval(r.scene, past) > 0.0, "a point past the far cap reads as inside");
        // Perpendicular to the axis, well outside the radius. The axis runs along (2,2,2), so
        // (1,-1,0) is at right angles to it.
        const double side[3] = {0.6, -0.6, 0.0};
        check(makina::eval(r.scene, side) > 0.0,
              "a point beside the axis, further out than the radius, reads as inside");
    } catch (const makina::PovParseError& e) {
        check(false, std::string("a slanted cylinder was refused: ") + e.what());
    }

    // A plane facing the other way. POV's `-y` keeps the half above zero, and getting the sense
    // backwards would produce the complement -- a solid that fills everything the file says is
    // empty, which still renders.
    try {
        const makina::PovImportResult up = makina::importPov("plane{-y,0}");
        const double above[3] = {0.0, 1.0, 0.0};
        const double below[3] = {0.0, -1.0, 0.0};
        check(makina::eval(up.scene, above) < 0.0, "plane{-y,0} should keep the half above zero");
        check(makina::eval(up.scene, below) > 0.0, "plane{-y,0} should not keep the half below");

        const makina::PovImportResult down = makina::importPov("plane{y,0}");
        check(makina::eval(down.scene, below) < 0.0, "plane{y,0} should keep the half below zero");
        check(makina::eval(down.scene, above) > 0.0, "plane{y,0} should not keep the half above");
    } catch (const makina::PovParseError& e) {
        check(false, std::string("a plane was refused: ") + e.what());
    }

    // A transform declared once and used twice, which is how a hand-written file places a
    // repeated part. The vector arithmetic is part of it: `rotate x*20` is not a literal.
    try {
        const makina::PovImportResult r = makina::importPov(
            "#declare Turn = transform { rotate y*90 }\n"
            "box{<-2,-0.2,-0.2>,<2,0.2,0.2> transform{Turn}}");
        // Turned a quarter about Y, the long axis runs along z.
        const makina::Aabb box = makina::worldBounds(r.scene).box;
        check(box.valid && std::fabs(box.hi[2] - 2.0) < 1e-4 && std::fabs(box.hi[0] - 0.2) < 1e-4,
              "a box turned 90 degrees about Y should be long in z and short in x");
    } catch (const makina::PovParseError& e) {
        check(false, std::string("a declared transform was refused: ") + e.what());
    }

    // The whole point of the intermediate tree: an instance carries a transform of its own, and
    // the wrapper is created after the subtree it wraps.
    try {
        const makina::PovImportResult r = makina::importPov(
            "#declare Ball = sphere{<0,0,0>,1}\n"
            "union{ object{Ball translate <2,0,0>} object{Ball translate <-2,0,0>} }");
        check(r.scene.nodes.count == 6,
              "an instanced sphere under two translates should be 6 nodes, not " +
                  std::to_string(r.scene.nodes.count));
        // Every node's children must be contiguous, which is the invariant building directly into
        // the flat scene could not hold.
        for (std::uint32_t i = 0; i < r.scene.nodes.count; ++i) {
            const makina::CsgNode& n = r.scene.nodes[i];
            for (std::uint16_t c = 0; c < n.childCount; ++c) {
                const std::uint16_t at = static_cast<std::uint16_t>(n.firstChild + c);
                check(r.scene.nodes[at].parent == i,
                      "node " + std::to_string(at) + " does not name its parent");
            }
        }
        const makina::Aabb box = makina::worldBounds(r.scene).box;
        check(box.valid && std::fabs(box.lo[0] + 3.0) < 1e-5 && std::fabs(box.hi[0] - 3.0) < 1e-5,
              "the two instances should span -3..3 in x");
    } catch (const makina::PovParseError& e) {
        check(false, std::string("instancing was refused: ") + e.what());
    }
}

/// #macro definition, call, parameter substitution, and the refusals around them.
///
/// Geometry is asserted through world bounds, stated independently of the expansion: a macro
/// that expanded to the wrong tokens would still parse into *something*, and only the resulting
/// solid's extent tells the two apart.
void macroExpansion() {
    std::printf("#macro expands at the call\n");

    // A parameter used twice, once negated, and an expression as the argument.
    const char* const kPair =
        "#macro Pair(S) union { sphere{<S,0,0>,1} sphere{<-S,0,0>,1} } #end\n";
    for (const char* call : {"object { Pair(2) }", "object { Pair(1+1) }"}) {
        const makina::PovImportResult r = makina::importPov(std::string(kPair) + call);
        const makina::Aabb box = makina::worldBounds(r.scene).box;
        check(box.valid && std::fabs(box.lo[0] + 3.0) < 1e-5 && std::fabs(box.hi[0] - 3.0) < 1e-5,
              std::string("'") + call + "' should span -3..3 in x");
    }

    // A macro whose body calls another macro: the spliced tokens are read by the same parser,
    // so the inner call must expand too.
    const makina::PovImportResult nested = makina::importPov(
        "#macro Ball() sphere{<0,0,0>,1} #end\n"
        "#macro Two() union { object{ Ball() } object{ Ball() translate <4,0,0> } } #end\n"
        "object { Two() }");
    const makina::Aabb box = makina::worldBounds(nested.scene).box;
    check(box.valid && std::fabs(box.lo[0] + 1.0) < 1e-5 && std::fabs(box.hi[0] - 5.0) < 1e-5,
          "a macro called from a macro should place both spheres");

    refuses("#macro Pair(S) union { sphere{<S,0,0>,1} } #end object { Pair(1,2) }", "expects");
    refuses("#macro Loop() object { Loop() } #end object { Loop() }", "does not terminate");
    refuses("#macro F() sphere{<0,0,0>,1}", "not closed with #end");
}

/// blob: reading, the field it evaluates to, and the write/read round trip.
///
/// The geometry oracle is the same closed form blob_check uses: with threshold 0.5625 and
/// strength 1 a component's surface sits at r* = 0.5 exactly, so a misread threshold, radius,
/// strength or component transform all move a number this can see.
void blobImport() {
    std::printf("blob reads, evaluates, and round-trips\n");
    const double rStar = 0.5;

    const std::string src =
        "#declare B = blob {\n"
        "  threshold 0.5625\n"
        "  sphere { <0,0,0>, 1, 1 scale <1,0.5,0.5> }\n"
        "  cylinder { <6,-0.5,0>, <6,0.5,0>, 1 strength 1 }\n"
        "}\n"
        "object { B translate <0,2,0> }\n";
    const auto at = [](const makina::Scene& s, double x, double y, double z) {
        const double p[3] = {x, y, z};
        return makina::eval(s, p);
    };

    try {
        const makina::PovImportResult r = makina::importPov(src);
        check(r.unsupported.empty(), "the blob should read whole");
        check(std::fabs(at(r.scene, rStar, 2, 0)) < 1e-6,
              "the sphere component's surface should sit at r* along the unscaled axis");
        check(at(r.scene, 0, 2, 0) < 0.0, "the sphere component's center should be inside");
        check(std::fabs(at(r.scene, 6.0 + rStar, 2, 0)) < 1e-6,
              "the cylinder component's surface should sit r* off the axis");

        makina::PovOptions opt;
        const makina::PovImportResult back = makina::importPov(makina::writePov(r.scene, opt));
        // The exporter always writes a camera and the reader always reports the frame; anything
        // beyond that one line is a real loss.
        std::string lost;
        for (const std::string& u : back.unsupported) {
            if (u != "camera") {
                lost += " [" + u + "]";
            }
        }
        check(lost.empty(), "the exported blob should read back whole, not report" + lost);
        const double pts[4][3] = {{rStar, 2, 0}, {0, 2, 0}, {6.0 + rStar, 2, 0}, {1.5, 2, 0}};
        for (const double* p : pts) {
            check(std::fabs(at(r.scene, p[0], p[1], p[2]) - at(back.scene, p[0], p[1], p[2])) <
                      1e-6,
                  "the round-tripped blob evaluates differently");
        }
    } catch (const makina::PovParseError& e) {
        check(false, std::string("the blob was refused: ") + e.what());
    }

    // Left out, the threshold is POV's default 1.0: strength 2 then puts r* at
    // sqrt(1 - sqrt(1/2)).
    try {
        const makina::PovImportResult d = makina::importPov("blob { sphere{<0,0,0>,1,2} }");
        const double rDefault = std::sqrt(1.0 - std::sqrt(0.5));
        check(std::fabs(at(d.scene, rDefault, 0, 0)) < 1e-6,
              "the default threshold should be 1.0");
    } catch (const makina::PovParseError& e) {
        check(false, std::string("the default-threshold blob was refused: ") + e.what());
    }

    // An unknown word must refuse, not skip: a component skipped as "some modifier" would load a
    // solid with a lump missing.
    refuses("blob { threshold 0.5 hierarchy off sphere{<0,0,0>,1,1} }", "inside a blob");
    refuses("blob { sphere{<0,0,0>,1,1 hollow} }", "on a blob component");
    refuses("blob { sphere{<0,0,0>,-1,1} }", "positive radius");
    notes("blob { sphere{<0,0,0>,1,1 texture{pigment{color rgb<1,0,0>}}} }",
          "blob component texture");
}

/// sor: reading, the solid it evaluates to, and the write/read round trip.
///
/// The geometry oracles: a profile whose radii are all equal must evaluate as the capped
/// cylinder it describes (straight segments, so the polyline walk is exact), and any profile
/// must pass through its interior control points -- the spline interpolates them by
/// construction, so a surface that misses one misread the file.
void sorImport() {
    std::printf("sor reads, evaluates, and round-trips\n");
    const auto at = [](const makina::Scene& s, double x, double y, double z) {
        const double p[3] = {x, y, z};
        return makina::eval(s, p);
    };

    try {
        const makina::PovImportResult r = makina::importPov(
            "sor { 4, <0.5,-0.5>, <0.5,0>, <0.5,1>, <0.5,1.5> }");
        check(r.unsupported.empty(), "the cylinder-profile sor should read whole");
        const double probes[5][3] = {
            {0.9, 0.5, 0}, {0.2, 0.5, 0}, {0, 0.5, 0}, {0, 1.4, 0}, {0.3, -0.2, 0}};
        for (const double* p : probes) {
            const double rho = std::sqrt(p[0] * p[0] + p[2] * p[2]);
            const double dr = rho - 0.5;
            const double dy = std::fabs(p[1] - 0.5) - 0.5;
            const double truth = dr > 0.0 && dy > 0.0 ? std::sqrt(dr * dr + dy * dy)
                                                      : (dr > dy ? dr : dy);
            check(std::fabs(at(r.scene, p[0], p[1], p[2]) - truth) < 1e-6,
                  "the flat-profile sor should evaluate as its capped cylinder");
        }
    } catch (const makina::PovParseError& e) {
        check(false, std::string("the cylinder-profile sor was refused: ") + e.what());
    }

    // pingu.pov's body profile, verbatim: the surface must pass through the interior points.
    const char* const kBody =
        "sor { 8, <0.14,-0.12>, <0.03,0.02>, <0.34,0.14>, <0.508,0.42>, <0.472,0.84>, "
        "<0.385,1.24>, <0.03,1.50>, <0.14,1.64> sturm }";
    try {
        const makina::PovImportResult r = makina::importPov(kBody);
        check(r.unsupported.empty(), "the body-profile sor should read whole");
        const double interior[6][2] = {{0.03, 0.02},  {0.34, 0.14}, {0.508, 0.42},
                                       {0.472, 0.84}, {0.385, 1.24}, {0.03, 1.50}};
        for (const double* q : interior) {
            check(std::fabs(at(r.scene, q[0], q[1], 0)) < 1e-4,
                  "the surface should pass through the control point at h=" +
                      std::to_string(q[1]));
        }
        check(at(r.scene, 0, 0.7, 0) < 0.0, "the axis inside the body should be inside");
        check(at(r.scene, 1.0, 0.7, 0) > 0.0, "well off the profile should be outside");

        makina::PovOptions opt;
        const makina::PovImportResult back = makina::importPov(makina::writePov(r.scene, opt));
        const double pts[3][3] = {{0.508, 0.42, 0}, {0, 0.7, 0}, {0.6, 0.9, 0}};
        for (const double* p : pts) {
            check(std::fabs(at(r.scene, p[0], p[1], p[2]) - at(back.scene, p[0], p[1], p[2])) <
                      1e-6,
                  "the round-tripped sor evaluates differently");
        }
    } catch (const makina::PovParseError& e) {
        check(false, std::string("the body-profile sor was refused: ") + e.what());
    }

    refuses("sor { 4, <0.5,0>, <0.5,1>, <0.5,0.5>, <0.5,2> }", "strictly increase");
    refuses("sor { 4, <0.5,-0.5>, <0.5,0>, <0.5,1>, <0.5,1.5> open }", "no interior");
}

/// sphere_sweep: reading, the solid it evaluates to, and the write/read round trip.
///
/// Oracles: a two-point linear sweep with equal radii is a capsule, checked against an
/// independent capsule formula written here; and a b_spline through collinear, evenly spaced
/// points is a straight run whose ends the basis pins at the one-sixth blend of the outer
/// points -- four points at y = 0,1,2,3 must give exactly the capsule from y=1 to y=2. A wrong
/// basis moves those ends and this sees it.
void sweepImport() {
    std::printf("sphere_sweep reads, evaluates, and round-trips\n");
    const auto at = [](const makina::Scene& s, double x, double y, double z) {
        const double p[3] = {x, y, z};
        return makina::eval(s, p);
    };
    const auto capsule = [](double px, double py, double pz, double y0, double y1, double r) {
        const double t = py < y0 ? y0 : (py > y1 ? y1 : py);
        return std::sqrt(px * px + (py - t) * (py - t) + pz * pz) - r;
    };

    try {
        const makina::PovImportResult r = makina::importPov(
            "sphere_sweep { linear_spline 2, <0,0,0>, 0.3, <0,1,0>, 0.3 }");
        check(r.unsupported.empty(), "the linear sweep should read whole");
        const double probes[4][3] = {{0.5, 0.5, 0}, {0, -0.4, 0}, {0.1, 0.5, 0.1}, {0, 1.6, 0}};
        for (const double* p : probes) {
            // 1e-6, not tighter: the scene stores the points and radii as float32, and the
            // oracle here computes in double.
            check(std::fabs(at(r.scene, p[0], p[1], p[2]) -
                            capsule(p[0], p[1], p[2], 0.0, 1.0, 0.3)) < 1e-6,
                  "the two-point linear sweep should evaluate as its capsule");
        }
    } catch (const makina::PovParseError& e) {
        check(false, std::string("the linear sweep was refused: ") + e.what());
    }

    try {
        const makina::PovImportResult r = makina::importPov(
            "sphere_sweep { b_spline 4, <0,0,0>, 0.3, <0,1,0>, 0.3, <0,2,0>, 0.3, "
            "<0,3,0>, 0.3 }");
        const double probes[3][3] = {{0.5, 1.5, 0}, {0, 0.9, 0}, {0.2, 2.4, 0.1}};
        for (const double* p : probes) {
            check(std::fabs(at(r.scene, p[0], p[1], p[2]) -
                            capsule(p[0], p[1], p[2], 1.0, 2.0, 0.3)) < 1e-6,
                  "the collinear b_spline should run exactly from y=1 to y=2");
        }
    } catch (const makina::PovParseError& e) {
        check(false, std::string("the b_spline sweep was refused: ") + e.what());
    }

    // pingu.pov's FlipperUp, verbatim, plus the round trip.
    try {
        const makina::PovImportResult r = makina::importPov(
            "sphere_sweep {\n"
            "  b_spline 6\n"
            "  <-0.04, 1.02,  0.02>, 0.152\n"
            "  <-0.30, 1.26,  0.00>, 0.148\n"
            "  <-0.56, 1.66, -0.03>, 0.130\n"
            "  <-0.63, 2.04, -0.05>, 0.100\n"
            "  <-0.54, 2.26, -0.05>, 0.068\n"
            "  <-0.43, 2.33, -0.04>, 0.028\n"
            "  tolerance 0.00004\n"
            "  scale <1, 1, 0.78>\n"
            "}");
        check(r.unsupported.empty(), "the flipper sweep should read whole");
        makina::PovOptions opt;
        const makina::PovImportResult back = makina::importPov(makina::writePov(r.scene, opt));
        const double pts[3][3] = {{-0.4, 1.6, 0}, {-0.56, 1.66, 0}, {0.2, 2.0, 0.3}};
        for (const double* p : pts) {
            check(std::fabs(at(r.scene, p[0], p[1], p[2]) - at(back.scene, p[0], p[1], p[2])) <
                      1e-6,
                  "the round-tripped sweep evaluates differently");
        }
    } catch (const makina::PovParseError& e) {
        check(false, std::string("the flipper sweep was refused: ") + e.what());
    }

    // The comma after a skipped value: an area_light's argument list once hung the reader,
    // because skipValue could decline to move. The note must arrive, not a timeout.
    notes("light_source { <0,0,0> color rgb <1,1,1> area_light <2,0,0>, <0,2,0>, 9, 9 "
          "adaptive 1 jitter circular orient } sphere{<0,0,0>,1}",
          "area_light");

    refuses("sphere_sweep { cubic_spline 4, <0,0,0>, 1, <0,1,0>, 1, <0,2,0>, 1, <0,3,0>, 1 }",
            "not held yet");
    refuses("sphere_sweep { b_spline 3, <0,0,0>, 1, <0,1,0>, 1, <0,2,0>, 1 }",
            "at least four");
    refuses("sphere_sweep { linear_spline 2, <0,0,0>, 0, <0,1,0>, 1 }", "positive radius");
}

/// finish{diffuse} folds into the material exactly: pigment * d/0.6, ambient * 0.6/d, so POV's
/// pigment*(ambient + d*light) is unchanged term for term while the renderer keeps its baked
/// 0.6. Order must not matter -- POV's slots have none -- and a declared texture must carry its
/// diffuse to every object that wears it.
void finishDiffuse() {
    std::printf("finish diffuse reaches the material whichever order the blocks come in\n");
    const char* const kBoth[2] = {
        "sphere{<0,0,0>,1 pigment{color rgb <0.6,0.3,0.3>} finish{ambient 0.2 diffuse 0.9}}",
        "sphere{<0,0,0>,1 texture{finish{ambient 0.2 diffuse 0.9} pigment{color rgb "
        "<0.6,0.3,0.3>}}}"};
    for (const char* src : kBoth) {
        try {
            const makina::PovImportResult r = makina::importPov(src);
            check(r.unsupported.empty(), "the diffuse finish should read whole");
            check(r.scene.materials.count == 1, "one material expected");
            const makina::Material& m = r.scene.materials[0];
            check(std::fabs(m.diffuse[0] - 0.6f) < 1e-6f && std::fabs(m.diffuse[1] - 0.3f) < 1e-6f,
                  "the pigment should be held as written, not rescaled");
            check(std::fabs(m.finishDiffuse - 0.9f) < 1e-6f, "diffuse should reach the field");
            check(std::fabs(m.ambient - 0.2f) < 1e-6f, "the ambient should be held as written");
            check(makina::toGpuMaterial(m).diffuse == 0.9f, "diffuse should reach the shader");
        } catch (const makina::PovParseError& e) {
            check(false, std::string("the diffuse finish was refused: ") + e.what());
        }
    }

    try {
        const makina::PovImportResult r = makina::importPov(
            "#declare T = texture{pigment{color rgb <0.6,0.6,0.6>} finish{diffuse 0.3}}\n"
            "sphere{<0,0,0>,1 texture{T}}");
        check(std::fabs(r.scene.materials[0].finishDiffuse - 0.3f) < 1e-6f,
              "a declared texture should carry its diffuse to the object wearing it");

        makina::PovOptions opt;
        const std::string pov = makina::writePov(r.scene, opt);
        check(pov.find(" diffuse ") != std::string::npos, "the exporter should write diffuse");
        const makina::PovImportResult back = makina::importPov(pov);
        check(back.scene.materials.count == 1 &&
                  std::fabs(back.scene.materials[0].finishDiffuse - 0.3f) < 1e-6f,
              "diffuse should survive the POV round trip unchanged");
        const makina::Scene viaJson = makina::parseScene(makina::writeScene(r.scene));
        check(std::fabs(viaJson.materials[0].finishDiffuse - 0.3f) < 1e-6f,
              "diffuse should survive the JSON round trip");
    } catch (const makina::PovParseError& e) {
        check(false, std::string("the declared diffuse texture was refused: ") + e.what());
    }

    // A pattern with its own diffuse: the case the old color rescale could not hold, and the
    // reason the field exists (scene.pov's door is gradient x + diffuse 0.78).
    try {
        const makina::PovImportResult r = makina::importPov(
            "sphere{<0,0,0>,1 pigment{checker color rgb <1,0,0> color rgb <0,1,0>} "
            "finish{diffuse 0.9}}");
        check(r.unsupported.empty(), "a pattern with a diffuse should read whole");
        check(std::fabs(r.scene.materials[0].finishDiffuse - 0.9f) < 1e-6f,
              "the pattern's diffuse should reach the field");
    } catch (const makina::PovParseError& e) {
        check(false, std::string("the patterned diffuse was refused: ") + e.what());
    }

    // Unset stays unset in every file and shades as POV's 0.6.
    try {
        const makina::PovImportResult r = makina::importPov("sphere{<0,0,0>,1 pigment{color rgb 1}}");
        check(r.scene.materials[0].finishDiffuse == 0.0f, "an unset diffuse should stay zero");
        check(makina::toGpuMaterial(r.scene.materials[0]).diffuse == 0.6f,
              "an unset diffuse should shade as POV's 0.6");
        makina::PovOptions opt;
        check(makina::writePov(r.scene, opt).find(" diffuse ") == std::string::npos,
              "the exporter should not write an unset diffuse");
    } catch (const makina::PovParseError& e) {
        check(false, std::string("the plain sphere was refused: ") + e.what());
    }

    // The one case with no field form: zero is "unset", so diffuse 0 becomes a black color, and
    // beside a lit ambient that is named rather than silently wrong.
    notes("sphere{<0,0,0>,1 pigment{color rgb <1,0,0>} finish{ambient 0.3 diffuse 0}}",
          "finish diffuse 0");
}

/// color_map with more than two stops: read whole, kept through both round trips in order,
/// and one stop refused. Two stops at 0 and 1 still write the old colorA/colorB form so every
/// existing scene round trips byte for byte.
void colorMapStops() {
    std::printf("color_map stops survive both round trips in order\n");
    try {
        const makina::PovImportResult r = makina::importPov(
            "sphere{<0,0,0>,1 pigment{gradient x color_map{[0.0 color rgb <0.2,0.2,0.2>] "
            "[0.46 color rgb <0.5,0.5,0.5>] [0.5 color rgb <1,0,0>] [1.0 color rgb <0.9,0.9,0.9>]} "
            "scale 0.3}}");
        check(r.unsupported.empty(), "a four-stop map should read whole");
        check(r.scene.pigments.count == 1, "one pigment expected");
        const makina::Pigment& g = r.scene.pigments[0];
        check(g.stopCount == 4, "four stops expected, got " + std::to_string(g.stopCount));
        check(std::fabs(g.stop[1][3] - 0.46f) < 1e-6f && std::fabs(g.stop[2][0] - 1.0f) < 1e-6f,
              "stops should keep their positions and colors in file order");

        makina::PovOptions opt;
        const makina::PovImportResult back = makina::importPov(makina::writePov(r.scene, opt));
        check(back.scene.pigments.count == 1 && back.scene.pigments[0].stopCount == 4,
              "the POV round trip should keep all four stops");
        const makina::Scene viaJson = makina::parseScene(makina::writeScene(r.scene));
        check(viaJson.pigments.count == 1 && viaJson.pigments[0].stopCount == 4 &&
                  std::fabs(viaJson.pigments[0].stop[2][3] - 0.5f) < 1e-6f,
              "the JSON round trip should keep all four stops");
        check(makina::writeScene(r.scene).find("\"stops\"") != std::string::npos,
              "more than two stops should take the general JSON form");
    } catch (const makina::PovParseError& e) {
        check(false, std::string("the four-stop map was refused: ") + e.what());
    }

    // Two stops at 0 and 1 keep the old form, which is what keeps every existing scene the same.
    try {
        const makina::PovImportResult r = makina::importPov(
            "sphere{<0,0,0>,1 pigment{gradient y color_map{[0 color rgb 0] [1 color rgb 1]}}}");
        const std::string json = makina::writeScene(r.scene);
        check(json.find("\"colorA\"") != std::string::npos && json.find("\"stops\"") == std::string::npos,
              "two stops at 0 and 1 should still write colorA/colorB");
    } catch (const makina::PovParseError& e) {
        check(false, std::string("the two-stop map was refused: ") + e.what());
    }

    refuses("sphere{<0,0,0>,1 pigment{gradient x color_map{[0.5 color rgb 1]}}}",
            "at least two stops");
    notes("sphere{<0,0,0>,1 pigment{gradient x color_map{[0 color rgb 0][0.1 color rgb 1]"
          "[0.2 color rgb 0][0.3 color rgb 1][0.4 color rgb 0][0.5 color rgb 1][0.6 color rgb 0]"
          "[0.7 color rgb 1][0.8 color rgb 0]}}}",
          "color_map with 9 stops");
}

/// finish{brilliance}: read, kept through both round trips, and absent from a file that never
/// said it. Not folded like diffuse -- it is an exponent, and no algebra on the pigment reproduces
/// pow(N.L, b) -- so it earned Material a field.
void finishBrilliance() {
    std::printf("finish brilliance reaches the material and both files\n");
    try {
        const makina::PovImportResult r = makina::importPov(
            "sphere{<0,0,0>,1 pigment{color rgb <0.5,0.5,0.5>} "
            "finish{specular 0.18 roughness 0.09 brilliance 0.9}}");
        check(r.unsupported.empty(), "brilliance should read whole");
        check(r.scene.materials.count == 1, "one material expected");
        check(std::fabs(r.scene.materials[0].brilliance - 0.9f) < 1e-6f, "brilliance was lost");
        check(makina::toGpuMaterial(r.scene.materials[0]).brilliance == 0.9f,
              "brilliance should reach the shader material");

        makina::PovOptions opt;
        const std::string pov = makina::writePov(r.scene, opt);
        check(pov.find(" brilliance ") != std::string::npos, "the exporter should write it");
        const makina::PovImportResult back = makina::importPov(pov);
        check(std::fabs(back.scene.materials[0].brilliance - 0.9f) < 1e-6f,
              "brilliance should survive the POV round trip");

        const makina::Scene viaJson = makina::parseScene(makina::writeScene(r.scene));
        check(std::fabs(viaJson.materials[0].brilliance - 0.9f) < 1e-6f,
              "brilliance should survive the JSON round trip");
    } catch (const makina::PovParseError& e) {
        check(false, std::string("brilliance was refused: ") + e.what());
    }

    // Unset stays unset in every file: an older scene must not gain a line it never had, and the
    // shader must read the zero as POV's default of one.
    try {
        const makina::PovImportResult r = makina::importPov("sphere{<0,0,0>,1 pigment{color rgb 1}}");
        check(r.scene.materials[0].brilliance == 0.0f, "an unset brilliance should stay zero");
        check(makina::toGpuMaterial(r.scene.materials[0]).brilliance == 1.0f,
              "an unset brilliance should shade as POV's default one");
        makina::PovOptions opt;
        check(makina::writePov(r.scene, opt).find("brilliance") == std::string::npos,
              "the exporter should not write an unset brilliance");
        check(makina::writeScene(r.scene).find("brilliance") == std::string::npos,
              "the JSON should not carry an unset brilliance");
    } catch (const makina::PovParseError& e) {
        check(false, std::string("the plain sphere was refused: ") + e.what());
    }
}

/// #include: followed when the import knows a directory, named when it does not.
void includeFollows() {
    std::printf("#include splices the named file\n");
    {
        std::ofstream inc("povcheck_shapes.inc", std::ios::binary);
        inc << "#declare Ball = sphere{<0,0,0>,1}\n";
    }
    try {
        const makina::PovImportResult r =
            makina::importPov("#include \"povcheck_shapes.inc\"\nobject{Ball}", ".");
        check(r.unsupported.empty(), "the include should be followed, not noted");
        check(r.scene.nodes.count == 2, "the included Ball should be in the tree");
    } catch (const makina::PovParseError& e) {
        check(false, std::string("the include was refused: ") + e.what());
    }
    std::remove("povcheck_shapes.inc");

    // A missing file refuses by name; a string import (no directory) keeps the old note.
    try {
        makina::importPov("#include \"no_such_file.inc\"", ".");
        check(false, "a missing include should refuse");
    } catch (const makina::PovParseError& e) {
        check(std::string(e.what()).find("could not open") != std::string::npos,
              "the refusal should say the file could not be opened");
    }
    notes("#include \"whatever.inc\"\nsphere{<0,0,0>,1}", "#include");
}

/// rand/seed against POV's own measured stream.
///
/// The expected numbers are POV's #debug output for seed(42), read at 17 digits -- not values
/// this implementation produced. If the generator characterisation drifts from POV, this fails
/// against POV, not against itself.
void randStream() {
    std::printf("rand follows POV's measured stream\n");
    try {
        // Three draws land in a box's corner; the box then IS the stream, comparable exactly.
        const makina::PovImportResult r = makina::importPov(
            "#declare S = seed(42);\n"
            "box{<0,0,0>, <rand(S), rand(S), rand(S)>}");
        check(r.scene.nodes.count == 2, "the box should read");
        const makina::CsgNode& b = r.scene.nodes[1];
        const double expected[3] = {0.72358291124077112, 0.32997925959759838,
                                    0.75576167035749220};
        for (int i = 0; i < 3; ++i) {
            check(std::fabs(b.params[3 + i] - expected[i]) < 1e-7,
                  "draw " + std::to_string(i) + " should match POV's own stream for seed(42)");
        }
    } catch (const makina::PovParseError& e) {
        check(false, std::string("the rand box was refused: ") + e.what());
    }

    refuses("#declare N = 3; box{<0,0,0>, <rand(N), 1, 1>}", "not one");
    refuses("box{<0,0,0>, <rand(S), 1, 1>}", "not one");
}

/// `srgb` colors must come out decoded to linear, `rgb` must come out untouched.
///
/// The two spellings reach the material through different code paths (a flat pigment reads its
/// own vector; lights and pattern colors go through readMapColor), so each path is checked with
/// the same 0.5, whose linear value 0.2140 is far enough from 0.5 that a missing decode cannot
/// hide in tolerance.
void srgbDecode() {
    std::printf("srgb is display, rgb is linear\n");
    const float kLinearHalf = 0.214041144f;

    const makina::PovImportResult flat = makina::importPov(
        "sphere{<0,0,0>,1 pigment{color srgb <0.5,0.5,0.5>}}\n"
        "sphere{<3,0,0>,1 pigment{color rgb <0.5,0.5,0.5>}}");
    check(flat.scene.materials.count == 2, "two materials expected");
    check(std::fabs(flat.scene.materials[0].diffuse[0] - kLinearHalf) < 1e-5f,
          "a flat srgb pigment was read as linear");
    check(std::fabs(flat.scene.materials[1].diffuse[0] - 0.5f) < 1e-6f,
          "a flat rgb pigment was changed");

    const makina::PovImportResult pattern = makina::importPov(
        "sphere{<0,0,0>,1 pigment{checker color srgb <0.5,0.5,0.5> color rgb <0.5,0.5,0.5>}}");
    check(pattern.scene.pigments.count == 1, "one pigment expected");
    check(std::fabs(pattern.scene.pigments[0].stop[0][0] - kLinearHalf) < 1e-5f,
          "a checker srgb color was read as linear");
    check(std::fabs(pattern.scene.pigments[0].stop[1][0] - 0.5f) < 1e-6f,
          "a checker rgb color was changed");

    const makina::PovImportResult light = makina::importPov(
        "light_source{<0,5,0> color srgb <0.5,0.5,0.5>} sphere{<0,0,0>,1}");
    check(light.scene.lights.count == 1, "one light expected");
    check(std::fabs(light.scene.lights[0].color[0] - kLinearHalf) < 1e-5f,
          "a light's srgb color was read as linear");
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("makina-core POV-Ray reader\n\n");

    try {
        boundaries();
        macroExpansion();
        blobImport();
        sorImport();
        sweepImport();
        finishDiffuse();
        finishBrilliance();
        colorMapStops();
        randStream();
        includeFollows();
        srgbDecode();
    } catch (const std::exception& e) {
        std::printf("    FAIL  %s\n", e.what());
        ++failures;
    }
    std::printf("\n");

    for (int i = 1; i < argc; ++i) {
        try {
            roundTrip(argv[i]);
        } catch (const std::exception& e) {
            std::printf("    FAIL  %s\n", e.what());
            ++failures;
        }
    }

    if (failures == 0) {
        std::printf("\nwhat the exporter writes, the reader takes back (%d checks)\n", checks);
        return 0;
    }
    std::printf("\n%d of %d checks FAILED\n", failures, checks);
    return 1;
}
