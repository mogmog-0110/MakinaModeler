// Checks the transform session and where its numbers land in the tree.
//
// The plan calls typing the number the thing that matters most, because CSG is dimension-driven:
// "five along X" has to mean exactly five. So the checks are about exactness and about the states
// around it -- what happens when the mouse moves after a number is typed, what a half-typed minus
// means, what cancelling leaves behind.
//
// The second half is about the tree. A primitive has no position; placement is a Translate above
// it. Moving something therefore either edits the transform it has or gives it one, and the wrong
// answer there grows a chain of Translates, one per drag, until nobody can read the model.

#include <makina/SceneJson.hpp>
#include <makina/Transform.hpp>

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        std::printf("    FAIL  %s\n", what.c_str());
        ++failures;
    }
}

void checkNear(double got, double want, double tol, const std::string& what) {
    ++checks;
    if (std::fabs(got - want) > tol) {
        std::printf("    FAIL  %s: got %.9g want %.9g\n", what.c_str(), got, want);
        ++failures;
    }
}

constexpr const char* kScene = R"({
  "format": "makina-scene",
  "version": 1,
  "nextId": 20,
  "materials": [],
  "root": { "op": "SceneRoot", "id": 1, "name": "Scene", "children": [
    { "op": "Sphere", "id": 2, "name": "bare", "radius": 1.0 },
    { "op": "Translate", "id": 3, "name": "placed", "x": 4.0, "y": 0.0, "z": 0.0, "children": [
      { "op": "Box", "id": 4, "name": "cube", "x1": -0.5, "y1": -0.5, "z1": -0.5,
        "x2": 0.5, "y2": 0.5, "z2": 0.5 }
    ]},
    { "op": "Rotate", "id": 5, "name": "turned", "degree": 30.0, "axis": "Y", "children": [
      { "op": "Sphere", "id": 6, "name": "ball", "radius": 0.5 }
    ]}
  ]}
})";

void typedNumbersAreExact() {
    std::printf("a typed number is the number\n");

    makina::TransformSession t;
    t.begin(makina::TransformKind::Move);
    t.setAxis(makina::TransformAxis::X);
    for (char c : std::string("5")) {
        check(t.type(c), "a digit was refused");
    }
    checkNear(t.value(), 5.0, 1e-12, "typing 5 did not give 5");

    // The mouse must not touch it afterwards. Having typed the number, the hand is on the
    // keyboard, and a twitch turning 5 into 5.03 makes the typed value a suggestion.
    t.mouseDelta(0.9);
    checkNear(t.value(), 5.0, 1e-12, "the mouse moved a typed value");

    // Negatives and decimals.
    makina::TransformSession n;
    n.begin(makina::TransformKind::Move);
    for (char c : std::string("-2.5")) {
        check(n.type(c), std::string("'") + c + "' was refused");
    }
    checkNear(n.value(), -2.5, 1e-12, "-2.5 did not parse");

    // A leading point reads as 0.something rather than as an error.
    makina::TransformSession p;
    p.begin(makina::TransformKind::Move);
    p.type('.');
    p.type('5');
    checkNear(p.value(), 0.5, 1e-12, "a leading point did not become 0.5");

    // Letters and a second point are not part of the number.
    check(!p.type('q'), "a letter was taken as part of the number");
    check(!p.type('.'), "a second decimal point was accepted");

    // Backspacing everything hands control back to the mouse.
    makina::TransformSession b;
    b.begin(makina::TransformKind::Move);
    b.type('7');
    b.mouseDelta(0.4);
    checkNear(b.value(), 7.0, 1e-12, "the mouse moved a typed value");
    b.backspace();
    check(!b.typing(), "backspacing the last digit did not clear the buffer");
    b.mouseDelta(0.4);
    checkNear(b.value(), 0.4, 1e-12, "the mouse did not take over after backspacing");

    std::printf("    exact, signed, decimal, and the mouse stays out of it\n");
}

void theIdentityIsPerKind() {
    std::printf("an untouched session is the identity\n");

    // Confirming immediately must leave the model alone, and "leave alone" is 0 for a move and 1
    // for a scale. Sharing one default would collapse anything scaled by an accidental keypress.
    makina::TransformSession m;
    m.begin(makina::TransformKind::Move);
    checkNear(m.value(), 0.0, 1e-12, "a fresh move is not zero");

    makina::TransformSession s;
    s.begin(makina::TransformKind::Scale);
    checkNear(s.value(), 1.0, 1e-12, "a fresh scale is not one");

    // A half-typed minus is not a number yet, and must not read as zero.
    makina::TransformSession half;
    half.begin(makina::TransformKind::Scale);
    half.type('-');
    checkNear(half.value(), 1.0, 1e-12, "a lone minus collapsed a scale");

    std::printf("    move starts at 0, scale at 1, half-typed input is neither\n");
}

void axisAndSnapAndStatus() {
    std::printf("axis, snap and what the header says\n");

    makina::TransformSession t;
    t.begin(makina::TransformKind::Move);
    t.setAxis(makina::TransformAxis::Y);
    check(t.axis() == makina::TransformAxis::Y, "the axis did not take");
    // The same key again releases it. An axis you cannot get out of is worse than no axis.
    t.setAxis(makina::TransformAxis::Y);
    check(t.axis() == makina::TransformAxis::None, "pressing the same axis did not release it");

    t.setAxis(makina::TransformAxis::Z);
    t.mouseDelta(0.237);
    checkNear(t.value(), 0.237, 1e-12, "the mouse value is wrong");
    t.setSnap(true);
    checkNear(t.value(0.1), 0.2, 1e-12, "snapping did not round to the step");
    t.setSnap(false);
    checkNear(t.value(0.1), 0.237, 1e-12, "releasing snap did not restore the value");

    // The header has to say which axis and which number, and has to distinguish a typed value
    // from a dragged one -- they behave differently and the user needs to know which they are in.
    const std::string dragged = t.status();
    check(dragged.find("Move") != std::string::npos, "the status does not name the operation");
    check(dragged.find("Z") != std::string::npos, "the status does not name the axis");
    t.type('9');
    const std::string typed = t.status();
    check(typed.find("9") != std::string::npos, "the status does not show the typed number");
    check(typed.find("typed") != std::string::npos, "the status does not say it is typed");

    // Cancelling leaves nothing behind, because nothing was ever applied.
    t.cancel();
    check(!t.active(), "cancel did not end the session");
    check(t.status().empty(), "a cancelled session still reports a status");

    std::printf("    axis toggles, snap rounds, the header reads back\n");
}

void movingEditsOrCreatesATransform() {
    std::printf("a move lands in the right node\n");

    const makina::Scene s = makina::parseScene(kScene);

    // The node already has a Translate: its numbers change, and the tree does not grow.
    {
        const makina::EditResult r = makina::applyTransform(
            s, 3, makina::TransformKind::Move, makina::TransformAxis::X, 2.0);
        check(r.ok, "moving an existing Translate failed: " + r.why);
        check(r.scene.nodes.count == s.nodes.count, "moving an existing Translate grew the tree");
        const std::uint16_t at = makina::indexOfId(r.scene, 3);
        checkNear(r.scene.nodes[at].params[0], 6.0, 1e-5, "the translation did not accumulate");
    }

    // The node has none: it gains one, above it, in the same place among its siblings.
    {
        const makina::EditResult r = makina::applyTransform(
            s, 2, makina::TransformKind::Move, makina::TransformAxis::Y, 3.0);
        check(r.ok, "moving a bare primitive failed: " + r.why);
        check(r.scene.nodes.count == s.nodes.count + 1, "no transform was created");

        const std::uint16_t sphere = makina::indexOfId(r.scene, 2);
        check(sphere != makina::kNoChild, "the moved node disappeared");
        const std::uint16_t parent = r.scene.nodes[sphere].parent;
        check(static_cast<makina::Op>(r.scene.nodes[parent].op) == makina::Op::Translate,
              "the new parent is not a Translate");
        checkNear(r.scene.nodes[parent].params[1], 3.0, 1e-5, "the new Translate has the wrong y");

        // Still the first child of the root: appending instead would reorder the siblings, and
        // under a Difference the order is the geometry.
        check(r.scene.nodes[0].firstChild == parent, "the wrapper was not put back in place");

        // And moving again edits that same node rather than stacking another one.
        const makina::EditResult again = makina::applyTransform(
            r.scene, r.newId, makina::TransformKind::Move, makina::TransformAxis::Y, 1.0);
        check(again.ok, "the second move failed: " + again.why);
        check(again.scene.nodes.count == r.scene.nodes.count,
              "the second move stacked another Translate");
        checkNear(again.scene.nodes[makina::indexOfId(again.scene, r.newId)].params[1], 4.0, 1e-5,
                  "the second move did not accumulate");
    }

    std::printf("    existing transforms are edited, bare nodes gain one, in place\n");
}

void rotateAndScaleHaveTheirOwnRules() {
    std::printf("rotate and scale\n");

    const makina::Scene s = makina::parseScene(kScene);

    // A rotation with no axis is refused. Grasp3D's Rotate is single-axis, so "rotate by 30" has
    // no meaning yet, and picking one silently would turn a mis-key into a wrong model.
    check(!makina::applyTransform(s, 5, makina::TransformKind::Rotate, makina::TransformAxis::None,
                                  30.0)
               .ok,
          "a rotation with no axis was accepted");

    // Same axis as the existing Rotate: the angle accumulates.
    {
        const makina::EditResult r = makina::applyTransform(
            s, 5, makina::TransformKind::Rotate, makina::TransformAxis::Y, 15.0);
        check(r.ok, "rotating about the existing axis failed: " + r.why);
        check(r.scene.nodes.count == s.nodes.count, "rotating about the same axis grew the tree");
        checkNear(r.scene.nodes[makina::indexOfId(r.scene, 5)].params[0], 45.0, 1e-4,
                  "the angle did not accumulate");
    }

    // A different axis gets its own node. Adding Y degrees to a node that turns about X would
    // quietly change what the existing rotation meant.
    {
        const makina::EditResult r = makina::applyTransform(
            s, 5, makina::TransformKind::Rotate, makina::TransformAxis::X, 20.0);
        check(r.ok, "rotating about a new axis failed: " + r.why);
        check(r.scene.nodes.count == s.nodes.count + 1,
              "a rotation about a different axis reused the existing node");
        checkNear(r.scene.nodes[makina::indexOfId(r.scene, 5)].params[0], 30.0, 1e-4,
                  "the existing rotation was changed");
    }

    // Scale multiplies. Twice by two is four, and adding would make it three.
    {
        makina::EditResult r = makina::applyTransform(
            s, 2, makina::TransformKind::Scale, makina::TransformAxis::None, 2.0);
        check(r.ok, "scaling failed: " + r.why);
        const std::uint32_t scaleId = r.newId;
        r = makina::applyTransform(r.scene, scaleId, makina::TransformKind::Scale,
                                   makina::TransformAxis::None, 2.0);
        check(r.ok, "the second scale failed: " + r.why);
        const std::uint16_t at = makina::indexOfId(r.scene, scaleId);
        checkNear(r.scene.nodes[at].params[0], 4.0, 1e-5, "scale added instead of multiplying");
    }

    // A new Scale is 1 on the axes it does not touch, or the node collapses the model to nothing.
    {
        const makina::EditResult r = makina::applyTransform(
            s, 2, makina::TransformKind::Scale, makina::TransformAxis::X, 3.0);
        check(r.ok, "scaling on one axis failed: " + r.why);
        const std::uint16_t at = makina::indexOfId(r.scene, r.newId);
        checkNear(r.scene.nodes[at].params[0], 3.0, 1e-5, "the scaled axis is wrong");
        checkNear(r.scene.nodes[at].params[1], 1.0, 1e-5, "an untouched axis is not 1");
        checkNear(r.scene.nodes[at].params[2], 1.0, 1e-5, "an untouched axis is not 1");
    }

    std::printf("    axis required, angles accumulate per axis, scale multiplies\n");
}

void refusalsLeaveTheSceneAlone() {
    std::printf("refusals change nothing\n");

    const makina::Scene s = makina::parseScene(kScene);

    for (const makina::EditResult& r :
         {makina::applyTransform(s, 999, makina::TransformKind::Move, makina::TransformAxis::X, 1.0),
          makina::applyTransform(s, 1, makina::TransformKind::Move, makina::TransformAxis::X, 1.0),
          makina::applyTransform(s, 2, makina::TransformKind::Move, makina::TransformAxis::None,
                                 1.0),
          makina::applyTransform(s, 2, makina::TransformKind::None, makina::TransformAxis::X,
                                 1.0)}) {
        check(!r.ok, "a transform that should have been refused was accepted");
        check(!r.why.empty(), "a refusal gave no reason");
        check(r.scene.nodes.count == s.nodes.count, "a refused transform changed the scene");
    }

    std::printf("    unknown id, the root, no axis, no kind\n");
}

}  // namespace

int main() {
    std::printf("makina-core transforms\n\n");

    typedNumbersAreExact();
    theIdentityIsPerKind();
    axisAndSnapAndStatus();
    movingEditsOrCreatesATransform();
    rotateAndScaleHaveTheirOwnRules();
    refusalsLeaveTheSceneAlone();

    if (failures == 0) {
        std::printf("\ntransforms behave (%d checks)\n", checks);
        return 0;
    }
    std::printf("\n%d of %d checks FAILED\n", failures, checks);
    return 1;
}
