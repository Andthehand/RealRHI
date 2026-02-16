#pragma once
#include <cstdint>

namespace RealRHI {
    enum class Format {
        RGBA8_UNorm,
        RGBA8_SRGB,
        BGRA8_UNorm,
        D24S8,
        R32_Float,

		Unknown
    };

    enum class TextureUsage : uint32_t {
        None = 0,
        ShaderResource = 1 << 0,
        RenderTarget = 1 << 1,
        DepthStencil = 1 << 2,
        Storage = 1 << 3,
        TransferSrc = 1 << 4,
        TransferDst = 1 << 5,
    };

    struct TextureDesc {
        uint32_t width;
        uint32_t height;
        uint32_t depth = 1;

        uint32_t mipLevels = 1;
        uint32_t arrayLayers = 1;

        Format format;

        TextureUsage usage;
    };
}