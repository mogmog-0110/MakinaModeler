// Where Makina deliberately disagrees with Grasp3D, gathered in one place.
//
// A divergence that lives as a scattered `if` is a divergence nobody can audit. Each field here
// names one, defaults to Makina's answer, and can be flipped back for the comparison tests --
// which need the reference's answer or the comparison would differ everywhere for a known reason
// and say nothing about the code being compared.
//
// Both entries are PLAN.md D-11 category B: the output changes. PORT_STATUS.md 4.1 and 4.3 record
// why, and each is checked rather than merely asserted.

#pragma once

namespace makina {

struct Fidelity {
    /// Difference and Intersection get a boolean-aware box instead of the union of their children:
    /// A-B cannot reach outside A, and A&B cannot reach outside either operand. Grasp3D's own
    /// comment calls its estimate conservative; this makes it tight (Bounds.hpp).
    bool tightBounds = true;

    /// A Label's children are geometry.
    ///
    /// Grasp3D does not agree with itself here. Its GL view descends into a Label and draws what
    /// is under it, its POV export writes it out, and its bounds count it -- three consumers
    /// treating a Label as an annotation attached to real parts. Only SceneSdf.evalNode disagrees:
    /// it returns empty for a Label and never descends, so the evaluator alone cannot see them.
    /// pettobotoru.gsf settles which reading is intended -- one of the bottle's rings sits under a
    /// Label, and it is visibly there in the editor.
    ///
    /// A Label child of a Difference or an Intersection is still skipped as an *operand*. That is
    /// Grasp3D's rule and it is the right one: a comment is not the body, and it is not a blade.
    bool labelsAreGeometry = true;
};

/// Grasp3D's own answers, for the reference comparison.
inline constexpr Fidelity kGrasp3D{false, false};

}  // namespace makina
