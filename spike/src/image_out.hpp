// Phase S spike: minimal 24-bit BMP writer.
//
// The spike is headless, so the frame lands in a file rather than a window. BMP keeps this
// dependency-free; Windows opens it natively.

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace spike {

// src is tightly packed RGBA8, top row first. rowPitch is in bytes and may exceed width * 4,
// because a D3D12 readback buffer is padded to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT.
inline bool writeBmp(const std::string& path, const std::uint8_t* src, int width, int height,
                     int rowPitch, std::string& error) {
    const int rowBytes = width * 3;
    const int padding = (4 - (rowBytes % 4)) % 4;
    const int strideOut = rowBytes + padding;
    const std::uint32_t pixelBytes = static_cast<std::uint32_t>(strideOut) * height;
    const std::uint32_t fileBytes = 54 + pixelBytes;

    std::vector<std::uint8_t> out(fileBytes, 0);

    // BITMAPFILEHEADER (14 bytes) then BITMAPINFOHEADER (40 bytes), little-endian throughout.
    auto put32 = [&out](std::size_t at, std::uint32_t v) {
        out[at + 0] = static_cast<std::uint8_t>(v);
        out[at + 1] = static_cast<std::uint8_t>(v >> 8);
        out[at + 2] = static_cast<std::uint8_t>(v >> 16);
        out[at + 3] = static_cast<std::uint8_t>(v >> 24);
    };

    out[0] = 'B';
    out[1] = 'M';
    put32(2, fileBytes);
    put32(10, 54);
    put32(14, 40);
    put32(18, static_cast<std::uint32_t>(width));
    put32(22, static_cast<std::uint32_t>(height));  // positive: rows stored bottom-up
    out[26] = 1;                                    // planes
    out[28] = 24;                                   // bits per pixel
    put32(34, pixelBytes);

    for (int y = 0; y < height; ++y) {
        const std::uint8_t* srcRow = src + static_cast<std::size_t>(height - 1 - y) * rowPitch;
        std::uint8_t* dstRow = out.data() + 54 + static_cast<std::size_t>(y) * strideOut;
        for (int x = 0; x < width; ++x) {
            dstRow[x * 3 + 0] = srcRow[x * 4 + 2];  // B
            dstRow[x * 3 + 1] = srcRow[x * 4 + 1];  // G
            dstRow[x * 3 + 2] = srcRow[x * 4 + 0];  // R
        }
    }

    std::FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || f == nullptr) {
        error = "could not open '" + path + "' for writing";
        return false;
    }

    const std::size_t written = std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);

    if (written != out.size()) {
        error = "short write to '" + path + "'";
        return false;
    }
    return true;
}

}  // namespace spike
