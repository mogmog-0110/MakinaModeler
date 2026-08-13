// Reading a POV-Ray file back into a Makina scene.
//
// The subset, and why each boundary sits where it does:
//
//   geometry     sphere, box, cylinder, cone, torus, plane -- every one a primitive the distance
//                field already has, so nothing is approximated.
//   booleans     union, merge, difference, intersection. The same four this model has.
//   transforms   translate, rotate, scale.
//   appearance   pigment{color}, finish{ambient specular roughness}: the terms RenderMaterial.hpp
//                already means exactly what POV means by.
//   language     #declare of a number, a vector, a texture or an object, and object{Name} to use
//                one. Those cover 110 of the 117 language constructs in the two sample files.
//
// Everything else is refused or reported by name. A reader that skips what it does not understand
// hands back a scene that loaded, rendered, and is not the one in the file -- and unlike a stale
// baked shader there is no hash to catch it later.
//
// **The scene is built as an ordinary tree first and flattened at the end.** That is the shape of
// this file and it was learned the hard way: Scene requires a node's children to be contiguous in
// the array, while POV reads a shape *before* the modifiers that wrap it, so the wrapping node is
// created after the node it wraps. Building into Scene directly survives one torus and breaks on
// the first instanced object. Edit.hpp solves the same problem the same way, by rebuilding rather
// than patching.
//
// Two POV facts that are easy to get backwards, and are exactly what a silent reader would ruin:
//
//   handedness   POV is left-handed and this model is right-handed (COORDINATES.md), and yet
//                nothing is flipped here. The exporter compensates in the camera, with
//                `right<-aspect,0,0>`, which means the coordinates in a .pov file this project
//                wrote are already this model's coordinates. Flipping z on the way back in was
//                the first attempt and it mirrors every scene: exact round trips became wrong by
//                twice the offset. A hand-written file authored in POV's own handedness is
//                indistinguishable from ours by its contents, so agreeing with our own exporter
//                is the only choice that can be checked.
//   rotate       POV's `rotate <a,b,c>` is degrees about x, then y, then z, in that order. One
//                node here turns about one axis, so that is three nodes, innermost first.

#pragma once

// Edit.hpp for setNameText: the name table is written the same way every other edit writes it.
#include "Edit.hpp"
#include "Op.hpp"
#include "PovLex.hpp"
#include "Scene.hpp"

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace makina {

/// What a read produced, and what it had to leave out.
struct PovImportResult {
    Scene scene;
    /// Constructs recognised but not represented, each named once. Empty means a complete read.
    std::vector<std::string> unsupported;
};

namespace detail {

/// A node of the tree the parser builds, before anything is flattened.
///
/// Children by value: a POV file is a few hundred nodes and the copying is nothing next to being
/// able to wrap a subtree by moving it, which is what the parser does constantly.
struct PovNode {
    Op            op = Op::Unsupported;
    float         params[12]{};
    std::uint16_t flags = 0;
    int           material = -1;
    std::string   name;
    std::vector<PovNode> children;
};

/// One entry of a `transform { ... }`: what to do and by how much.
struct PovMove {
    std::string kind;   ///< translate | rotate | scale
    double      v[3]{};
};

/// One parse in progress: the token stream, the symbol tables, and the tree being built.
class PovReader {
public:
    explicit PovReader(const std::string& src) : m_tok(povTokenize(src)) {}

    PovImportResult read() {
        PovNode root;
        root.op = Op::SceneRoot;
        root.name = "Scene";

        while (peek().kind != PovTokenKind::End) {
            readTopLevel(root);
        }

        PovImportResult r;
        r.scene = flatten(root);
        r.unsupported = m_unsupported;
        return r;
    }

private:
    // ------------------------------------------------------------------ tokens

    const PovToken& peek(std::size_t ahead = 0) const {
        const std::size_t at = m_at + ahead;
        return at < m_tok.size() ? m_tok[at] : m_tok.back();
    }

    const PovToken& take() {
        const PovToken& t = m_tok[m_at];
        if (m_at + 1 < m_tok.size()) {
            ++m_at;
        }
        return t;
    }

    bool isWord(const char* w, std::size_t ahead = 0) const {
        return peek(ahead).kind == PovTokenKind::Word && peek(ahead).text == w;
    }

    bool isPunct(char c, std::size_t ahead = 0) const {
        return peek(ahead).kind == PovTokenKind::Punct && peek(ahead).text[0] == c;
    }

    void expectPunct(char c) {
        if (!isPunct(c)) {
            throw PovParseError(peek().line, std::string("expected '") + c + "' but found '" +
                                                 peek().text + "'");
        }
        take();
    }

    [[noreturn]] void refuse(const std::string& what) const {
        throw PovParseError(peek().line, what);
    }

    /// Records something recognised but not represented. Named once, however often it appears.
    void note(const std::string& what) {
        for (const std::string& s : m_unsupported) {
            if (s == what) {
                return;
            }
        }
        m_unsupported.push_back(what);
    }

    // ------------------------------------------------------------- expressions

    double number() {
        double v = term();
        while (isPunct('+') || isPunct('-')) {
            const char op = take().text[0];
            const double rhs = term();
            v = op == '+' ? v + rhs : v - rhs;
        }
        return v;
    }

    double term() {
        double v = factor();
        while (isPunct('*') || isPunct('/')) {
            const char op = take().text[0];
            const double rhs = factor();
            if (op == '/' && rhs == 0.0) {
                refuse("division by zero in an expression");
            }
            v = op == '*' ? v * rhs : v / rhs;
        }
        return v;
    }

    double factor() {
        if (isPunct('-')) { take(); return -factor(); }
        if (isPunct('+')) { take(); return factor(); }
        if (isPunct('(')) {
            take();
            const double v = number();
            expectPunct(')');
            return v;
        }
        if (peek().kind == PovTokenKind::Number) {
            return take().number;
        }
        if (peek().kind == PovTokenKind::Word) {
            const std::string name = peek().text;
            const auto it = m_numbers.find(name);
            if (it != m_numbers.end()) {
                take();
                return it->second;
            }
            if (m_vectors.count(name) > 0) {
                refuse("'" + name + "' is a vector and a number is needed here");
            }
            if (isPunct('(', 1)) {
                // POV has a function library, and some of it -- rand and seed above all -- can
                // only be matched by running POV's own generator. Naming it as a call rather than
                // as an unknown value is the difference between "this file uses something out of
                // scope" and "this reader is broken".
                refuse("'" + name + "' is a function call, and this reader evaluates none: POV's "
                       "functions would each have to produce the same values POV does");
            }
            refuse("'" + name + "' is not a value this reader knows");
        }
        refuse("expected a number but found '" + peek().text + "'");
    }

    /// A vector expression: `<x,y,z>`, an axis name, a declared vector, a number, or arithmetic
    /// over them.
    ///
    /// Everything is held as three components, a scalar included -- POV broadcasts one, which is
    /// what `scale 2` relies on, and it multiplies two vectors component by component. Carrying a
    /// separate scalar type would mean writing every operator twice for no gain.
    ///
    /// This exists because hand-written files say `rotate x*20` and `plane{-y, 0}`, both of which
    /// are arithmetic on a vector rather than a literal.
    void vector3(double out[3]) {
        vecTerm(out);
        while (isPunct('+') || isPunct('-')) {
            const char op = take().text[0];
            double rhs[3];
            vecTerm(rhs);
            for (int i = 0; i < 3; ++i) {
                out[i] = op == '+' ? out[i] + rhs[i] : out[i] - rhs[i];
            }
        }
    }

    void vecTerm(double out[3]) {
        vecFactor(out);
        while (isPunct('*') || isPunct('/')) {
            const char op = take().text[0];
            double rhs[3];
            vecFactor(rhs);
            for (int i = 0; i < 3; ++i) {
                if (op == '/' && rhs[i] == 0.0) {
                    refuse("division by zero in a vector expression");
                }
                out[i] = op == '*' ? out[i] * rhs[i] : out[i] / rhs[i];
            }
        }
    }

    void vecFactor(double out[3]) {
        if (isPunct('-')) {
            take();
            vecFactor(out);
            for (int i = 0; i < 3; ++i) {
                out[i] = -out[i];
            }
            return;
        }
        if (isPunct('+')) {
            take();
            vecFactor(out);
            return;
        }
        if (isPunct('(')) {
            take();
            vector3(out);
            expectPunct(')');
            return;
        }
        if (isPunct('<')) {
            take();
            out[0] = number();
            expectPunct(',');
            out[1] = number();
            expectPunct(',');
            out[2] = number();
            // A fourth component is a filter or a transmit; read and dropped, since a caller that
            // wants it asks for a material instead.
            while (isPunct(',')) {
                take();
                (void)number();
            }
            expectPunct('>');
            return;
        }
        if (peek().kind == PovTokenKind::Word) {
            // POV names the three axes, and the exporter in this very repository uses them:
            // `plane{y, 0}`. A reader that did not know them would refuse a file it wrote itself.
            const std::string& w = peek().text;
            if (w == "x" || w == "y" || w == "z") {
                take();
                out[0] = w == "x" ? 1.0 : 0.0;
                out[1] = w == "y" ? 1.0 : 0.0;
                out[2] = w == "z" ? 1.0 : 0.0;
                return;
            }
            const auto it = m_vectors.find(w);
            if (it != m_vectors.end()) {
                take();
                for (int i = 0; i < 3; ++i) {
                    out[i] = it->second[i];
                }
                return;
            }
        }
        const double v = number();
        out[0] = out[1] = out[2] = v;
    }

    // ------------------------------------------------------------------ blocks

    /// Skips a balanced brace group, starting from wherever the block begins.
    void skipBlock() {
        while (!isPunct('{') && peek().kind != PovTokenKind::End) {
            take();
        }
        if (peek().kind == PovTokenKind::End) {
            return;
        }
        int depth = 0;
        do {
            if (isPunct('{')) { ++depth; }
            else if (isPunct('}')) { --depth; }
            take();
        } while (depth > 0 && peek().kind != PovTokenKind::End);
    }

    /// Steps over one value of unknown shape, so an unsupported keyword does not derail its block.
    ///
    /// Only over things that can actually be a value. Many POV keywords carry none -- `no_shadow`,
    /// `hollow`, `open`, `cutaway_textures` -- and taking the next token unconditionally swallows
    /// the closing brace, after which the reader runs to the end of the file and reports the block
    /// as unclosed. That reads as a malformed scene, several hundred lines from the real cause.
    void skipValue() {
        if (isPunct('{')) { skipBlock(); return; }
        if (isPunct('<')) { double v[3]; vector3(v); return; }
        if (peek().kind == PovTokenKind::Number) { (void)number(); return; }
    }

    // ---------------------------------------------------------------- top level

    void readTopLevel(PovNode& root) {
        const PovToken t = peek();

        if (t.kind == PovTokenKind::Directive) {
            if (t.text == "#version") {
                take();
                while (!isPunct(';') && peek().kind != PovTokenKind::End) { take(); }
                if (isPunct(';')) { take(); }
                return;
            }
            if (t.text == "#declare" || t.text == "#local") {
                take();
                readDeclare();
                return;
            }
            if (t.text == "#include") {
                take();
                if (peek().kind == PovTokenKind::String) {
                    // Named rather than followed. Reading it would need a search path, and one bad
                    // path would look like a scene that simply has fewer objects in it.
                    note("#include \"" + take().text + "\"");
                    return;
                }
                refuse("#include needs a file name in quotes");
            }
            note(t.text);
            take();
            skipBlock();
            return;
        }

        if (t.kind == PovTokenKind::Word) {
            if (isObjectStart()) {
                root.children.push_back(readObject());
                return;
            }
            refuseUnsupportedShape();
            const std::string& w = t.text;
            if (w == "global_settings" || w == "camera" || w == "light_source" ||
                w == "background" || w == "sky_sphere" || w == "fog") {
                // The frame, not the model. This reader produces a scene; the renderer brings its
                // own camera and the scene format carries its own lights.
                take();
                note(w);
                skipBlock();
                return;
            }
            refuse("'" + w + "' is not something this reader knows at the top level");
        }

        refuse("unexpected '" + t.text + "'");
    }

    /// POV shapes this model has no form for, named so the refusal says something useful.
    ///
    /// Without this the word falls through to the expression parser and comes back as "'sor' is
    /// not a value this reader knows", which is true and useless -- it describes where the parser
    /// happened to be rather than what the file asked for. Each of these is a real shape POV can
    /// trace and this model cannot hold: splines revolved or extruded, implicit surfaces, meshes.
    /// Approximating one with stacked cones would be the silent difference this reader exists to
    /// refuse.
    static const char* unsupportedShape(const std::string& w) {
        struct Entry { const char* name; const char* why; };
        static const Entry kShapes[] = {
            {"sor",            "a spline revolved about an axis"},
            {"lathe",          "a spline revolved about an axis"},
            {"prism",          "a spline swept along an axis"},
            {"blob",           "a field of blended spheres"},
            {"sphere_sweep",   "a sphere dragged along a spline"},
            {"superellipsoid", "an implicit surface with two exponents"},
            {"isosurface",     "an implicit surface given by a function"},
            {"parametric",     "a surface given by two parameters"},
            {"height_field",   "a surface read from an image"},
            {"julia_fractal",  "a fractal"},
            {"mesh",           "a triangle mesh"},
            {"mesh2",          "a triangle mesh"},
            {"polygon",        "a flat outline with any number of sides"},
            {"text",           "glyphs from a font"},
            {"bicubic_patch",  "a bicubic patch"},
        };
        for (const Entry& e : kShapes) {
            if (w == e.name) {
                return e.why;
            }
        }
        return nullptr;
    }

    /// Stops on a shape this model cannot hold, saying what it is.
    void refuseUnsupportedShape() {
        const std::string w = peek().text;
        const char* why = unsupportedShape(w);
        if (why != nullptr) {
            refuse("'" + w + "' is " + std::string(why) +
                   ", which this model has no form for; approximating it would give a scene that "
                   "renders and is not the one in the file");
        }
    }

    bool isObjectStart() const {
        static const char* kStarts[] = {"sphere",     "box",          "cylinder", "cone",
                                        "torus",      "plane",        "disc",     "triangle",
                                        "union",      "merge",        "difference",
                                        "intersection", "object"};
        for (const char* s : kStarts) {
            if (isWord(s)) {
                return true;
            }
        }
        return false;
    }

    void readDeclare() {
        if (peek().kind != PovTokenKind::Word) {
            refuse("#declare needs a name");
        }
        const std::string name = take().text;
        expectPunct('=');
        refuseUnsupportedShape();

        if (isWord("texture") || isWord("pigment") || isWord("finish") || isWord("normal")) {
            m_textures[name] = readAppearance();
            return;
        }
        if (isWord("transform")) {
            m_transforms[name] = readTransformBlock();
            return;
        }
        if (isObjectStart()) {
            m_objects[name] = readObject();
            return;
        }
        if (isPunct('<')) {
            double v[3];
            vector3(v);
            m_vectors[name] = {v[0], v[1], v[2]};
            if (isPunct(';')) { take(); }
            return;
        }
        m_numbers[name] = number();
        if (isPunct(';')) { take(); }
    }

    // ---------------------------------------------------------------- appearance

    Material readAppearance() {
        Material m{};
        m.diffuse[0] = m.diffuse[1] = m.diffuse[2] = 1.0f;
        m.alpha = 1.0f;
        m.ambient = 0.1f;
        m.specular = 0.0f;
        m.shininess = 0.0f;
        m.emission = 0.0f;
        m.textureId = -1;
        m.reflection = 0.0f;
        readAppearanceInto(m);
        return m;
    }

    void readAppearanceInto(Material& m) {
        const std::string kind = take().text;   // texture | pigment | finish | normal

        if (kind == "normal") {
            // A bump pattern perturbs the shading normal, which this model has nowhere to put.
            // Reported rather than dropped: it is 13 uses across the sample files, so a reader
            // that swallowed it would leave the difference describable only as "somehow smoother".
            note("normal");
            skipBlock();
            return;
        }

        // `texture { Name }` reuses a declared one outright.
        if (isPunct('{') && peek(1).kind == PovTokenKind::Word &&
            m_textures.count(peek(1).text) > 0 && isPunct('}', 2)) {
            take();
            m = m_textures[take().text];
            take();
            return;
        }
        expectPunct('{');

        while (!isPunct('}')) {
            if (peek().kind == PovTokenKind::End) {
                refuse("a " + kind + " block was not closed");
            }
            if (isWord("texture") || isWord("pigment") || isWord("finish") || isWord("normal")) {
                readAppearanceInto(m);
                continue;
            }
            if (peek().kind != PovTokenKind::Word) {
                skipValue();
                continue;
            }

            const std::string w = take().text;
            if (w == "color" || w == "rgb" || w == "srgb" || w == "rgbf" || w == "rgbt") {
                // POV also accepts a British spelling of the first of these. It is not accepted
                // here, deliberately: it falls through to note(), so a file using it is reported
                // as unread rather than quietly losing its pigment.
                if (peek().kind == PovTokenKind::Word) {
                    continue;   // `color rgb <...>`: the keyword pair, take the second lap
                }
                double c[3];
                vector3(c);
                for (int i = 0; i < 3; ++i) {
                    m.diffuse[i] = static_cast<float>(c[i]);
                }
                continue;
            }
            if (w == "ambient")  { m.ambient  = static_cast<float>(number()); continue; }
            if (w == "specular") { m.specular = static_cast<float>(number()); continue; }
            if (w == "emission") { m.emission = static_cast<float>(number()); continue; }
            if (w == "reflection") {
                // POV also writes this as a block with its own colors and falloff. A plain number
                // is the common form and the one this model holds; the block is reported instead
                // of having its first number taken as though it were the whole thing.
                if (isPunct('{')) {
                    note("finish reflection block");
                    skipBlock();
                    continue;
                }
                m.reflection = static_cast<float>(number());
                continue;
            }
            if (w == "roughness") {
                // The inverse of the conversion RenderMaterial.hpp does on the way out.
                m.shininess = static_cast<float>((1.0 - number()) * 128.0);
                continue;
            }
            if (w == "diffuse") {
                // Material has no diffuse coefficient; RenderMaterial supplies POV's default of
                // 0.6. A file asking for another value is asking for a look this cannot hold.
                if (std::fabs(number() - 0.6) > 1e-6) {
                    note("finish diffuse");
                }
                continue;
            }
            if (m_textures.count(w) > 0) {
                m = m_textures[w];
                continue;
            }
            note(kind + " " + w);
            skipValue();
        }
        take();
    }

    // ------------------------------------------------------------------- shapes

    /// Reads one object with its modifiers and returns it as a subtree.
    PovNode readObject() {
        if (isWord("sphere"))   { return modifiers(sphere()); }
        if (isWord("box"))      { return modifiers(box()); }
        if (isWord("cylinder")) { return modifiers(cylinder()); }
        if (isWord("cone"))     { return modifiers(cone()); }
        if (isWord("torus"))    { return modifiers(torus()); }
        if (isWord("plane"))    { return modifiers(plane()); }
        if (isWord("disc"))     { return modifiers(disc()); }
        if (isWord("triangle")) { return modifiers(triangle()); }
        if (isWord("union") || isWord("merge") || isWord("difference") || isWord("intersection")) {
            return boolean();
        }
        if (isWord("object")) { return instance(); }
        refuse("'" + peek().text + "' does not begin an object");
    }

    static PovNode leaf(Op op, const char* name) {
        PovNode n;
        n.op = op;
        n.name = name;
        return n;
    }

    /// Puts a new node above a subtree. This is the operation building into the flat scene made
    /// impossible, and having it is the whole reason for the intermediate tree.
    static PovNode wrap(PovNode inner, Op op) {
        PovNode n = leaf(op, opName(op));
        n.children.push_back(std::move(inner));
        return n;
    }

    /// Places a shape at a point, if it is not already at the origin.
    static PovNode place(PovNode inner, double x, double y, double z) {
        if (x == 0.0 && y == 0.0 && z == 0.0) {
            return inner;
        }
        PovNode t = wrap(std::move(inner), Op::Translate);
        t.params[0] = static_cast<float>(x);
        t.params[1] = static_cast<float>(y);
        t.params[2] = static_cast<float>(z);
        return t;
    }

    /// Turns a subtree built along +Y so that +Y points along `axis`.
    ///
    /// Two rotations, Z then Y, which is enough because the object is a solid of revolution about
    /// the axis -- a third would only spin it about itself. The angles come from the rotation
    /// matrices in Bounds.hpp rather than from a convention assumed here: a turn of t about Z
    /// sends (0,1,0) to (-sin t, cos t, 0), and a turn of p about Y then sends that to
    /// (-sin t cos p, cos t, sin t sin p). Matching that against the wanted axis gives both.
    static PovNode alignY(PovNode inner, const double axis[3], double len) {
        const double u[3] = {axis[0] / len, axis[1] / len, axis[2] / len};
        const double clamped = u[1] > 1.0 ? 1.0 : (u[1] < -1.0 ? -1.0 : u[1]);
        const double tilt = std::acos(clamped) * 180.0 / 3.14159265358979323846;
        if (std::fabs(tilt) < 1e-9) {
            return inner;   // already along +Y
        }
        // Straight down is the one case the second angle cannot be recovered from, because the
        // axis lies on the pole and every spin about Y sends +Y to the same place. Zero is as
        // good as any other value there, and picking one keeps atan2(0,0) out of the result.
        const double sinTilt = std::sqrt(1.0 - clamped * clamped);
        const double spin = sinTilt < 1e-9
                                ? 0.0
                                : std::atan2(u[2], -u[0]) * 180.0 / 3.14159265358979323846;

        PovNode z = wrap(std::move(inner), Op::Rotate);
        z.params[0] = static_cast<float>(tilt);
        z.flags |= flags::kAxisZ;
        if (std::fabs(spin) < 1e-9) {
            return z;
        }
        PovNode y = wrap(std::move(z), Op::Rotate);
        y.params[0] = static_cast<float>(spin);
        y.flags |= flags::kAxisY;
        return y;
    }

    PovNode sphere() {
        take();
        expectPunct('{');
        double c[3];
        vector3(c);
        expectPunct(',');
        const double r = number();
        if (r <= 0.0) {
            refuse("a sphere needs a positive radius");
        }
        PovNode n = leaf(Op::Sphere, "Sphere");
        n.params[0] = static_cast<float>(r);
        return place(std::move(n), c[0], c[1], c[2]);
    }

    PovNode box() {
        take();
        expectPunct('{');
        double a[3], b[3];
        vector3(a);
        expectPunct(',');
        vector3(b);

        PovNode n = leaf(Op::Box, "Box");
        // Either corner may be the smaller; POV does not require an order and neither does this.
        n.params[0] = static_cast<float>(a[0] < b[0] ? a[0] : b[0]);
        n.params[1] = static_cast<float>(a[1] < b[1] ? a[1] : b[1]);
        n.params[2] = static_cast<float>(a[2] < b[2] ? a[2] : b[2]);
        n.params[3] = static_cast<float>(a[0] > b[0] ? a[0] : b[0]);
        n.params[4] = static_cast<float>(a[1] > b[1] ? a[1] : b[1]);
        n.params[5] = static_cast<float>(a[2] > b[2] ? a[2] : b[2]);
        return n;
    }

    PovNode cylinder() {
        take();
        expectPunct('{');
        double a[3], b[3];
        vector3(a);
        expectPunct(',');
        vector3(b);
        expectPunct(',');
        const double r = number();

        if (r <= 0.0) {
            refuse("a cylinder needs a positive radius");
        }
        // A cylinder reads the same from either end, so the one that gives an upward axis is
        // chosen. That is not tidiness: the exporter writes cap before base, so every upright
        // cylinder would otherwise come back tilted a full half turn -- geometrically right,
        // two extra nodes, and a round trip exact only to the last few bits of a float.
        const double* lo = a;
        const double* hi = b;
        if (b[1] < a[1]) {
            lo = b;
            hi = a;
        }
        const double d[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
        const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len < 1e-12) {
            refuse("a cylinder needs two distinct end points");
        }

        // Built along +Y from the origin, then turned onto the axis the file gives and moved to
        // its first end point. The axis-aligned case falls out of the same code with both angles
        // zero, so there is one path rather than two that have to agree.
        PovNode n = leaf(Op::Cylinder, "Cylinder");
        n.params[2] = static_cast<float>(r);

        // An upright cylinder keeps the cap-and-base form this model stores, which is what the
        // exporter wrote in the first place. Sending it through the general path instead would be
        // a cylinder from zero to its length under a translate: the same solid, one node more,
        // and a round trip exact only to the last bits of a float rather than to every bit.
        if (std::fabs(d[0]) < 1e-9 && std::fabs(d[2]) < 1e-9) {
            n.params[0] = static_cast<float>(hi[1]);
            n.params[1] = static_cast<float>(lo[1]);
            return place(std::move(n), lo[0], 0.0, lo[2]);
        }
        n.params[0] = static_cast<float>(len);
        n.params[1] = 0.0f;
        return place(alignY(std::move(n), d, len), lo[0], lo[1], lo[2]);
    }

    PovNode cone() {
        take();
        expectPunct('{');
        double a[3], b[3];
        vector3(a);
        expectPunct(',');
        const double ra = number();
        expectPunct(',');
        vector3(b);
        expectPunct(',');
        const double rb = number();

        if (ra > 1e-9 && rb > 1e-9) {
            // A truncated cone has two radii; this model's Cone has one and a tip.
            refuse("this reader takes a cone that comes to a point; this one is truncated");
        }
        const bool baseFirst = ra > rb;
        const double radius = baseFirst ? ra : rb;
        const double* base = baseFirst ? a : b;
        const double* tip  = baseFirst ? b : a;
        const double d[3] = {tip[0] - base[0], tip[1] - base[1], tip[2] - base[2]};
        const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len < 1e-12) {
            refuse("a cone needs two distinct end points");
        }

        PovNode n = leaf(Op::Cone, "Cone");
        n.params[0] = static_cast<float>(radius);
        // The height stays signed even here, where the tilt could carry the direction instead:
        // an axis-aligned cone that points down has to come back as the negative height this
        // model uses, or the round trip against the exporter stops being exact.
        const bool alignedDown = std::fabs(d[0]) < 1e-9 && std::fabs(d[2]) < 1e-9 && d[1] < 0.0;
        n.params[1] = static_cast<float>(alignedDown ? -len : len);
        PovNode out = alignedDown ? std::move(n) : alignY(std::move(n), d, len);
        return place(std::move(out), base[0], base[1], base[2]);
    }

    PovNode torus() {
        take();
        expectPunct('{');
        const double major = number();
        expectPunct(',');
        const double minor = number();
        PovNode n = leaf(Op::Torus, "Torus");
        n.params[0] = static_cast<float>(major);
        n.params[1] = static_cast<float>(minor);
        return n;
    }

    PovNode plane() {
        take();
        expectPunct('{');
        double normal[3];
        vector3(normal);
        expectPunct(',');
        const double dist = number();
        const double len = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] +
                                     normal[2] * normal[2]);
        if (len < 1e-12) {
            refuse("a plane needs a normal with a direction");
        }

        // This model's Plane keeps the half below y = 0, so its outward normal is +Y -- the same
        // sense POV gives a plane. Turning +Y onto the normal in the file therefore reproduces
        // the half-space rather than its complement, and `plane{-y,0}` comes out as the upper
        // half without needing a mirror.
        const double u[3] = {normal[0] / len, normal[1] / len, normal[2] / len};
        PovNode n = leaf(Op::Plane, "Plane");
        return place(alignY(std::move(n), u, 1.0), u[0] * dist, u[1] * dist, u[2] * dist);
    }

    /// POV's disc: a centre, a normal, a radius, and optionally a hole.
    ///
    /// Only the +Y normal, for the reason the plane reader gives: this model's Disc lies in the
    /// xz plane and a tilted one would have to be turned, which is geometry the file did not ask
    /// to move. Written by the exporter as `disc{<0,0,0>,<0,1,0>,r,hole}` -- always four
    /// arguments, so the hole is read rather than guessed at.
    PovNode disc() {
        take();
        expectPunct('{');
        double c[3], normal[3];
        vector3(c);
        expectPunct(',');
        vector3(normal);
        expectPunct(',');
        const double radius = number();
        double hole = 0.0;
        if (isPunct(',')) {
            take();
            hole = number();
        }
        if (radius <= 0.0) {
            refuse("a disc needs a positive radius");
        }
        const double len = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] +
                                     normal[2] * normal[2]);
        if (len < 1e-12) {
            refuse("a disc needs a normal with a direction");
        }
        PovNode n = leaf(Op::Disc, "Disc");
        n.params[0] = static_cast<float>(radius);
        n.params[1] = static_cast<float>(hole);
        // Turned onto its normal by the same helper the cylinder uses. A disc is a solid of
        // revolution about that normal, so two rotations place it and a third would only spin it
        // in its own plane.
        const double u[3] = {normal[0] / len, normal[1] / len, normal[2] / len};
        return place(alignY(std::move(n), u, 1.0), c[0], c[1], c[2]);
    }

    /// Three points, verbatim. A triangle has no orientation to reconcile.
    PovNode triangle() {
        take();
        expectPunct('{');
        PovNode n = leaf(Op::Triangle, "Triangle");
        for (int i = 0; i < 3; ++i) {
            if (i > 0) {
                expectPunct(',');
            }
            double v[3];
            vector3(v);
            for (int k = 0; k < 3; ++k) {
                n.params[i * 3 + k] = static_cast<float>(v[k]);
            }
        }
        return n;
    }

    PovNode boolean() {
        const std::string kind = take().text;
        const Op op = kind == "difference"     ? Op::Difference
                      : kind == "intersection" ? Op::Intersection
                                               : Op::Merge;
        expectPunct('{');
        PovNode n = leaf(op, opName(op));
        while (!isPunct('}')) {
            if (peek().kind == PovTokenKind::End) {
                refuse("a " + kind + " block was not closed");
            }
            refuseUnsupportedShape();
            if (!isObjectStart()) {
                break;
            }
            n.children.push_back(readObject());
        }
        return modifiers(std::move(n));
    }

    /// `object { ... }`, which POV uses two ways.
    ///
    /// With a name it is an instance of something declared earlier; with a shape inside it is
    /// just a wrapper, and hand-written files use it that way to hang modifiers on a boolean.
    /// Reading only the first form refuses files for a reason that has nothing to do with what
    /// they contain.
    PovNode instance() {
        take();
        expectPunct('{');
        if (isObjectStart()) {
            return modifiers(readObject());
        }
        if (peek().kind != PovTokenKind::Word) {
            refuse("object{ needs a shape or the name of a declared object");
        }
        const std::string name = take().text;
        const auto it = m_objects.find(name);
        if (it == m_objects.end()) {
            refuse("'" + name + "' is not a declared object");
        }
        // A copy, so one declared shape can appear many times under different transforms.
        return modifiers(PovNode(it->second));
    }

    /// The modifiers after a shape's own arguments, up to the closing brace.
    PovNode modifiers(PovNode node) {
        while (!isPunct('}')) {
            if (peek().kind == PovTokenKind::End) {
                refuse("an object block was not closed");
            }

            if (isWord("texture") || isWord("pigment") || isWord("finish") || isWord("normal")) {
                const Material m = readAppearance();
                node.material = materialIndex(m);
                continue;
            }
            if (isWord("translate") || isWord("scale") || isWord("rotate")) {
                node = transform(std::move(node));
                continue;
            }
            if (isWord("transform")) {
                for (const PovMove& m : readTransformBlock()) {
                    node = applyMove(std::move(node), m.kind, m.v);
                }
                continue;
            }
            if (peek().kind == PovTokenKind::Word && m_textures.count(peek().text) > 0) {
                node.material = materialIndex(m_textures[take().text]);
                continue;
            }
            if (peek().kind == PovTokenKind::Word) {
                // open, hollow, no_shadow, interior, bounded_by, photons: each changes what the
                // object is or how it is drawn, so each is reported rather than dropped.
                note("modifier " + take().text);
                skipValue();
                continue;
            }
            skipValue();
        }
        take();
        return node;
    }

    /// One `transform { ... }`, as the list of moves it stands for.
    ///
    /// Kept as a list rather than a matrix. This model has no matrix node -- a placement is a
    /// stack of single-axis Rotates and one Translate and one Scale -- so a matrix would have to
    /// be taken apart again, and a decomposition that is nearly right is worse than a list that
    /// is exactly right.
    std::vector<PovMove> readTransformBlock() {
        take();   // transform
        expectPunct('{');
        std::vector<PovMove> out;
        while (!isPunct('}')) {
            if (peek().kind == PovTokenKind::End) {
                refuse("a transform block was not closed");
            }
            if (isWord("translate") || isWord("scale") || isWord("rotate")) {
                PovMove m;
                m.kind = take().text;
                vector3(m.v);
                out.push_back(m);
                continue;
            }
            if (peek().kind == PovTokenKind::Word) {
                const auto it = m_transforms.find(peek().text);
                if (it != m_transforms.end()) {
                    take();
                    // A named transform inside another one composes, which is how POV builds a
                    // placement out of parts.
                    out.insert(out.end(), it->second.begin(), it->second.end());
                    continue;
                }
                refuse("'" + peek().text + "' is not a declared transform");
            }
            refuse("a transform block takes translate, rotate, scale, or a declared transform");
        }
        take();
        return out;
    }

    PovNode applyMove(PovNode node, const std::string& kind, const double v[3]) {
        if (kind == "translate") {
            PovNode t = wrap(std::move(node), Op::Translate);
            t.params[0] = static_cast<float>(v[0]);
            t.params[1] = static_cast<float>(v[1]);
            t.params[2] = static_cast<float>(v[2]);
            return t;
        }

        if (kind == "scale") {
            for (int i = 0; i < 3; ++i) {
                if (v[i] == 0.0) {
                    refuse("a scale of zero collapses the object and cannot be undone");
                }
            }
            PovNode t = wrap(std::move(node), Op::Scale);
            for (int i = 0; i < 3; ++i) {
                t.params[i] = static_cast<float>(v[i]);
            }
            return t;
        }

        // x, then y, then z, each its own node, innermost first -- POV applies them in that
        // order and the last one written has to end up outermost.
        const double angles[3] = {v[0], v[1], v[2]};
        const std::uint16_t axes[3] = {flags::kAxisX, flags::kAxisY, flags::kAxisZ};
        PovNode out = std::move(node);
        for (int i = 0; i < 3; ++i) {
            if (angles[i] == 0.0) {
                continue;
            }
            out = wrap(std::move(out), Op::Rotate);
            out.params[0] = static_cast<float>(angles[i]);
            out.flags |= axes[i];
        }
        return out;
    }

    /// The modifier form: one move written straight onto an object.
    PovNode transform(PovNode node) {
        const std::string kind = take().text;
        double v[3];
        vector3(v);
        return applyMove(std::move(node), kind, v);
    }

    int materialIndex(const Material& m) {
        // Deduplicated: a texture declared once and used fifty times is one material, which is
        // what the file means and what keeps the table inside its limit.
        for (std::size_t i = 0; i < m_materials.size(); ++i) {
            if (std::memcmp(&m_materials[i], &m, sizeof(Material)) == 0) {
                return static_cast<int>(i);
            }
        }
        if (m_materials.size() >= Scene::kMaxMaterials) {
            refuse("the scene needs more than this model's " +
                   std::to_string(Scene::kMaxMaterials) + " materials");
        }
        m_materials.push_back(m);
        return static_cast<int>(m_materials.size() - 1);
    }

    // ----------------------------------------------------------------- flatten

    /// Writes the tree into a Scene, each node's children contiguous.
    ///
    /// The same walk Edit.hpp's rebuild does, and for the same reason: the layout is a product of
    /// the whole tree, so it can only be produced once the whole tree exists.
    Scene flatten(const PovNode& root) {
        Scene s{};
        s.nextId = 1;
        s.nodes.count = 1;
        s.names.count = 1;
        s.nodes[0].parent = kNoParent;
        for (std::size_t i = 0; i < m_materials.size(); ++i) {
            s.materials[s.materials.count++] = m_materials[i];
        }
        emit(s, root, 0);
        return s;
    }

    void emit(Scene& s, const PovNode& n, std::uint16_t at) {
        const std::uint16_t keepParent = s.nodes[at].parent;
        CsgNode& d = s.nodes[at];
        d = CsgNode{};
        d.op = static_cast<std::uint8_t>(n.op);
        d.id = s.nextId++;
        d.parent = keepParent;
        d.flags = n.flags;
        d.materialId = n.material < 0 ? kNoMaterial : static_cast<std::uint8_t>(n.material);
        d.firstChild = kNoChild;
        d.childCount = 0;
        for (int i = 0; i < 12; ++i) {
            d.params[i] = n.params[i];
        }
        setNameText(s, at, n.name);
        s.nodes[at].nameId = at;
        if (s.names.count < s.nodes.count) {
            s.names.count = s.nodes.count;
        }

        if (n.children.empty()) {
            return;
        }
        const std::size_t count = n.children.size();
        if (s.nodes.count + count > Scene::kMaxNodes) {
            refuse("the scene needs more than this model's " + std::to_string(Scene::kMaxNodes) +
                   " nodes");
        }
        const std::uint16_t first = static_cast<std::uint16_t>(s.nodes.count);
        s.nodes.count += static_cast<std::uint32_t>(count);
        s.names.count = s.nodes.count;
        s.nodes[at].firstChild = first;
        s.nodes[at].childCount = static_cast<std::uint16_t>(count);

        for (std::size_t i = 0; i < count; ++i) {
            const std::uint16_t slot = static_cast<std::uint16_t>(first + i);
            s.nodes[slot].parent = at;
            emit(s, n.children[i], slot);
        }
    }

    std::size_t m_at = 0;
    std::vector<PovToken> m_tok;
    std::map<std::string, double> m_numbers;
    std::map<std::string, std::vector<double>> m_vectors;
    std::map<std::string, Material> m_textures;
    std::map<std::string, PovNode> m_objects;
    std::map<std::string, std::vector<PovMove>> m_transforms;
    std::vector<Material> m_materials;
    std::vector<std::string> m_unsupported;
};

}  // namespace detail

/// Reads a POV-Ray file into a scene, or throws PovParseError saying why it cannot.
inline PovImportResult importPov(const std::string& source) {
    detail::PovReader reader(source);
    return reader.read();
}

}  // namespace makina
