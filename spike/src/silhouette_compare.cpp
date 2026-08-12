// Compares two silhouette masks: the SDF ray-march's and POV-Ray's.
//
// This is Phase 5's third leg, and the only one that puts the whole chain under one number. A
// silhouette depends on the geometry, the transforms, the camera *and* the handedness convention;
// a picture also depends on shading, tone mapping and light units, which the two renderers were
// never going to agree on and which would hide a real disagreement in a sea of tuning noise.
//
// Reported as intersection over union. Anything short of the whole silhouette shows up: a boolean
// taken the wrong way round, a rotation with the wrong sign, a mirrored scene -- the last of which
// scores near zero and is precisely the failure a right-handed scene written for a left-handed
// renderer produces.
//
// The edge is expected to disagree by roughly its own perimeter: POV-Ray decides a pixel by where
// the surface crosses it, the ray march by where its epsilon lands. `--tolerance` is therefore in
// units of the silhouette's perimeter, not of its area -- a fixed pixel count would be far too
// tight on a large model and meaningless on a small one.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct Image {
    int                       width = 0;
    int                       height = 0;
    std::vector<std::uint8_t> luma;   ///< row 0 is the top row
};

std::uint32_t readU32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::int32_t readI32(const std::uint8_t* p) {
    return static_cast<std::int32_t>(readU32(p));
}

/// Uncompressed 24- and 32-bit BMP only, which is what both producers write.
///
/// A negative height means the rows are stored top-down; POV-Ray and the spike's own writer
/// disagree about this, and getting it wrong flips one image and scores a near-zero IoU that looks
/// exactly like a handedness bug.
Image readBmp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("could not open '" + path + "'");
    }
    const std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
    if (raw.size() < 54 || raw[0] != 'B' || raw[1] != 'M') {
        throw std::runtime_error("'" + path + "' is not a BMP");
    }

    const std::uint32_t dataOffset = readU32(&raw[10]);
    const std::int32_t width = readI32(&raw[18]);
    const std::int32_t rawHeight = readI32(&raw[22]);
    const int bpp = raw[28] | (raw[29] << 8);
    const std::uint32_t compression = readU32(&raw[30]);

    if (compression != 0 || (bpp != 24 && bpp != 32)) {
        throw std::runtime_error("'" + path + "' is not an uncompressed 24- or 32-bit BMP (bpp " +
                                 std::to_string(bpp) + ", compression " +
                                 std::to_string(compression) + ")");
    }

    const bool topDown = rawHeight < 0;
    const int height = topDown ? -rawHeight : rawHeight;
    const int bytesPerPixel = bpp / 8;
    const std::size_t stride = (static_cast<std::size_t>(width) * bytesPerPixel + 3u) & ~std::size_t(3);

    if (raw.size() < dataOffset + stride * static_cast<std::size_t>(height)) {
        throw std::runtime_error("'" + path + "' is truncated");
    }

    Image img;
    img.width = width;
    img.height = height;
    img.luma.resize(static_cast<std::size_t>(width) * height);

    for (int y = 0; y < height; ++y) {
        const int srcRow = topDown ? y : height - 1 - y;
        const std::uint8_t* row = &raw[dataOffset + stride * static_cast<std::size_t>(srcRow)];
        for (int x = 0; x < width; ++x) {
            const std::uint8_t* px = row + static_cast<std::size_t>(x) * bytesPerPixel;
            // BGR order. Plain average: the mask is white or black, so weighting buys nothing.
            const int v = (px[0] + px[1] + px[2]) / 3;
            img.luma[static_cast<std::size_t>(y) * width + x] = static_cast<std::uint8_t>(v);
        }
    }
    return img;
}

void writeMaskBmp(const std::string& path, int width, int height,
                  const std::vector<std::uint8_t>& rgb) {
    const std::size_t stride = (static_cast<std::size_t>(width) * 3u + 3u) & ~std::size_t(3);
    const std::size_t pixels = stride * static_cast<std::size_t>(height);
    std::vector<std::uint8_t> out(54 + pixels, 0);

    auto put32 = [&out](std::size_t at, std::uint32_t v) {
        out[at] = static_cast<std::uint8_t>(v);
        out[at + 1] = static_cast<std::uint8_t>(v >> 8);
        out[at + 2] = static_cast<std::uint8_t>(v >> 16);
        out[at + 3] = static_cast<std::uint8_t>(v >> 24);
    };

    out[0] = 'B';
    out[1] = 'M';
    put32(2, static_cast<std::uint32_t>(out.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, static_cast<std::uint32_t>(width));
    put32(22, static_cast<std::uint32_t>(height));
    out[26] = 1;
    out[28] = 24;
    put32(34, static_cast<std::uint32_t>(pixels));

    for (int y = 0; y < height; ++y) {
        std::uint8_t* row = &out[54 + stride * static_cast<std::size_t>(height - 1 - y)];
        for (int x = 0; x < width; ++x) {
            const std::size_t src = (static_cast<std::size_t>(y) * width + x) * 3;
            row[x * 3 + 0] = rgb[src + 2];
            row[x * 3 + 1] = rgb[src + 1];
            row[x * 3 + 2] = rgb[src + 0];
        }
    }

    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
}

/// Boundary length of a mask, counted as pixels that have a differing 4-neighbour.
///
/// The tolerance is expressed against this rather than against the area: the two renderers can
/// only disagree at the edge, so the perimeter is the size of the region where disagreement is
/// legitimate. An area-relative tolerance would be lax on a blob and impossible on a lattice.
long perimeterOf(const std::vector<std::uint8_t>& m, int w, int h) {
    long n = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            if (m[i] == 0) {
                continue;
            }
            const bool edge = x == 0 || y == 0 || x == w - 1 || y == h - 1 ||
                              m[i - 1] == 0 || m[i + 1] == 0 ||
                              m[i - w] == 0 || m[i + w] == 0;
            if (edge) {
                ++n;
            }
        }
    }
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    // Allowed symmetric difference, as a multiple of the silhouette's perimeter. 2.0 means "up to
    // two pixels of edge disagreement all the way round", which is what differing edge rules cost.
    double tolerance = 2.0;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--tolerance" && i + 1 < argc) {
            tolerance = std::atof(argv[++i]);
        } else {
            args.push_back(a);
        }
    }

    if (args.size() < 2 || args.size() % 2 != 0) {
        std::fprintf(stderr, "usage: silhouette_compare [--tolerance N] <a.bmp> <b.bmp> "
                             "[more pairs ...]\n");
        return 2;
    }

    std::printf("Makina SDF vs POV-Ray, silhouette agreement\n\n");

    int failures = 0;
    for (std::size_t pair = 0; pair + 1 < args.size(); pair += 2) {
        const std::string& aPath = args[pair];
        const std::string& bPath = args[pair + 1];
        try {
            const Image a = readBmp(aPath);
            const Image b = readBmp(bPath);
            if (a.width != b.width || a.height != b.height) {
                std::printf("%s\n    FAIL  %dx%d vs %dx%d\n", aPath.c_str(), a.width, a.height,
                            b.width, b.height);
                ++failures;
                continue;
            }

            std::vector<std::uint8_t> ma(a.luma.size()), mb(a.luma.size());
            for (std::size_t i = 0; i < a.luma.size(); ++i) {
                ma[i] = a.luma[i] > 127 ? 1 : 0;
                mb[i] = b.luma[i] > 127 ? 1 : 0;
            }

            long inter = 0, uni = 0, onlyA = 0, onlyB = 0;
            for (std::size_t i = 0; i < ma.size(); ++i) {
                if (ma[i] && mb[i]) ++inter;
                if (ma[i] || mb[i]) ++uni;
                if (ma[i] && !mb[i]) ++onlyA;
                if (!ma[i] && mb[i]) ++onlyB;
            }

            if (uni == 0) {
                std::printf("%s\n    FAIL  both silhouettes are empty; nothing was compared\n",
                            aPath.c_str());
                ++failures;
                continue;
            }

            const long perim = perimeterOf(ma, a.width, a.height);
            const long diff = onlyA + onlyB;
            const double iou = static_cast<double>(inter) / static_cast<double>(uni);
            const double allowed = tolerance * static_cast<double>(perim);

            std::vector<std::uint8_t> rgb(ma.size() * 3, 0);
            for (std::size_t i = 0; i < ma.size(); ++i) {
                rgb[i * 3 + 0] = static_cast<std::uint8_t>((ma[i] && !mb[i]) ? 255 : 0);
                rgb[i * 3 + 1] = static_cast<std::uint8_t>((ma[i] && mb[i]) ? 90 : 0);
                rgb[i * 3 + 2] = static_cast<std::uint8_t>((!ma[i] && mb[i]) ? 255 : 0);
            }
            const std::string diffPath = aPath.substr(0, aPath.find_last_of('.')) + "_iou.bmp";
            writeMaskBmp(diffPath, a.width, a.height, rgb);

            std::printf("%s\n", aPath.c_str());
            std::printf("    IoU %.5f   %ld px only in the march, %ld only in POV, "
                        "perimeter %ld\n", iou, onlyA, onlyB, perim);
            if (static_cast<double>(diff) > allowed) {
                std::printf("    FAIL  %ld px differ, more than %.0f (%.1f x the perimeter) "
                            "-> %s\n", diff, allowed, tolerance, diffPath.c_str());
                ++failures;
            } else {
                std::printf("    agrees within %.1f px of edge all round -> %s\n",
                            static_cast<double>(diff) / static_cast<double>(perim),
                            diffPath.c_str());
            }
        } catch (const std::exception& e) {
            std::printf("%s\n    FAIL  %s\n", aPath.c_str(), e.what());
            ++failures;
        }
    }

    if (failures == 0) {
        std::printf("\nthe ray march and the ray trace draw the same shape\n");
        return 0;
    }
    std::printf("\n%d comparison(s) FAILED\n", failures);
    return 1;
}
