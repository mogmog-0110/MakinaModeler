// A report card for a .pov file: what is in it, and what of that this project can hold.
//
// The importer refuses at the first construct it cannot represent, which is the right behaviour
// for loading -- a scene that reads as almost-the-file is worse than no scene -- but the wrong
// tool for judging a file found on the internet. One refusal names one problem; the file may
// hold five more behind it.
//
// So this walks the whole token stream without building anything, counts every construct it
// recognises, and reports each with a status. It never refuses: a survey that stops early is a
// survey of the wrong file.
//
// Honesty is kept by a cross-check rather than by care: pov_import_check asserts, on every
// fixture, that a survey with nothing unsupported is exactly a file povImport() reads without
// refusing. If this table drifts from the importer, that test fails -- the table cannot quietly
// promise more or less than the reader delivers.

#pragma once

#include "PovLex.hpp"

#include <map>
#include <string>
#include <vector>

namespace makina {

enum class PovStatus {
    Supported,            ///< the importer represents this 1:1
    Ignored,              ///< read and deliberately dropped; the geometry is unaffected
    UnsupportedShape,     ///< a shape this model has no form for -- the picture would differ
    UnsupportedLanguage,  ///< a language construct the reader does not evaluate
    UnsupportedOther,     ///< appearance or feature that would change the picture
};

struct PovSurveyItem {
    std::string name;
    int         count = 0;
    int         firstLine = 0;
    PovStatus   status = PovStatus::Supported;
    std::string note;
};

struct PovSurveyResult {
    std::vector<PovSurveyItem> items;
    /// True when nothing unsupported was seen: povImport() will read this file whole.
    bool clean = true;
};

namespace detail {

struct PovWordClass {
    const char* name;
    PovStatus   status;
    const char* note;
};

/// Every word the survey has an opinion about. Grouped by status, not alphabet, because the
/// groups are the maintenance unit: a new shape lands in one of exactly two rows depending on
/// whether the importer learned it too, and the cross-check in pov_import_check catches the
/// mismatch either way.
inline const std::vector<PovWordClass>& povWordTable() {
    static const std::vector<PovWordClass> kTable = {
        // Geometry the model holds, 1:1.
        {"sphere", PovStatus::Supported, ""},
        {"box", PovStatus::Supported, ""},
        {"cylinder", PovStatus::Supported, ""},
        {"cone", PovStatus::Supported, ""},
        {"torus", PovStatus::Supported, ""},
        {"plane", PovStatus::Supported, ""},
        {"disc", PovStatus::Supported, ""},
        {"triangle", PovStatus::Supported, ""},
        {"union", PovStatus::Supported, ""},
        {"merge", PovStatus::Supported, ""},
        {"difference", PovStatus::Supported, ""},
        {"intersection", PovStatus::Supported, ""},
        {"translate", PovStatus::Supported, ""},
        {"rotate", PovStatus::Supported, ""},
        {"scale", PovStatus::Supported, ""},
        {"object", PovStatus::Supported, ""},
        // Appearance the renderer reproduces (measured against POV, docs/VERIFICATION.md).
        {"pigment", PovStatus::Supported, ""},
        {"texture", PovStatus::Supported, ""},
        {"finish", PovStatus::Supported, ""},
        {"color", PovStatus::Supported, ""},
        {"rgb", PovStatus::Supported, ""},
        {"srgb", PovStatus::Supported, "display values, decoded to linear"},
        {"color_map", PovStatus::Supported, "two stops; more are refused, not truncated"},
        {"checker", PovStatus::Supported, ""},
        {"gradient", PovStatus::Supported, ""},
        {"radial", PovStatus::Supported, ""},
        {"interior", PovStatus::Supported, "ior"},
        {"blob", PovStatus::Supported, "sphere and cylinder components"},
        {"camera", PovStatus::Supported, "perspective / orthographic / fisheye / ultra_wide_angle / panoramic"},
        {"light_source", PovStatus::Supported, "point lights; softness is out of scope by policy"},
        // Language the reader evaluates.
        {"#declare", PovStatus::Supported, "numbers, vectors, textures, transforms, objects"},
        {"#local", PovStatus::Supported, ""},
        {"#macro", PovStatus::Supported, "expanded by token substitution at the call"},
        {"#include", PovStatus::Supported, "read relative to the including file"},
        {"#version", PovStatus::Ignored, ""},
        // Read and dropped on purpose: none of these move a surface.
        {"global_settings", PovStatus::Ignored, "gamma, radiosity and friends tune POV's solver"},
        {"background", PovStatus::Ignored, ""},
        {"fog", PovStatus::Ignored, ""},
        {"sky_sphere", PovStatus::Ignored, ""},
        // Shapes the model has no form for. Mirrors the importer's refusal table; the
        // cross-check keeps the two lists honest.
        {"sor", PovStatus::UnsupportedShape, "a spline revolved about an axis"},
        {"lathe", PovStatus::UnsupportedShape, "a spline revolved about an axis"},
        {"prism", PovStatus::UnsupportedShape, "a spline swept along an axis"},
        {"sphere_sweep", PovStatus::UnsupportedShape, "a sphere dragged along a spline"},
        {"superellipsoid", PovStatus::UnsupportedShape, "an implicit surface with two exponents"},
        {"isosurface", PovStatus::UnsupportedShape, "an implicit surface given by a function"},
        {"parametric", PovStatus::UnsupportedShape, "a surface given by two parameters"},
        {"height_field", PovStatus::UnsupportedShape, "a surface read from an image"},
        {"julia_fractal", PovStatus::UnsupportedShape, "a fractal"},
        {"mesh", PovStatus::UnsupportedShape, "a triangle mesh"},
        {"mesh2", PovStatus::UnsupportedShape, "a triangle mesh"},
        {"polygon", PovStatus::UnsupportedShape, "a flat outline with any number of sides"},
        {"text", PovStatus::UnsupportedShape, "glyphs from a font"},
        {"bicubic_patch", PovStatus::UnsupportedShape, "a bicubic patch"},
        // Language the reader does not evaluate.
        {"rand", PovStatus::UnsupportedLanguage,
         "POV's own generator; only its measured stream would match"},
        {"seed", PovStatus::UnsupportedLanguage, ""},
        {"#while", PovStatus::UnsupportedLanguage, ""},
        {"#for", PovStatus::UnsupportedLanguage, ""},
        {"#if", PovStatus::UnsupportedLanguage, ""},
        {"#ifdef", PovStatus::UnsupportedLanguage, ""},
        {"#ifndef", PovStatus::UnsupportedLanguage, ""},
        {"#switch", PovStatus::UnsupportedLanguage, ""},
        {"#function", PovStatus::UnsupportedLanguage, ""},
        // Appearance that would change the picture if dropped silently.
        {"normal", PovStatus::UnsupportedOther, "surface perturbation (bumps etc.)"},
        {"media", PovStatus::UnsupportedOther, "participating media"},
        {"photons", PovStatus::UnsupportedOther, ""},
        {"matrix", PovStatus::UnsupportedOther, "a full 3x4 transform; only T/R/S are held"},
    };
    return kTable;
}

}  // namespace detail

/// Walks the whole file and reports every recognised construct with a status.
///
/// Token-level rather than parsed, so it works on files the parser would refuse at line one --
/// which are exactly the files whose report card is wanted. The price is precision: `mesh` as a
/// #declare name would be counted as the shape. Real files do not do this; a survey is a scout,
/// and povImport() remains the authority on whether the file truly reads.
[[nodiscard]] inline PovSurveyResult povSurvey(const std::string& text) {
    PovSurveyResult out;
    std::map<std::string, PovSurveyItem> seen;

    const std::vector<detail::PovToken> tokens = detail::povTokenize(text);
    for (const detail::PovToken& t : tokens) {
        if (t.kind != detail::PovTokenKind::Word && t.kind != detail::PovTokenKind::Directive) {
            continue;
        }
        for (const detail::PovWordClass& w : detail::povWordTable()) {
            if (t.text != w.name) {
                continue;
            }
            PovSurveyItem& item = seen[t.text];
            if (item.count == 0) {
                item.name = t.text;
                item.firstLine = t.line;
                item.status = w.status;
                item.note = w.note;
            }
            ++item.count;
            if (w.status != PovStatus::Supported && w.status != PovStatus::Ignored) {
                out.clean = false;
            }
            break;
        }
    }
    for (const auto& kv : seen) {
        out.items.push_back(kv.second);
    }
    return out;
}

}  // namespace makina
