#pragma once
#include "Texture.h"
#include "FrameContext.h"

namespace RealRHI {
    class Swapchain {
    public:
		virtual ~Swapchain() = default;

        virtual TextureFormat GetBackBufferFormat() const = 0;

        virtual FrameContext BeginFrame() = 0;
        virtual void Present(const FrameContext& frame) = 0;
        virtual uint32_t GetMaxFramesInFlight() const = 0;
    };
}
