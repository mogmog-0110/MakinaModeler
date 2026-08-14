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
    for (int i = 0; i < 3; ++i) { v.push_back(g.pattern.a[i]); }
    for (int i = 0; i < 3; ++i) { v.push_back(g.pattern.b[i]); }
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
    const makina::PovSurveyResult dirty = makina::povSurvey(pov + "\nsor { 2, <0,0>, <1,1> }");
    if (dirty.clean) {
        throw std::runtime_error("the survey missed a 'sor' appended to the file");
    }
}

void roundTrip(const std::string& path) {
    std::printf("%s\n", path.c_str());
    // The solid, not the tree that was authored. A .pov has no way to say "this node is muted",
    // so the only thing a round trip can preserve is the shape the mute leaves behind -- and
    // handing the exporter the authored tree would write out a node the modeller does not draw.
    const makina::Scene original = makina::withoutMuted(makina::parseScene(readFile(path)));

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
    refuses("#declare S = sor { 3, <0,0>,<1,1>,<0,2> }", "revolved about an axis");
    refuses("isosurface { function { x*x } }", "given by a function");
    refuses("sphere{<0,0,0>,rand(R)}", "is a function call");
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
    check(std::fabs(pattern.scene.pigments[0].a[0] - kLinearHalf) < 1e-5f,
          "a checker srgb color was read as linear");
    check(std::fabs(pattern.scene.pigments[0].b[0] - 0.5f) < 1e-6f,
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
