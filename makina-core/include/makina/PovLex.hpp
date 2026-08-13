// Turning POV-Ray source into tokens.
//
// The exporter has always been the third independent implementation of this model. Reading the
// format back makes that route two-way, and the reason to want it is verification: every fixture
// in this repository was written by whoever wrote the renderer, so none of them can catch an
// assumption made twice. A scene someone else wrote can.
//
// Comments are handled here rather than in the parser because POV allows one between any two
// tokens, including inside a vector literal, and a parser that had to skip them at every step
// would grow that check into every rule.

#pragma once

#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace makina {

/// Thrown for anything the reader will not guess at.
///
/// Carries the line, because a POV file is long and "unsupported" without a position is a search
/// rather than a fix.
class PovParseError : public std::runtime_error {
public:
    PovParseError(int line, const std::string& what)
        : std::runtime_error("line " + std::to_string(line) + ": " + what) {}
};

namespace detail {

enum class PovTokenKind { End, Word, Number, String, Punct, Directive };

struct PovToken {
    PovTokenKind kind = PovTokenKind::End;
    std::string  text;
    double       number = 0.0;
    int          line = 1;
};

inline bool povIsWordStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

inline bool povIsWordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

/// Skips a block comment, which POV nests unlike C.
///
/// Counting depth rather than searching for the first close is the difference between reading a
/// file and reading half of one.
inline void povSkipBlockComment(const std::string& src, std::size_t& i, int& line) {
    const int opened = line;
    i += 2;
    int depth = 1;
    while (i < src.size() && depth > 0) {
        if (src[i] == '\n') {
            ++line;
        } else if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*') {
            ++depth;
            ++i;
        } else if (src[i] == '*' && i + 1 < src.size() && src[i + 1] == '/') {
            --depth;
            ++i;
        }
        ++i;
    }
    if (depth > 0) {
        throw PovParseError(opened, "a block comment opened here and never closed");
    }
}

inline std::vector<PovToken> povTokenize(const std::string& src) {
    std::vector<PovToken> out;
    int line = 1;
    std::size_t i = 0;

    while (i < src.size()) {
        const char c = src[i];

        if (c == '\n') { ++line; ++i; continue; }
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }

        if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') { ++i; }
            continue;
        }
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '*') {
            povSkipBlockComment(src, i, line);
            continue;
        }

        if (c == '#') {
            std::size_t j = i + 1;
            while (j < src.size() && povIsWordChar(src[j])) { ++j; }
            out.push_back({PovTokenKind::Directive, src.substr(i, j - i), 0.0, line});
            i = j;
            continue;
        }

        if (c == '"') {
            std::size_t j = i + 1;
            std::string text;
            while (j < src.size() && src[j] != '"') {
                if (src[j] == '\n') { ++line; }
                text.push_back(src[j++]);
            }
            if (j >= src.size()) {
                throw PovParseError(line, "a string opened here and never closed");
            }
            out.push_back({PovTokenKind::String, text, 0.0, line});
            i = j + 1;
            continue;
        }

        if (povIsWordStart(c)) {
            std::size_t j = i;
            while (j < src.size() && povIsWordChar(src[j])) { ++j; }
            out.push_back({PovTokenKind::Word, src.substr(i, j - i), 0.0, line});
            i = j;
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < src.size() &&
             std::isdigit(static_cast<unsigned char>(src[i + 1])))) {
            const char* start = src.c_str() + i;
            char* end = nullptr;
            const double v = std::strtod(start, &end);
            const std::size_t used = static_cast<std::size_t>(end - start);
            if (used == 0) {
                throw PovParseError(line, "a number was expected here");
            }
            out.push_back({PovTokenKind::Number, src.substr(i, used), v, line});
            i += used;
            continue;
        }

        // Everything else is one character of punctuation. POV has no multi-character operator
        // this subset needs, and joining them here would only make the parser guess.
        out.push_back({PovTokenKind::Punct, std::string(1, c), 0.0, line});
        ++i;
    }

    out.push_back({PovTokenKind::End, "", 0.0, line});
    return out;
}

}  // namespace detail
}  // namespace makina
