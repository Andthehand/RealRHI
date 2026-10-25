#pragma once
#include "TextureDesc.h"

#include <cstdint>

namespace RealRHI {
	enum class TextureViewType {
		View1D,
		View2D,
		View2DArray,
		View3D,
		Cube,
		CubeArray,
	};

	struct TextureViewDesc {
		TextureViewType type = TextureViewType::View2D;

		uint32_t baseMipLevel = 0;
		uint32_t mipLevelCount = 1;

		uint32_t baseArrayLayer = 0;
		uint32_t arrayLayerCount = 1;

		// Select descriptor/attachment usage from the texture's creation flags.
		// None uses all view-compatible texture usages. Storage and attachment views select one mip.
		TextureUsage usage = TextureUsage::None;
		const char* debugName = nullptr;
	};
}
