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
		// Present consumes the acquired frame; BeginFrame returns an invalid view while minimized.
		virtual Result Present(const FrameContext& frame) = 0;
		virtual bool IsFrameReady(const FrameContext& frame) const = 0;
		// Borrowed image for the exact acquisition ID; stale contexts resolve to nullptr.
		virtual Texture* GetBackBuffer(const FrameContext& frame) = 0;
		virtual uint32_t GetMaxFramesInFlight() const = 0;
	};
}
