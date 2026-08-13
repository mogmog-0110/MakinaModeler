// The ray march against the ray trace, pixel for pixel this time.
//
// silhouette_compare asks whether the two renderers draw the same *shape*. That check is worth a
// lot -- it catches geometry, transforms, camera and handedness in one number -- but it is blind
// to everything the surface does. Two renderers can agree on every outline and disagree about
// every material, and until now nothing here would have noticed.
//
// This is the other half, and it only became possible once the renderer computed POV's finish{}
// and POV's pigment patterns rather than something chosen to look good (scene_finish.hlsl).
//
// What it reports, and why not just one number:
//
//   mean       what the two disagree by on average, over pixels where either drew something. The
//              number to watch: a shading change moves it and nothing else does.
//   p99        the 99th percentile. A handful of edge pixels will always differ -- one renderer
//              antialiases a boundary the other steps over -- and a max would be entirely those.
//   max        kept anyway, with its position, because a single wildly wrong pixel is a real
//              failure and hiding it inside a percentile would be the wrong kind of tidy.
//
// Background pixels where both drew nothing are excluded. Including them would flood the average
// with agreement about black and make any real difference look small.

#include "image_out.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb;   ///< 3 bytes per pixel, top row first
};

/// Reads the 24-bit BMPs both renderers write. Not a general reader: it refuses anything else
/// rather than guessing, because a silently misread image compares as noise.
bool readBmp(const std::string& path, Image& out, std::string& why) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        why = "could not open '" + path + "'";
        return false;
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 54 || bytes[0] != 'B' || bytes[1] != 'M') {
        why = "'" + path + "' is not a BMP";
        return false;
    }

    std::uint32_t dataOffset = 0;
    std::int32_t width = 0, height = 0;
    std::uint16_t bpp = 0;
    std::uint32_t compression = 0;
    std::memcpy(&dataOffset, bytes.data() + 10, 4);
    std::memcpy(&width, bytes.data() + 18, 4);
    std::memcpy(&height, bytes.data() + 22, 4);
    std::memcpy(&bpp, bytes.data() + 28, 2);
    std::memcpy(&compression, bytes.data() + 30, 4);

    if (bpp != 24 || compression != 0) {
        why = "'" + path + "' is " + std::to_string(bpp) + "-bit compression " +
              std::to_string(compression) + "; this reads 24-bit uncompressed only";
        return false;
    }

    const bool bottomUp = height > 0;
    const int h = height > 0 ? height : -height;
    const int rowBytes = ((width * 3 + 3) / 4) * 4;
    if (static_cast<std::size_t>(dataOffset) + static_cast<std::size_t>(rowBytes) * h >
        bytes.size()) {
        why = "'" + path + "' is shorter than its header says";
        return false;
    }

    out.width = width;
    out.height = h;
    out.rgb.assign(static_cast<std::size_t>(width) * h * 3, 0);
    for (int y = 0; y < h; ++y) {
        // BMP rows run bottom-up unless the height is negative, and the two writers here do not
        // agree on that. Normalised to top-down so the comparison never depends on which wrote it.
        const int src = bottomUp ? (h - 1 - y) : y;
        const char* row = bytes.data() + dataOffset + static_cast<std::size_t>(src) * rowBytes;
        for (int x = 0; x < width; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * width + x) * 3;
            out.rgb[at + 0] = static_cast<std::uint8_t>(row[x * 3 + 2]);   // BMP is BGR
            out.rgb[at + 1] = static_cast<std::uint8_t>(row[x * 3 + 1]);
            out.rgb[at + 2] = static_cast<std::uint8_t>(row[x * 3 + 0]);
        }
    }
    return true;
}

struct Report {
    double mean = 0.0;
    int    p99 = 0;
    int    max = 0;
    int    maxX = 0;
    int    maxY = 0;
    long   counted = 0;
    long   strong = 0;   ///< pixels differing by more than kStrong
};

/// The largest of the three channel differences, which is what a person sees as "these are not the
/// same color" -- averaging the channels would let a strong shift in one hide behind two matches.
int pixelDifference(const std::uint8_t* a, const std::uint8_t* b) {
    int worst = 0;
    for (int c = 0; c < 3; ++c) {
        worst = std::max(worst, std::abs(static_cast<int>(a[c]) - static_cast<int>(b[c])));
    }
    return worst;
}

bool anythingDrawn(const std::uint8_t* p) {
    // Both renderers put the background at black in this mode, so a pixel counts when either
    // renderer put something there.
    return p[0] > 6 || p[1] > 6 || p[2] > 6;
}

Report compare(const Image& a, const Image& b, int strongThreshold,
               std::vector<std::uint8_t>& diffOut) {
    Report r;
    std::vector<int> all;
    all.reserve(static_cast<std::size_t>(a.width) * a.height);
    diffOut.assign(static_cast<std::size_t>(a.width) * a.height * 3, 0);

    double sum = 0.0;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * a.width + x) * 3;
            const std::uint8_t* pa = a.rgb.data() + at;
            const std::uint8_t* pb = b.rgb.data() + at;
            if (!anythingDrawn(pa) && !anythingDrawn(pb)) {
                continue;
            }
            const int d = pixelDifference(pa, pb);
            sum += d;
            all.push_back(d);
            if (d > strongThreshold) {
                ++r.strong;
            }
            if (d > r.max) {
                r.max = d;
                r.maxX = x;
                r.maxY = y;
            }
            // Amplified so a 5-level difference is visible; the numbers are what is judged on.
            const std::uint8_t v = static_cast<std::uint8_t>(std::min(255, d * 8));
            diffOut[at + 0] = v;
            diffOut[at + 1] = v;
            diffOut[at + 2] = v;
        }
    }

    r.counted = static_cast<long>(all.size());
    if (all.empty()) {
        return r;
    }
    r.mean = sum / static_cast<double>(all.size());
    std::sort(all.begin(), all.end());
    r.p99 = all[static_cast<std::size_t>(all.size() * 99 / 100)];
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    // Two gates, and the second is not a percentile, which was the first attempt.
    //
    // A hard-edged pattern breaks percentiles. Where a checker changes square the two colors are
    // 200 levels apart, so a half-pixel disagreement about where the surface is -- which is
    // guaranteed, one renderer marches and the other solves -- puts the full 200 into that pixel.
    // Measured on the checker fixture: 1,471 pixels of 146,210 exceed 40, of which 527 sit on the
    // silhouette and the rest on square boundaries. The p99 therefore reads 41 on a scene whose
    // mean is 4.7 and whose pattern lines up exactly.
    //
    // So the second gate counts *how many* pixels disagree strongly rather than asking how bad the
    // 99th is. A shading error moves the mean; a pattern that has slipped moves the share.
    //
    // The defaults are what two *different* renderers can be held to. Two paths through the same
    // shading, which is what --mean is for, have to agree far more closely than that, and a limit
    // loose enough for POV would pass them however far apart they were.
    double      meanLimit = 6.0;
    double      strongShareLimit = 0.02;
    std::string title = "makina ray march vs POV-Ray, pixel for pixel";
    std::string agreed = "the two renderers put the same colors on the same surfaces";
    // Inverted, for the checks that exist to prove a difference is there.
    //
    // "The two agree" is worth nothing when both are blank, and a feature that stopped reaching the
    // picture would pass every comparison in this project by agreeing with a copy of itself. So a
    // check that a material actually changes the image asks for the opposite verdict.
    bool mustDiffer = false;
    constexpr int kStrong = 40;

    int first = 1;
    while (first + 1 < argc && argv[first][0] == '-') {
        const std::string flag = argv[first];
        if (flag == "--mean") {
            meanLimit = std::atof(argv[first + 1]);
        } else if (flag == "--share") {
            strongShareLimit = std::atof(argv[first + 1]);
        } else if (flag == "--title") {
            title = argv[first + 1];
        } else if (flag == "--agreed") {
            agreed = argv[first + 1];
        } else if (flag == "--differ") {
            mustDiffer = true;
            // No value of its own; the loop below steps by two.
            --first;
        } else {
            std::fprintf(stderr, "color_compare: unknown option '%s'\n", flag.c_str());
            return 2;
        }
        first += 2;
    }

    // Pairs, the same shape silhouette_compare takes, so one batch file can drive both.
    if (argc - first < 2 || ((argc - first) % 2) != 0) {
        std::fprintf(stderr, "usage: color_compare [--mean <x>] [--share <x>] [--title <text>] "
                             "[--agreed <text>] <a.bmp> <b.bmp> [<a.bmp> <b.bmp>]\n");
        return 2;
    }

    std::printf("%s\n\n", title.c_str());

    int failures = 0;
    for (int i = first; i + 1 < argc; i += 2) {
        Image march, pov;
        std::string why;
        if (!readBmp(argv[i], march, why) || !readBmp(argv[i + 1], pov, why)) {
            std::printf("    FAIL  %s\n", why.c_str());
            ++failures;
            continue;
        }
        if (march.width != pov.width || march.height != pov.height) {
            std::printf("    FAIL  %s is %dx%d and %s is %dx%d\n", argv[i], march.width,
                        march.height, argv[i + 1], pov.width, pov.height);
            ++failures;
            continue;
        }

        std::vector<std::uint8_t> diff;
        const Report r = compare(march, pov, kStrong, diff);
        const double share = r.counted > 0 ? static_cast<double>(r.strong) / r.counted : 0.0;
        std::printf("%s\n", argv[i]);
        std::printf("    mean %.2f, %.2f%% over %d, p99 %d, max %d at (%d,%d), %ld pixels\n",
                    r.mean, share * 100.0, kStrong, r.p99, r.max, r.maxX, r.maxY, r.counted);

        std::string diffPath(argv[i]);
        const std::size_t dot = diffPath.find_last_of('.');
        diffPath = (dot == std::string::npos ? diffPath : diffPath.substr(0, dot)) + "_cdiff.bmp";
        std::string err;
        if (!spike::writeBmp(diffPath, diff.data(), march.width, march.height, march.width * 3,
                             err)) {
            std::printf("    (no diff image: %s)\n", err.c_str());
        }

        if (r.counted == 0) {
            std::printf("    FAIL  neither renderer drew anything; there is nothing to compare\n");
            ++failures;
        } else if (mustDiffer) {
            if (r.mean <= meanLimit && share <= strongShareLimit) {
                std::printf("    FAIL  the two are the same picture, and should not be\n");
                ++failures;
            } else {
                std::printf("    differs, as it must -> %s\n", diffPath.c_str());
            }
        } else if (r.mean > meanLimit || share > strongShareLimit) {
            std::printf("    FAIL  past mean %.1f or %.0f%% strongly differing\n", meanLimit,
                        strongShareLimit * 100.0);
            ++failures;
        } else {
            std::printf("    agrees -> %s\n", diffPath.c_str());
        }
    }

    std::printf("\n");
    if (failures == 0) {
        // The caller's own words when it gave any: "the two renderers" is wrong for a comparison
        // between two evaluators of the same renderer, and a line that says the wrong thing on
        // success is read more often than one that says it on failure.
        std::printf("%s\n", agreed.c_str());
        return 0;
    }
    std::printf("%d comparison(s) FAILED\n", failures);
    return 1;
}
