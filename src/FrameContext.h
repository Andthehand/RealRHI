#pragma once
#include <cstdint>

namespace RealRHI {
    class TextureView;

    struct FrameContext {
        uint32_t frameIndex = 0;
        uint32_t imageIndex = 0;
        TextureView* backBufferView = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
    };
}
