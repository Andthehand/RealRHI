#pragma once
#include "TextureView.h"

#include <cstdint>

namespace RealRHI {
    struct FrameContext {
        uint32_t frameIndex = 0;
        uint32_t imageIndex = 0;
        TextureView* backBufferView = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
    };
}
