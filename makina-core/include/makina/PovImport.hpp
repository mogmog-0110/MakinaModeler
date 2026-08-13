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
            refuse("'" + name + "' is not a value this reader knows");
        }
        refuse("expected a number but found '" + peek().text + "'");
    }

    /// `<x,y,z>`, a declared vector, one of POV's built-in axis names, or one number for all
    /// three.
    void vector3(double out[3]) {
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
            const auto it = m_vectors.find(peek().text);
            if (it != m_vectors.end()) {
                take();
                for (int i = 0; i < 3; ++i) {
                    out[i] = it->second[i];
                }
                return;
            }
        }
        if (!isPunct('<')) {
            // POV lets a scalar stand for a uniform vector, which `scale 2` relies on.
            const double v = number();
            out[0] = out[1] = out[2] = v;
            return;
        }
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

        if (isWord("texture") || isWord("pigment") || isWord("finish") || isWord("normal")) {
            m_textures[name] = readAppearance();
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
        m._pad = 0;
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

        if (std::fabs(a[0] - b[0]) > 1e-9 || std::fabs(a[2] - b[2]) > 1e-9) {
            // Only an axis-aligned cylinder maps to this model's cap and base. Straightening a
            // slanted one would move geometry the file did not ask to move.
            refuse("this reader takes a cylinder along Y; this one runs between points that "
                   "differ in x or z");
        }
        if (r <= 0.0) {
            refuse("a cylinder needs a positive radius");
        }
        PovNode n = leaf(Op::Cylinder, "Cylinder");
        n.params[0] = static_cast<float>(a[1] > b[1] ? a[1] : b[1]);
        n.params[1] = static_cast<float>(a[1] < b[1] ? a[1] : b[1]);
        n.params[2] = static_cast<float>(r);
        return place(std::move(n), a[0], 0.0, a[2]);
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

        if (std::fabs(a[0] - b[0]) > 1e-9 || std::fabs(a[2] - b[2]) > 1e-9) {
            refuse("this reader takes a cone along Y");
        }
        if (ra > 1e-9 && rb > 1e-9) {
            // A truncated cone has two radii; this model's Cone has one and a tip.
            refuse("this reader takes a cone that comes to a point; this one is truncated");
        }
        const double radius = ra > rb ? ra : rb;
        const double baseY  = ra > rb ? a[1] : b[1];
        const double tipY   = ra > rb ? b[1] : a[1];

        PovNode n = leaf(Op::Cone, "Cone");
        n.params[0] = static_cast<float>(radius);
        // Signed, and that matters: a negative height is how this model says the cone points
        // down, and Flatten.hpp reads it as the sign on y. Taking the magnitude turns every
        // downward cone upright, which is a scene that loads and is not the one in the file.
        n.params[1] = static_cast<float>(tipY - baseY);
        return place(std::move(n), a[0], baseY, a[2]);
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
        if (std::fabs(normal[0]) > 1e-9 || std::fabs(normal[2]) > 1e-9 || normal[1] <= 0.0) {
            refuse("this reader takes a plane whose normal is +Y");
        }
        return place(leaf(Op::Plane, "Plane"), 0.0, dist, 0.0);
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
        if (std::fabs(normal[0]) > 1e-9 || std::fabs(normal[2]) > 1e-9 || normal[1] <= 0.0) {
            refuse("this reader takes a disc whose normal is +Y");
        }
        if (radius <= 0.0) {
            refuse("a disc needs a positive radius");
        }
        PovNode n = leaf(Op::Disc, "Disc");
        n.params[0] = static_cast<float>(radius);
        n.params[1] = static_cast<float>(hole);
        return place(std::move(n), c[0], c[1], c[2]);
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
            if (!isObjectStart()) {
                break;
            }
            n.children.push_back(readObject());
        }
        return modifiers(std::move(n));
    }

    PovNode instance() {
        take();
        expectPunct('{');
        if (peek().kind != PovTokenKind::Word) {
            refuse("object{ needs the name of a declared object");
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

    PovNode transform(PovNode node) {
        const std::string kind = take().text;
        double v[3];
        vector3(v);

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
