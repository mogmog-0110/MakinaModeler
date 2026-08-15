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
// For kMaxSorPoints / kMaxSweepPoints: the reader refuses what the evaluator could not hold,
// in the same breath.
#include "SorProfile.hpp"
#include "SweepProfile.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
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

/// A `#macro`: its parameter names and its body, kept as the tokens that were written.
struct PovMacro {
    std::vector<std::string> params;
    std::vector<PovToken>    body;
};

/// A material being read, plus the one finish term Material has no slot for.
///
/// finish{diffuse} rides here until the appearance is interned, because pigment and finish
/// arrive in either order and the value must be settled once, after both: it becomes
/// Material::finishDiffuse, except diffuse 0, which has no field form and becomes a black color.
struct PovAppearance {
    Material material{};
    /// finish{diffuse}, or negative while the file has not named one.
    double finishDiffuse = -1.0;
};

/// One parse in progress: the token stream, the symbol tables, and the tree being built.
class PovReader {
public:
    /// @param baseDir where #include resolves from; empty means includes are only named.
    explicit PovReader(const std::string& src, std::string baseDir = std::string())
        : m_tok(povTokenize(src)), m_baseDir(std::move(baseDir)) {}

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
            if (name == "rand" && isPunct('(', 1)) {
                // POV's own generator, characterised by measurement (black box, never its
                // source): seed(N) sets a 32-bit state to N, each draw steps
                // state = 1812433253 * state + 12345 mod 2^32 and returns state / (2^32 - 1).
                // Verified by predicting five draws for a fresh seed and matching POV's #debug
                // output to all 17 printed digits. If POV ever changes generators, the
                // scene.pov silhouette comparison is what catches it.
                take();
                take();
                if (peek().kind != PovTokenKind::Word ||
                    m_streams.count(peek().text) == 0) {
                    refuse("rand() wants a stream made by seed(); '" + peek().text +
                           "' is not one");
                }
                std::uint32_t& state = m_streams[take().text];
                expectPunct(')');
                state = static_cast<std::uint32_t>(1812433253u * state + 12345u);
                return static_cast<double>(state) / 4294967295.0;
            }
            if (isPunct('(', 1)) {
                // The rest of POV's function library. Each would have to produce the same
                // values POV does; rand/seed above are the ones a measured characterisation
                // exists for.
                refuse("'" + name + "' is a function call this reader does not evaluate");
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
    /// A POV 2D vector `<r, h>`, as sor writes its profile points. Literal components only: the
    /// profile lists in every measured file are plain numbers, and a declared vector here would
    /// be a 3D one wearing the wrong number of parts.
    void vector2(double out[2]) {
        expectPunct('<');
        out[0] = number();
        expectPunct(',');
        out[1] = number();
        expectPunct('>');
    }

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
            // A fourth and fifth component are POV's filter and transmit. Kept aside rather than
            // dropped: geometry never wants them, a pigment always does, and a reader that threw
            // them away turned every see-through surface in the file into a solid one.
            m_tail.clear();
            while (isPunct(',')) {
                take();
                m_tail.push_back(number());
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
        // Anything else -- a separating comma above all -- is consumed as one token. Every
        // caller loops "skip until '}'", and a skip that can decline to move turns the first
        // `area_light <..>, <..>, 9, 9` into a loop that never ends.
        if (peek().kind != PovTokenKind::End && !isPunct('}')) {
            take();
        }
    }

    // ---------------------------------------------------------------- top level

    void readTopLevel(PovNode& root) {
        while (expandIfMacro()) {}
        const PovToken t = peek();

        if (t.kind == PovTokenKind::Directive) {
            if (t.text == "#macro") {
                take();
                readMacroDefinition();
                return;
            }
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
                    const std::string file = take().text;
                    if (m_baseDir.empty()) {
                        // A string import has no directory to resolve from, so the include is
                        // named rather than silently skipped -- a scene that simply has fewer
                        // objects in it is the failure this line prevents.
                        note("#include \"" + file + "\"");
                        return;
                    }
                    spliceInclude(file);
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
            if (w == "light_source") {
                readLight();
                return;
            }
            if (w == "global_settings" || w == "camera" || w == "background" ||
                w == "sky_sphere" || w == "fog") {
                // The frame, not the model. This reader produces a scene, and the scene format
                // has nowhere to put a camera or a sky.
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
            {"lathe",          "a spline revolved about an axis"},
            {"prism",          "a spline swept along an axis"},
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
                                        "intersection", "object",     "blob",     "sor",
                                        "sphere_sweep"};
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
        while (expandIfMacro()) {}
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
        if (isWord("seed") && isPunct('(', 1)) {
            // A random stream, not a number: the name holds mutable generator state, and every
            // rand(name) after this advances it in file order -- the same order POV reads in.
            take();
            take();
            const double n = number();
            expectPunct(')');
            m_streams[name] = static_cast<std::uint32_t>(n);
            if (isPunct(';')) { take(); }
            return;
        }
        m_numbers[name] = number();
        if (isPunct(';')) { take(); }
    }

    // -------------------------------------------------------------------- macros

    /// `#macro Name(A, B) ... #end`: the body is captured as raw tokens, unparsed.
    ///
    /// Parsing at definition time would refuse a macro that is never called for something its
    /// caller would never reach; POV itself only reads a body on invocation, and agreeing with
    /// that is what lets a file define more than this subset and still load.
    void readMacroDefinition() {
        if (peek().kind != PovTokenKind::Word) {
            refuse("#macro needs a name");
        }
        PovMacro mac;
        const std::string name = take().text;
        expectPunct('(');
        while (!isPunct(')')) {
            if (peek().kind != PovTokenKind::Word) {
                refuse("#macro parameters must be names");
            }
            mac.params.push_back(take().text);
            if (isPunct(',')) {
                take();
            }
        }
        take();

        // To the matching #end. #while / #for / #switch close with #end too, so depth counts
        // them; a nested #macro is refused because POV refuses it (public reference, 3.7).
        int depth = 0;
        while (true) {
            const PovToken& t = peek();
            if (t.kind == PovTokenKind::End) {
                refuse("a #macro was not closed with #end");
            }
            if (t.kind == PovTokenKind::Directive) {
                if (t.text == "#macro") {
                    refuse("a #macro inside a #macro");
                }
                if (t.text == "#while" || t.text == "#for" || t.text == "#switch") {
                    ++depth;
                } else if (t.text == "#end") {
                    if (depth == 0) {
                        take();
                        break;
                    }
                    --depth;
                }
            }
            mac.body.push_back(take());
        }
        m_macros[name] = std::move(mac);
    }

    /// Reads an included file and splices its tokens at the cursor, the way a macro call
    /// splices its body: the stream continues seamlessly and every construct lands in the same
    /// tables. Only files inside the scene's own directory tree resolve -- POV's bundled
    /// include library (colors.inc and friends) is AGPL and stays out of this project's reach,
    /// so a scene needing it refuses by name instead of quietly losing its palette.
    void spliceInclude(const std::string& file) {
        const std::string path = m_baseDir + "/" + file;
        for (const std::string& already : m_includedFiles) {
            if (already == file) {
                refuse("'" + file + "' includes itself, through however many files");
            }
        }
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            refuse("#include could not open '" + file + "' beside the scene");
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        m_includedFiles.push_back(file);
        std::vector<PovToken> tokens = povTokenize(ss.str());
        if (!tokens.empty() && tokens.back().kind == PovTokenKind::End) {
            tokens.pop_back();
        }
        m_tok.insert(m_tok.begin() + static_cast<std::ptrdiff_t>(m_at), tokens.begin(),
                     tokens.end());
    }

    /// Expands `Name(args)` in place when Name is a defined macro. Returns whether it did.
    ///
    /// Expansion is token substitution: each argument is captured verbatim and spliced where the
    /// parameter's name appears, and the result is inserted into the stream for the ordinary
    /// parser to read. POV evaluates arguments at the call instead, which differs only when a
    /// name is redefined between call and use -- a file relying on that is refused by the
    /// expansion limit rather than misread, since the limit is the only place this shortcut can
    /// surface.
    bool expandIfMacro() {
        if (peek().kind != PovTokenKind::Word || !isPunct('(', 1)) {
            return false;
        }
        const auto it = m_macros.find(peek().text);
        if (it == m_macros.end()) {
            return false;
        }
        const std::string name = take().text;
        take();

        // Each argument's raw tokens. Only a comma at depth zero separates: one inside <...> or
        // (...) belongs to the vector or call it sits in.
        std::vector<std::vector<PovToken>> args;
        int paren = 0;
        int angle = 0;
        while (!(isPunct(')') && paren == 0 && angle == 0)) {
            const PovToken& t = peek();
            if (t.kind == PovTokenKind::End) {
                refuse("the call of macro '" + name + "' was not closed");
            }
            if (args.empty()) {
                args.emplace_back();
            }
            if (t.kind == PovTokenKind::Punct) {
                const char c = t.text[0];
                if (c == '(') { ++paren; }
                if (c == ')') { --paren; }
                if (c == '<') { ++angle; }
                if (c == '>') { --angle; }
                if (c == ',' && paren == 0 && angle == 0) {
                    take();
                    args.emplace_back();
                    continue;
                }
            }
            args.back().push_back(take());
        }
        take();

        const PovMacro& mac = it->second;
        if (args.size() != mac.params.size()) {
            refuse("macro '" + name + "' expects " + std::to_string(mac.params.size()) +
                   " argument(s) and was called with " + std::to_string(args.size()));
        }

        // POV evaluates arguments at the call, and for numbers this must not be skipped: spliced
        // raw, Pair(1+1) puts `-1+1` where the body wrote `-S`, and the minus binds to the first
        // token alone. Anything that does not read whole as an expression -- a texture name, an
        // object -- splices raw, which is the case substitution is right for.
        for (std::vector<PovToken>& a : args) {
            if (a.empty()) {
                continue;
            }
            std::vector<PovToken> probe = a;
            PovToken end = probe.back();
            end.kind = PovTokenKind::End;
            end.text.clear();
            probe.push_back(end);
            std::swap(m_tok, probe);
            const std::size_t at = m_at;
            m_at = 0;
            bool numeric = false;
            double v = 0.0;
            try {
                v = number();
                numeric = peek().kind == PovTokenKind::End;
            } catch (const PovParseError&) {
            }
            std::swap(m_tok, probe);
            m_at = at;
            if (numeric) {
                PovToken n = a.front();
                n.kind = PovTokenKind::Number;
                n.number = v;
                a.assign(1, n);
            }
        }
        // A macro that reaches itself grows the stream forever; there is no legitimate call
        // chain anywhere near this deep in a scene file.
        if (++m_expansions > 1000) {
            refuse("macro expansion does not terminate (at '" + name + "')");
        }

        std::vector<PovToken> out;
        for (const PovToken& t : mac.body) {
            std::size_t p = mac.params.size();
            if (t.kind == PovTokenKind::Word) {
                for (std::size_t i = 0; i < mac.params.size(); ++i) {
                    if (t.text == mac.params[i]) {
                        p = i;
                        break;
                    }
                }
            }
            if (p < mac.params.size()) {
                out.insert(out.end(), args[p].begin(), args[p].end());
            } else {
                out.push_back(t);
            }
        }
        m_tok.insert(m_tok.begin() + static_cast<std::ptrdiff_t>(m_at), out.begin(), out.end());
        return true;
    }

    // ---------------------------------------------------------------- appearance

    /// POV's default texture, which is what an object with no texture of its own wears.
    static Material defaultAppearance() {
        Material m{};
        m.diffuse[0] = m.diffuse[1] = m.diffuse[2] = 1.0f;
        m.alpha = 1.0f;
        m.ambient = 0.1f;
        m.specular = 0.0f;
        m.shininess = 0.0f;
        m.emission = 0.0f;
        m.textureId = -1;
        m.reflection = 0.0f;
        m.ior = 1.0f;
        return m;
    }

    PovAppearance readAppearance() {
        PovAppearance a;
        a.material = defaultAppearance();
        readAppearanceInto(a);
        return a;
    }

    /// `interior { ... }`, of which only the index of refraction is representable.
    ///
    /// Returns 1.0 when the block says nothing about `ior`, which is POV's own default and means
    /// the ray goes straight through. Everything else in there -- media, fade, caustics -- is
    /// reported by name, because an interior that was read but half ignored is exactly the kind of
    /// scene that renders without complaint and is not the one in the file.
    float readInterior() {
        take();
        expectPunct('{');
        float ior = 1.0f;
        while (!isPunct('}')) {
            if (peek().kind == PovTokenKind::End) {
                refuse("an interior block was not closed");
            }
            if (isWord("ior")) {
                take();
                ior = static_cast<float>(number());
                continue;
            }
            if (peek().kind == PovTokenKind::Word) {
                note("interior " + take().text);
                skipValue();
                continue;
            }
            skipValue();
        }
        take();
        return ior;
    }

    /// One `light_source { <at> color ... }`.
    ///
    /// Always a point light. POV has no directional light at all -- the exporter fakes one by
    /// putting a point light ten thousand units away along the direction -- so there is nothing in
    /// the file to read it back from, and a reader that guessed "far away means directional" would
    /// turn a genuinely distant lamp into one that never falls off.
    ///
    /// `softness` stays zero for the same reason: POV casts a hard shadow from a point light, so a
    /// file cannot be asking for anything else.
    void readLight() {
        take();
        expectPunct('{');
        Light l{};
        l.color[0] = l.color[1] = l.color[2] = 1.0f;

        double v[3];
        vector3(v);
        for (int i = 0; i < 3; ++i) {
            l.position[i] = static_cast<float>(v[i]);
        }

        while (!isPunct('}')) {
            if (peek().kind == PovTokenKind::End) {
                refuse("a light_source block was not closed");
            }
            if (isWord("color") || isWord("rgb") || isWord("srgb")) {
                float c[3] = {1, 1, 1};
                readMapColor(c);
                for (int i = 0; i < 3; ++i) {
                    l.color[i] = c[i];
                }
                continue;
            }
            if (isWord("shadowless")) {
                take();
                l.shadowless = 1u;
                continue;
            }
            if (isWord("fade_distance")) {
                take();
                l.fadeDistance = static_cast<float>(number());
                continue;
            }
            if (isWord("fade_power")) {
                take();
                l.fadePower = static_cast<float>(number());
                continue;
            }
            if (peek().kind == PovTokenKind::Word) {
                // area_light, spotlight, looks_like, projected_through: each changes what the lamp
                // is, and this model holds one kind of lamp. Named rather than dropped, because a
                // spotlight read as a point light lights the whole room.
                note("light_source " + take().text);
                skipValue();
                continue;
            }
            skipValue();
        }
        take();

        if (m_lights.size() >= Scene::kMaxLights) {
            refuse("the scene needs more than this model's " + std::to_string(Scene::kMaxLights) +
                   " lights");
        }
        m_lights.push_back(l);
    }

    /// The published sRGB transfer function, display value to linear.
    ///
    /// `srgb` numbers are display values and everything downstream of the reader is linear --
    /// the shading, the export, the comparison against POV's own render. Reading them as linear
    /// (which this did for a while) brightens every color and nothing complains, because the
    /// scene still loads and still renders. The decode is the standard's, not anything of POV's.
    static double srgbToLinear(double v) {
        return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    }

    /// One `color` entry of a pigment or a color_map, as three floats in 0..1.
    ///
    /// POV allows the keyword pair `color rgb <...>`, so the leading words are eaten until the
    /// vector is reached. The filter that may follow is not read here: a color_map entry that was
    /// half transparent is not a thing this model's two-stop pigment can hold.
    void readMapColor(float out[3]) {
        bool srgb = false;
        while (isWord("color") || isWord("rgb") || isWord("srgb") || isWord("rgbf") ||
               isWord("rgbt") || isWord("rgbft")) {
            if (peek().text == "srgb") {
                srgb = true;
            }
            take();
        }
        double c[3];
        vector3(c);
        for (int i = 0; i < 3; ++i) {
            out[i] = static_cast<float>(srgb ? srgbToLinear(c[i]) : c[i]);
        }
    }

    /// `color_map { [0.0 color ...] [1.0 color ...] }`, of which two stops are representable.
    ///
    /// A map with more stops is reported rather than truncated. Keeping the first and last would
    /// render a smooth ramp where the file asked for bands, which is a picture nobody would
    /// question and nobody could compare.
    void readColorMap(Pigment& g) {
        take();
        expectPunct('{');
        int stops = 0;
        while (!isPunct('}')) {
            if (peek().kind == PovTokenKind::End) {
                refuse("a color_map block was not closed");
            }
            if (!isPunct('[')) {
                skipValue();
                continue;
            }
            take();
            (void)number();   // the position, which a two-stop pigment fixes at 0 and 1
            float c[3] = {0, 0, 0};
            readMapColor(c);
            while (!isPunct(']')) {
                if (peek().kind == PovTokenKind::End) {
                    refuse("a color_map entry was not closed");
                }
                skipValue();
            }
            take();
            if (stops == 0) {
                for (int i = 0; i < 3; ++i) { g.a[i] = c[i]; }
            } else if (stops == 1) {
                for (int i = 0; i < 3; ++i) { g.b[i] = c[i]; }
            }
            ++stops;
        }
        take();
        if (stops > 2) {
            note("color_map with " + std::to_string(stops) + " stops");
        }
    }

    /// The pattern of a pigment, and the transform POV applies to it.
    ///
    /// Only the three with no noise in them. The rest of POV's patterns read its own permutation
    /// table, and without that exact table what comes out is "something that looks like marble" --
    /// which is the failure this reader exists to refuse.
    void readPatternInto(Material& m) {
        Pigment g{};
        g.scale[0] = g.scale[1] = g.scale[2] = 1.0f;
        g.axis[0] = 1.0f;

        const std::string kind = take().text;
        if (kind == "checker") {
            g.type = static_cast<std::uint8_t>(PigmentType::Checker);
            // The two colors follow the keyword directly, with no map.
            if (isWord("color") || isWord("rgb") || isWord("srgb")) {
                readMapColor(g.a);
            }
            if (isWord("color") || isWord("rgb") || isWord("srgb")) {
                readMapColor(g.b);
            }
        } else if (kind == "gradient") {
            g.type = static_cast<std::uint8_t>(PigmentType::Gradient);
            double v[3];
            vector3(v);
            for (int i = 0; i < 3; ++i) {
                g.axis[i] = static_cast<float>(v[i]);
            }
        } else {
            g.type = static_cast<std::uint8_t>(PigmentType::Radial);
        }

        // The modifiers, which POV lets follow in any order and any number.
        while (!isPunct('}')) {
            if (peek().kind == PovTokenKind::End) {
                refuse("a pigment block was not closed");
            }
            if (isWord("color_map")) {
                readColorMap(g);
                continue;
            }
            if (isWord("scale") || isWord("translate")) {
                const bool isScale = isWord("scale");
                take();
                double v[3];
                if (peek().kind == PovTokenKind::Number && !isPunct('<')) {
                    // POV takes a bare number as the same value on all three axes.
                    const double u = number();
                    v[0] = v[1] = v[2] = u;
                } else {
                    vector3(v);
                }
                for (int i = 0; i < 3; ++i) {
                    if (isScale) {
                        g.scale[i] *= static_cast<float>(v[i]);
                    } else {
                        g.translate[i] += static_cast<float>(v[i]);
                    }
                }
                continue;
            }
            if (isWord("rotate")) {
                // Pigment has three axes of scale and a translation and nowhere to put a rotation,
                // so a rotated pattern would come out aligned to the wrong axes. Named, not turned
                // into the nearest thing that parses.
                note("pigment rotate");
                skipValue();
                continue;
            }
            if (peek().kind == PovTokenKind::Word) {
                note("pigment " + take().text);
                skipValue();
                continue;
            }
            skipValue();
        }

        if (m_pigments.size() >= Scene::kMaxPigments) {
            refuse("the scene needs more than this model's " +
                   std::to_string(Scene::kMaxPigments) + " pigments");
        }
        for (std::size_t i = 0; i < m_pigments.size(); ++i) {
            if (std::memcmp(&m_pigments[i], &g, sizeof(Pigment)) == 0) {
                m.textureId = static_cast<std::int32_t>(i);
                return;
            }
        }
        m_pigments.push_back(g);
        m.textureId = static_cast<std::int32_t>(m_pigments.size() - 1);
    }

    void readAppearanceInto(PovAppearance& a) {
        Material& m = a.material;
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
            a = m_textures[take().text];
            take();
            return;
        }
        expectPunct('{');

        while (!isPunct('}')) {
            if (peek().kind == PovTokenKind::End) {
                refuse("a " + kind + " block was not closed");
            }
            if (isWord("texture") || isWord("pigment") || isWord("finish") || isWord("normal")) {
                readAppearanceInto(a);
                continue;
            }
            if (peek().kind != PovTokenKind::Word) {
                skipValue();
                continue;
            }

            // A pattern, before the plain-color branch below: `checker` is followed by the two
            // colors it alternates, so reading the first of them as this pigment's flat color
            // would give a solid surface in the color of one square.
            if (isWord("checker") || isWord("gradient") || isWord("radial")) {
                readPatternInto(m);
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
                m_tail.clear();
                vector3(c);
                // On the lap that reads the vector, w is the last keyword of the pair -- which
                // is `srgb` exactly when the numbers are display values needing the decode.
                for (int i = 0; i < 3; ++i) {
                    m.diffuse[i] = static_cast<float>(w == "srgb" ? srgbToLinear(c[i]) : c[i]);
                }
                // POV's fourth component is a filter after `rgbf` and a transmit after `rgbt`,
                // and `rgbft` carries both. This model holds one opacity, so filter is what it
                // reads; transmit is reported instead of being folded in, because a transmitting
                // surface does not tint what is behind it and pretending it does is a picture
                // nobody would question.
                if (!m_tail.empty()) {
                    if (w == "rgbt") {
                        note("pigment transmit");
                    } else {
                        m.alpha = static_cast<float>(1.0 - m_tail[0]);
                        if (m_tail.size() > 1 && m_tail[1] != 0.0) {
                            note("pigment transmit");
                        }
                    }
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
                a.finishDiffuse = number();
                continue;
            }
            if (w == "brilliance") {
                m.brilliance = static_cast<float>(number());
                continue;
            }
            if (m_textures.count(w) > 0) {
                a = m_textures[w];
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
        if (isWord("blob"))   { return readBlob(); }
        if (isWord("sor"))    { return readSor(); }
        if (isWord("sphere_sweep")) { return readSphereSweep(); }
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
            while (expandIfMacro()) {}
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
        while (expandIfMacro()) {}
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

    /// `blob { threshold T sphere{...} cylinder{...} ... }`.
    ///
    /// Handled with its own loop rather than modifiers(), because POV lets components and
    /// modifiers interleave and an unknown word here has to refuse: a component skipped as "some
    /// modifier" would load a solid with a lump missing, which no report line makes acceptable.
    PovNode readBlob() {
        take();
        expectPunct('{');
        PovNode node = leaf(Op::Blob, "Blob");
        node.params[0] = 1.0f;   // POV's default threshold (public reference, 3.7)
        PovAppearance appearance;
        appearance.material = defaultAppearance();
        bool dressed = false;

        while (!isPunct('}')) {
            if (peek().kind == PovTokenKind::End) {
                refuse("a blob block was not closed");
            }
            // Through shapeOf: a transform modifier may already have wrapped the Blob node.
            if (isWord("threshold")) {
                take();
                shapeOf(node).params[0] = static_cast<float>(number());
                continue;
            }
            if (isWord("sphere") || isWord("cylinder")) {
                shapeOf(node).children.push_back(blobComponent());
                continue;
            }
            if (modifierStep(node, appearance, dressed)) {
                continue;
            }
            refuse("'" + peek().text + "' inside a blob is not something this reader knows");
        }
        take();
        if (dressed) {
            shapeOf(node).material = materialIndex(appearance);
        }
        return node;
    }

    /// `sor { N, <r,h>, ... [sturm] mods }`: a profile revolved about local Y.
    ///
    /// The heights must climb through the whole list -- POV requires it, and the cross-section
    /// this model evaluates (Eval.hpp) needs the profile to be a function of height. `open`
    /// refuses: a capless surface of revolution has no interior and cannot join a CSG solid.
    PovNode readSor() {
        take();
        expectPunct('{');
        PovNode n = leaf(Op::Sor, "Sor");
        const int total = static_cast<int>(number());
        if (total < 4) {
            refuse("a sor needs at least four points: the outer two only steer the end slopes");
        }
        if (total > detail::kMaxSorPoints) {
            refuse("a sor with more than " + std::to_string(detail::kMaxSorPoints) +
                   " points is more profile than this model holds");
        }
        double prevH = 0.0;
        for (int i = 0; i < total; ++i) {
            if (isPunct(',')) {
                take();
            }
            double v[2];
            vector2(v);
            if (i > 0 && v[1] <= prevH) {
                refuse("a sor's heights must strictly increase; point " + std::to_string(i + 1) +
                       " goes back down");
            }
            prevH = v[1];
            PovNode pt = leaf(Op::SorPoint, "SorPoint");
            pt.params[0] = static_cast<float>(v[0]);
            pt.params[1] = static_cast<float>(v[1]);
            n.children.push_back(std::move(pt));
        }
        while (isWord("sturm") || isWord("open")) {
            if (isWord("open")) {
                refuse("an open sor has no interior and cannot be a CSG solid");
            }
            // sturm tunes POV's root solver, not the surface: the one word read without a note.
            take();
        }
        return modifiers(std::move(n));
    }

    /// `sphere_sweep { KIND N, <c>, r, ... [tolerance] mods }`: the envelope of a sphere
    /// moving along a spline, its radius interpolated with the same spline as its centre.
    ///
    /// cubic_spline is refused by name until it is measured; guessing its basis would draw a
    /// tube that follows a subtly different path with nothing to say so. `tolerance` tunes
    /// POV's root finding, not the surface, so it reads silently like sor's sturm.
    PovNode readSphereSweep() {
        take();
        expectPunct('{');
        PovNode n = leaf(Op::SphereSweep, "SphereSweep");
        bool bspline = false;
        if (isWord("linear_spline")) {
            take();
        } else if (isWord("b_spline")) {
            take();
            bspline = true;
            n.flags |= flags::kSweepBspline;
        } else if (isWord("cubic_spline")) {
            refuse("a cubic_spline sphere_sweep is not held yet; linear_spline and b_spline "
                   "are");
        } else {
            refuse("a sphere_sweep names its spline first: linear_spline or b_spline");
        }
        const int total = static_cast<int>(number());
        if (total > detail::kMaxSweepPoints) {
            refuse("a sphere_sweep with more than " +
                   std::to_string(detail::kMaxSweepPoints) + " points is more path than this "
                   "model holds");
        }
        if (total < (bspline ? 4 : 2)) {
            refuse(std::string("a ") + (bspline ? "b_spline" : "linear_spline") +
                   " sphere_sweep needs at least " + (bspline ? "four" : "two") + " points");
        }
        for (int i = 0; i < total; ++i) {
            if (isPunct(',')) {
                take();
            }
            double v[3];
            vector3(v);
            if (isPunct(',')) {
                take();
            }
            const double r = number();
            if (r <= 0.0) {
                refuse("a sphere_sweep point needs a positive radius");
            }
            PovNode pt = leaf(Op::SweepPoint, "SweepPoint");
            for (int k = 0; k < 3; ++k) {
                pt.params[k] = static_cast<float>(v[k]);
            }
            pt.params[3] = static_cast<float>(r);
            n.children.push_back(std::move(pt));
        }
        while (isWord("tolerance")) {
            take();
            number();
        }
        return modifiers(std::move(n));
    }

    /// One blob component: `sphere { <c>, R [, [strength] S] mods }` or
    /// `cylinder { <a>, <b>, R [, [strength] S] mods }`. Transform modifiers wrap the component
    /// the same way they wrap an object; the field walk in Eval.hpp undoes them per component.
    PovNode blobComponent() {
        const bool isSphere = peek().text == "sphere";
        take();
        expectPunct('{');
        PovNode c = leaf(isSphere ? Op::BlobSphere : Op::BlobCylinder,
                         isSphere ? "BlobSphere" : "BlobCylinder");
        double v[3];
        vector3(v);
        int at = 0;
        for (int i = 0; i < 3; ++i) {
            c.params[at++] = static_cast<float>(v[i]);
        }
        if (!isSphere) {
            expectPunct(',');
            vector3(v);
            for (int i = 0; i < 3; ++i) {
                c.params[at++] = static_cast<float>(v[i]);
            }
        }
        expectPunct(',');
        const double radius = number();
        if (radius <= 0.0) {
            refuse("a blob component needs a positive radius");
        }
        // The strength and its optional keyword. Left out, POV takes 1.0.
        double strength = 1.0;
        if (isPunct(',') || isWord("strength")) {
            if (isPunct(',')) {
                take();
            }
            if (isWord("strength")) {
                take();
            }
            strength = number();
        }
        c.params[at++] = static_cast<float>(radius);
        c.params[at] = static_cast<float>(strength);

        PovNode out = std::move(c);
        while (!isPunct('}')) {
            if (peek().kind == PovTokenKind::End) {
                refuse("a blob component was not closed");
            }
            if (isWord("translate") || isWord("scale") || isWord("rotate")) {
                out = transform(std::move(out));
                continue;
            }
            if (isWord("transform")) {
                for (const PovMove& m : readTransformBlock()) {
                    out = applyMove(std::move(out), m.kind, m.v);
                }
                continue;
            }
            if (isWord("texture") || isWord("pigment") || isWord("finish")) {
                // A texture per component blends textures across the surface, and this model
                // holds one material per blob. Named, so the file's look is reported as lost.
                note("blob component texture");
                take();
                skipBlock();
                continue;
            }
            if (peek().kind == PovTokenKind::Word) {
                refuse("'" + peek().text + "' on a blob component is not something this reader "
                       "knows");
            }
            skipValue();
        }
        take();
        return out;
    }

    /// One recognised object-modifier at the cursor: appearance folds into `appearance`, a
    /// transform wraps `node`. False when the token is not a modifier this reader knows -- the
    /// caller decides whether that is noted or refused. The split exists for readBlob(): a blob
    /// must refuse an unknown word, or a mistyped component would be skipped and the solid would
    /// quietly lose a lump.
    bool modifierStep(PovNode& node, PovAppearance& appearance, bool& dressed) {
        if (isWord("texture")) {
            // A whole texture replaces whatever came before it. The index of refraction does
            // not: POV hangs that on the object and the texture on the surface, so a file may
            // write either first and the second must not undo the first.
            const float ior = appearance.material.ior;
            appearance = readAppearance();
            appearance.material.ior = ior;
            dressed = true;
            return true;
        }
        if (isWord("pigment") || isWord("finish") || isWord("normal")) {
            // Bare, these are items of one implicit texture and each says only its own part,
            // so they accumulate. Starting a fresh material per block was the bug that made
            // every exported scene come back white: the exporter writes `pigment` and then
            // `finish`, and the finish was overwriting the color that had just been read.
            readAppearanceInto(appearance);
            dressed = true;
            return true;
        }
        if (isWord("interior")) {
            appearance.material.ior = readInterior();
            dressed = true;
            return true;
        }
        if (isWord("translate") || isWord("scale") || isWord("rotate")) {
            node = transform(std::move(node));
            return true;
        }
        if (isWord("transform")) {
            for (const PovMove& m : readTransformBlock()) {
                node = applyMove(std::move(node), m.kind, m.v);
            }
            return true;
        }
        if (peek().kind == PovTokenKind::Word && m_textures.count(peek().text) > 0) {
            const float ior = appearance.material.ior;
            appearance = m_textures[take().text];
            appearance.material.ior = ior;
            dressed = true;
            return true;
        }
        return false;
    }

    /// The modifiers after a shape's own arguments, up to the closing brace.
    ///
    /// Appearance is gathered and interned once, at the end. POV lets a texture and an interior sit
    /// on the same object in either order, and interning after each one would leave the half-built
    /// material in the table for nothing to point at.
    PovNode modifiers(PovNode node) {
        PovAppearance appearance;
        appearance.material = node.material >= 0
                                  ? m_materials[static_cast<std::size_t>(node.material)]
                                  : defaultAppearance();
        bool dressed = false;
        while (!isPunct('}')) {
            if (peek().kind == PovTokenKind::End) {
                refuse("an object block was not closed");
            }
            if (modifierStep(node, appearance, dressed)) {
                continue;
            }
            if (peek().kind == PovTokenKind::Word) {
                // open, hollow, no_shadow, bounded_by, photons: each changes what the object is or
                // how it is drawn, so each is reported rather than dropped.
                note("modifier " + take().text);
                skipValue();
                continue;
            }
            skipValue();
        }
        take();
        if (dressed) {
            // On the shape, not on the transform wrapping it. POV writes the texture inside the
            // object and the translate after it, and both apply to the object -- so the shape is
            // where the file put it. It matters beyond tidiness: a pattern is read in the space of
            // whatever wears it, and a checker hung on the wrapper instead would be read in world
            // space and come out a square out of step.
            shapeOf(node).material = materialIndex(appearance);
        }
        return node;
    }

    /// The shape inside any transforms this reader wrapped around it.
    ///
    /// A transform node here always has exactly one child, because that is the only way this
    /// reader builds one; anything else is left alone rather than guessed at.
    static PovNode& shapeOf(PovNode& n) {
        PovNode* p = &n;
        while (isTransform(p->op) && p->children.size() == 1) {
            p = &p->children[0];
        }
        return *p;
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

    /// Interns an appearance, settling finish{diffuse} exactly once.
    int materialIndex(const PovAppearance& a) {
        Material m = a.material;
        if (a.finishDiffuse >= 0.0 && std::fabs(a.finishDiffuse - 0.6) > 1e-9) {
            if (a.finishDiffuse <= 0.0) {
                // Zero is the field's "unset" value (Scene.hpp), so diffuse 0 is held as a black
                // color instead: the lamps then contribute nothing, which is what POV draws.
                // Beside a lit ambient that has no form -- the pigment would have to be black
                // for the lamps and colored for the ambient at once -- so it is named.
                if (m.ambient > 0.0f) {
                    note("finish diffuse 0");
                }
                for (int c = 0; c < 3; ++c) {
                    m.diffuse[c] = 0.0f;
                }
            } else {
                m.finishDiffuse = static_cast<float>(a.finishDiffuse);
            }
        }
        return materialIndex(m);
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
        for (std::size_t i = 0; i < m_pigments.size(); ++i) {
            s.pigments[s.pigments.count++] = m_pigments[i];
        }
        for (std::size_t i = 0; i < m_lights.size(); ++i) {
            s.lights[s.lights.count++] = m_lights[i];
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
    std::map<std::string, PovAppearance> m_textures;
    std::vector<Pigment> m_pigments;
    std::vector<Light> m_lights;
    std::map<std::string, PovNode> m_objects;
    std::map<std::string, std::vector<PovMove>> m_transforms;
    std::map<std::string, PovMacro> m_macros;
    std::string m_baseDir;
    /// Every file spliced so far. One splice per name: a repeat is a cycle more often than a
    /// design, and refusing it names the loop instead of running the token count to the cap.
    std::vector<std::string> m_includedFiles;
    /// Random streams by name: seed(N) creates one, rand(name) steps it. See factor() for the
    /// measured generator.
    std::map<std::string, std::uint32_t> m_streams;
    /// Total macro expansions so far; the bound that turns recursion into a refusal.
    int m_expansions = 0;
    /// The components past the third of the last `<...>` literal read.
    ///
    /// A side channel, because the vector grammar is shared with geometry and widening every
    /// caller to four numbers would put a filter where a coordinate goes. The last literal wins,
    /// which is what a color wants: it is the whole expression in every file this reads.
    std::vector<double> m_tail;
    std::vector<Material> m_materials;
    std::vector<std::string> m_unsupported;
};

}  // namespace detail

/// Reads a POV-Ray file into a scene, or throws PovParseError saying why it cannot.
/// @param baseDir the directory #include resolves from. Empty (the default) keeps the old
/// behaviour: includes are reported by name instead of followed.
inline PovImportResult importPov(const std::string& source,
                                 const std::string& baseDir = std::string()) {
    detail::PovReader reader(source, baseDir);
    return reader.read();
}

}  // namespace makina
