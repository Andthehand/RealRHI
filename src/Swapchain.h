#pragma once
#include "Texture.h"

namespace RealRHI {
    class Swapchain {
    public:
		virtual ~Swapchain() = default;

        virtual TextureFormat GetBackBufferFormat() const = 0;
    };
}
