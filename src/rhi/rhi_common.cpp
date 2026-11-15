#include "rhi/rhi.h"

namespace gfx::rhi {

bool isDepthFormat(Format f) { return f == Format::D32F || f == Format::D24S8; }

bool isCompressedFormat(Format f) {
    switch (f) {
        case Format::BC1_SRGB: case Format::BC3_SRGB: case Format::BC4: case Format::BC5: case Format::BC7_SRGB: return true;
        default: return false;
    }
}

uint32_t formatBytesPerPixel(Format f) {
    switch (f) {
        case Format::R8: return 1;
        case Format::RG8: case Format::R16F: return 2;
        case Format::RGBA8: case Format::RGBA8_SRGB: case Format::BGRA8_SRGB: case Format::RG16F: case Format::R32F:
        case Format::R11G11B10F: case Format::RGB10A2: case Format::R32UI: case Format::D32F: case Format::D24S8: return 4;
        case Format::RGBA16F: case Format::RG32F: case Format::RG32UI: return 8;
        case Format::RGBA32F: case Format::RGBA32UI: return 16;
        case Format::BC1_SRGB: case Format::BC4: return 1;    // 8 bytes / 16 px -> 0.5, rounded up
        case Format::BC3_SRGB: case Format::BC5: case Format::BC7_SRGB: return 1;
        default: return 4;
    }
}

}  // namespace gfx::rhi
