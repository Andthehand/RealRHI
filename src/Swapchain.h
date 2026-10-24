#pragma once
#include "FrameContext.h"
#include "RefCounted.h"
#include "Texture.h"
#include "TextureFormat.h"

namespace RealRHI {
	class Swapchain : public RefCounted {
	public:
		virtual ~Swapchain() = default;

		virtual TextureFormat GetBackBufferFormat() const = 0;

		virtual FrameContext BeginFrame() = 0;
		virtual void Present(const FrameContext& frame) = 0;
		virtual uint32_t GetMaxFramesInFlight() const = 0;
	};
}
