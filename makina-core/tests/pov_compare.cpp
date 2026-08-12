// Compares the POV-Ray source makina-core writes against the one Grasp3D writes.
//
// tools/povdump writes the reference; this generates the same scene from JSON and checks the two
// describe the same objects.
//
// Not a text diff. The two spell numbers differently -- CsgNode keeps parameters as float, the
// reference works in double -- and lay out whitespace differently, so a byte comparison would fail
// on every line while telling nothing about the geometry. Both sides are tokenised instead, and the
// token streams have to match exactly except that numbers are compared numerically. That still
// catches everything worth catching: a missing union around a multi-object CSG operand, a
// difference that took the wrong operand first, a transform applied in the wrong order, a face that
// went into a boolean without being thickened.
//
// Camera, lights and background are skipped on both sides. They are not in Scene -- the caller
// supplies them -- so comparing them would compare two copies of a constant.

#include <makina/Pov.hpp>
#include <makina/SceneJson.hpp>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

/// Numbers agree to a relative 1e-5. Wider than the SDF comparison because a POV literal is the
/// end of a longer float chain -- a normalised plane normal in a triangle solid, say -- and the
/// question here is whether the two wrote the same value, not how precisely they wrote it.
constexpr double kRelTolerance = 1e-5;

struct Token {
    std::string text;
    double      value = 0.0;
    bool        isNumber = false;
};

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("could not open '" + path + "'");
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool startsNumber(const std::string& s, std::size_t i) {
    if (std::isdigit(static_cast<unsigned char>(s[i]))) {
        return true;
    }
    // A sign or a point only opens a number when a digit follows; otherwise '-' is the negation
    // POV writes inside a vector and '.' cannot occur at all.
    if ((s[i] == '-' || s[i] == '+' || s[i] == '.') && i + 1 < s.size()) {
        return std::isdigit(static_cast<unsigned char>(s[i + 1])) != 0;
    }
    return false;
}

std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> out;
    std::size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') {
                ++i;
            }
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            // The reference writes a Label out as a block comment. It carries the label text, not
            // geometry, so it is skipped rather than compared.
            const std::size_t close = s.find("*/", i + 2);
            i = close == std::string::npos ? s.size() : close + 2;
            continue;
        }
        if (startsNumber(s, i)) {
            std::size_t end = i;
            while (end < s.size() &&
                   (std::isdigit(static_cast<unsigned char>(s[end])) || s[end] == '.' ||
                    s[end] == '-' || s[end] == '+' || s[end] == 'e' || s[end] == 'E')) {
                // A '-' only continues the number as an exponent sign.
                if ((s[end] == '-' || s[end] == '+') && end != i &&
                    !(s[end - 1] == 'e' || s[end - 1] == 'E')) {
                    break;
                }
                ++end;
            }
            Token t;
            t.text = s.substr(i, end - i);
            t.value = std::stod(t.text);
            t.isNumber = true;
            out.push_back(t);
            i = end;
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::size_t end = i;
            while (end < s.size() &&
                   (std::isalnum(static_cast<unsigned char>(s[end])) || s[end] == '_')) {
                ++end;
            }
            out.push_back(Token{s.substr(i, end - i), 0.0, false});
            i = end;
            continue;
        }
        out.push_back(Token{std::string(1, c), 0.0, false});
        ++i;
    }
    return out;
}

bool isSkipped(const std::string& name) {
    return name == "camera" || name == "light_source" || name == "background" ||
           name == "sky_sphere" || name == "global_settings";
}

/// Drops the blocks neither side takes from the scene, leaving geometry only.
std::vector<Token> geometryOnly(const std::vector<Token>& in) {
    std::vector<Token> out;
    for (std::size_t i = 0; i < in.size();) {
        if (!in[i].isNumber && isSkipped(in[i].text) && i + 1 < in.size() &&
            in[i + 1].text == "{") {
            int depth = 0;
            std::size_t j = i + 1;
            for (; j < in.size(); ++j) {
                if (in[j].text == "{") {
                    ++depth;
                } else if (in[j].text == "}") {
                    if (--depth == 0) {
                        ++j;
                        break;
                    }
                }
            }
            i = j;
            continue;
        }
        out.push_back(in[i]);
        ++i;
    }
    return out;
}

/// A few tokens of context, so a mismatch report says where in the scene it happened.
std::string around(const std::vector<Token>& t, std::size_t at) {
    const std::size_t from = at > 6 ? at - 6 : 0;
    const std::size_t to = at + 4 < t.size() ? at + 4 : t.size();
    std::string s;
    for (std::size_t i = from; i < to; ++i) {
        if (i == at) {
            s += ">>";
        }
        s += t[i].text;
        s += " ";
    }
    return s;
}

void compareOne(const std::string& jsonPath, const std::string& refPath) {
    std::printf("%s\n", jsonPath.c_str());

    const makina::Scene scene = makina::parseScene(readFile(jsonPath));
    const std::string mine = makina::writePov(scene, makina::PovOptions{});

    const std::vector<Token> a = geometryOnly(tokenize(readFile(refPath)));
    const std::vector<Token> b = geometryOnly(tokenize(mine));

    const std::size_t n = a.size() < b.size() ? a.size() : b.size();
    int reported = 0;
    for (std::size_t i = 0; i < n; ++i) {
        ++checks;
        bool ok;
        if (a[i].isNumber != b[i].isNumber) {
            ok = false;
        } else if (a[i].isNumber) {
            const double scale = std::fabs(a[i].value) > 1.0 ? std::fabs(a[i].value) : 1.0;
            ok = std::fabs(a[i].value - b[i].value) <= kRelTolerance * scale;
        } else {
            ok = a[i].text == b[i].text;
        }
        if (!ok) {
            ++failures;
            if (reported++ < 3) {
                std::printf("    FAIL  token %zu: java '%s' cpp '%s'\n", i, a[i].text.c_str(),
                            b[i].text.c_str());
                std::printf("          java ... %s\n", around(a, i).c_str());
                std::printf("          cpp  ... %s\n", around(b, i).c_str());
            }
            // The streams have desynchronised; everything after this would be noise.
            break;
        }
    }

    ++checks;
    if (a.size() != b.size()) {
        std::printf("    FAIL  %zu tokens in the reference, %zu here\n", a.size(), b.size());
        ++failures;
        return;
    }
    if (reported == 0) {
        std::printf("    %zu tokens, all agree\n", a.size());
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || (argc - 1) % 2 != 0) {
        std::fprintf(stderr, "usage: pov_compare <scene.json> <scene.pov> [more pairs ...]\n");
        return 2;
    }

    std::printf("makina-core vs Grasp3D POV export\n\n");

    for (int i = 1; i + 1 < argc; i += 2) {
        try {
            compareOne(argv[i], argv[i + 1]);
        } catch (const std::exception& e) {
            std::printf("    FAIL  %s\n", e.what());
            ++failures;
        }
    }

    if (failures == 0) {
        std::printf("\nthe POV export agrees with the reference (%d checks)\n", checks);
        return 0;
    }
    std::printf("\n%d of %d checks FAILED\n", failures, checks);
    return 1;
}
