// Joints and motion (D-15): what the maths says, without a second renderer.
//
// A Joint is Translate(pivot) Rotate Translate(-pivot), so its geometry is checked against a
// tree built from those three nodes -- the same field, two spellings. Motion is checked at its
// keys (the value is the key), between them (linear midpoint, Catmull-Rom through the keys with
// its end tangents), outside them (held), and through the JSON round trip. The picture is held
// elsewhere: a scene sampled at a time is a still scene and goes through every existing gate.

#include <makina/Animation.hpp>
#include <makina/Bounds.hpp>
#include <makina/Eval.hpp>
#include <makina/SceneJson.hpp>

#include <cmath>
#include <cstdio>
#include <string>

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

const char* kHead = "{\"format\":\"makina-scene\",\"version\":1,\"nextId\":20,\"materials\":[],";
const char* kBox = "{\"id\":9,\"op\":\"Box\",\"x1\":-0.1,\"y1\":0,\"z1\":-0.1,\"x2\":0.1,\"y2\":1,\"z2\":0.1}";

double at(const makina::Scene& s, double x, double y, double z) {
    const double p[3] = {x, y, z};
    return makina::eval(s, p);
}

void jointIsThreeTransforms() {
    std::printf("a Joint is Translate(pivot) Rotate Translate(-pivot)\n");
    // A bar from y=0 to 1, hinged at its top (0,1,0) and swung 90 degrees about Z.
    const std::string joint = std::string(kHead) +
        "\"root\":{\"id\":1,\"op\":\"SceneRoot\",\"children\":[{\"id\":2,\"op\":\"Joint\","
        "\"pivotX\":0,\"pivotY\":1,\"pivotZ\":0,\"degree\":90,\"axis\":\"Z\",\"children\":[" +
        kBox + "]}]}}";
    const std::string spelled = std::string(kHead) +
        "\"root\":{\"id\":1,\"op\":\"SceneRoot\",\"children\":[{\"id\":2,\"op\":\"Translate\","
        "\"x\":0,\"y\":1,\"z\":0,\"children\":[{\"id\":3,\"op\":\"Rotate\",\"degree\":90,\"axis\":"
        "\"Z\",\"children\":[{\"id\":4,\"op\":\"Translate\",\"x\":0,\"y\":-1,\"z\":0,\"children\":[" +
        kBox + "]}]}]}]}}";
    const makina::Scene a = makina::parseScene(joint);
    const makina::Scene b = makina::parseScene(spelled);
    double worst = 0.0;
    for (int i = 0; i < 9; ++i) {
        for (int j = 0; j < 9; ++j) {
            for (int k = 0; k < 5; ++k) {
                const double x = -1.5 + 3.0 * i / 8, y = -0.5 + 2.0 * j / 8, z = -0.5 + 1.0 * k / 4;
                const double d = std::fabs(at(a, x, y, z) - at(b, x, y, z));
                if (d > worst) {
                    worst = d;
                }
            }
        }
    }
    std::printf("    worst difference against the three-node spelling %.2e\n", worst);
    check(worst < 1e-9, "a Joint should evaluate exactly as the three transforms it stands for");
    // And it did swing: a right-handed 90 about Z takes (x, y) to (-y, x) about the pivot, so
    // the bar that stood on y in [0, 1] now lies along x in [0, 1] at y = 1. Sampled inside the
    // faces, not on them, where the field is exactly 0.
    check(at(a, 0.5, 1.0, 0.0) < 0, "the swung bar should now cover (0.5, 1, 0)");
    check(at(a, 0.0, 0.5, 0.0) > 0, "the swung bar should have left the line it stood on");
    // The pivot stays put: just beside it, the bar is still there.
    check(at(a, 0.02, 1.0, 0.0) < 0, "the pivot end stays inside");
}

void interpolation() {
    std::printf("tracks: keys, midpoints, ends\n");
    makina::Scene s = makina::parseScene(std::string(kHead) +
        "\"root\":{\"id\":1,\"op\":\"SceneRoot\",\"children\":[{\"id\":2,\"op\":\"Joint\","
        "\"pivotX\":0,\"pivotY\":1,\"pivotZ\":0,\"degree\":0,\"axis\":\"Z\",\"children\":[" +
        kBox + "]}]}}");
    // degree is params[3] of a Joint.
    check(makina::setKey(s, 2, 3, 0.0f, 0.0f), "first key");
    check(makina::setKey(s, 2, 3, 1.0f, 90.0f), "second key");
    check(makina::setKey(s, 2, 3, 2.0f, 0.0f), "third key");
    check(s.tracks.count == 1 && s.tracks[0].keyCount == 3, "one track of three keys");
    check(makina::animationLength(s) == 2.0f, "the motion is 2 seconds long");

    const auto deg = [&](float t) { return makina::sampleAt(s, t).nodes[1].params[3]; };
    check(deg(0.0f) == 0.0f && deg(1.0f) == 90.0f && deg(2.0f) == 0.0f, "on a key, the key");
    check(std::fabs(deg(0.5f) - 45.0f) < 1e-5f, "linear midpoint");
    check(deg(-1.0f) == 0.0f && deg(5.0f) == 0.0f, "held flat outside the keys");
    // Keys arrive out of order and land sorted; a repeated time replaces.
    check(makina::setKey(s, 2, 3, 0.5f, 10.0f), "insert between");
    check(s.tracks[0].keyCount == 4 && s.tracks[0].time[1] == 0.5f, "keys stay sorted");
    check(makina::setKey(s, 2, 3, 0.5f, 20.0f) && s.tracks[0].keyCount == 4 &&
              s.tracks[0].value[1] == 20.0f,
          "a key at an existing time replaces it");

    // Catmull-Rom passes through every key and is smooth across the middle one: the value just
    // before and just after a key differ by about the same amount (C1).
    s.tracks[0].interp = static_cast<std::uint8_t>(makina::TrackInterp::CatmullRom);
    check(deg(1.0f) == 90.0f, "catmull-rom passes through the key");
    const float before = deg(1.0f) - deg(0.99f), after = deg(1.01f) - deg(1.0f);
    check(std::fabs(before - after) < 0.5f, "catmull-rom is smooth across a key (C1)");
    // The scene itself is untouched by sampling.
    check(s.nodes[1].params[3] == 0.0f, "sampleAt returns a copy; the scene keeps its rest pose");
}

void jsonRoundTrip() {
    std::printf("tracks survive the JSON round trip by name\n");
    makina::Scene s = makina::parseScene(std::string(kHead) +
        "\"root\":{\"id\":1,\"op\":\"SceneRoot\",\"children\":[{\"id\":2,\"op\":\"Joint\","
        "\"pivotX\":0,\"pivotY\":1,\"pivotZ\":0,\"degree\":0,\"axis\":\"Z\",\"children\":[" +
        kBox + "]}]}}");
    makina::setKey(s, 2, 3, 0.0f, 0.0f, makina::TrackInterp::CatmullRom);
    makina::setKey(s, 2, 3, 1.5f, 60.0f, makina::TrackInterp::CatmullRom);
    const std::string json = makina::writeScene(s);
    check(json.find("\"animation\"") != std::string::npos, "motion is written");
    check(json.find("\"param\": \"degree\"") != std::string::npos, "the parameter is written by name");
    const makina::Scene back = makina::parseScene(json);
    check(back.tracks.count == 1 && back.tracks[0].keyCount == 2 &&
              back.tracks[0].paramIndex == 3 && back.tracks[0].time[1] == 1.5f &&
              back.tracks[0].value[1] == 60.0f &&
              static_cast<makina::TrackInterp>(back.tracks[0].interp) ==
                  makina::TrackInterp::CatmullRom,
          "the track comes back whole");
    // A still scene writes no animation at all.
    const makina::Scene still = makina::parseScene(std::string(kHead) +
        "\"root\":{\"id\":1,\"op\":\"SceneRoot\",\"children\":[" + kBox + "]}}");
    check(makina::writeScene(still).find("animation") == std::string::npos,
          "a still scene stays free of an animation section");
    // A track naming a parameter the op does not have is refused, not guessed.
    bool refused = false;
    try {
        (void)makina::parseScene(std::string(kHead) +
            "\"animation\":[{\"node\":9,\"param\":\"radius\",\"keys\":[[0,1]]}],"
            "\"root\":{\"id\":1,\"op\":\"SceneRoot\",\"children\":[" + kBox + "]}}");
    } catch (const makina::SceneJsonError&) {
        refused = true;
    }
    check(refused, "a track on a parameter the node lacks is refused");
}

}  // namespace

int main() {
    std::printf("makina-core animation check\n\n");
    jointIsThreeTransforms();
    interpolation();
    jsonRoundTrip();
    std::printf("\n%d checks", checks);
    if (failures > 0) {
        std::printf(", %d FAILED\n", failures);
        return 1;
    }
    std::printf(", all passed\n");
    return 0;
}
