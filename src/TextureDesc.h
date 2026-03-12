#pragma once
#include "TextureFormat.h"

#include <cstdint>

namespace RealRHI {
    enum class TextureUsage : uint32_t {
        None = 0,
        ShaderResource = 1 << 0,
        RenderTarget = 1 << 1,
        DepthStencil = 1 << 2,
        Storage = 1 << 3,
        TransferSrc = 1 << 4,
        TransferDst = 1 << 5,
    };

    constexpr TextureUsage operator|(TextureUsage a, TextureUsage b) {
        return static_cast<TextureUsage>(
            static_cast<uint32_t>(a) | static_cast<uint32_t>(b)
        );
    }
    constexpr TextureUsage operator&(TextureUsage a, TextureUsage b) {
        return static_cast<TextureUsage>(
            static_cast<uint32_t>(a) & static_cast<uint32_t>(b)
        );
    }
    constexpr TextureUsage& operator|=(TextureUsage& a, TextureUsage b) {
        a = a | b;
        return a;
    }
    constexpr bool Any(TextureUsage v) {
        return static_cast<uint32_t>(v) != 0;
    }

    struct TextureDesc {
        uint32_t width;
        uint32_t height;
        uint32_t depth = 1;

        uint32_t mipLevels = 1;
        uint32_t arrayLayers = 1;

        TextureFormat format;

        TextureUsage usage;
    };
}