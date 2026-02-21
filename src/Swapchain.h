#pragma once
#include "Texture.h"

namespace RealRHI {
    class Swapchain {
    public:
		virtual ~Swapchain() = default;

        virtual TextureFormat GetBackBufferFormat() const = 0;

        virtual void BeginFrame() = 0;
        virtual void Present() = 0;
        virtual Texture* GetCurrentBackBuffer() = 0;
    };
}
