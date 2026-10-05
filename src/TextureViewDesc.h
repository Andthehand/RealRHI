#pragma once
#include "Texture.h"

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

		// Subresource range
		uint32_t baseMipLevel = 0;
		uint32_t mipLevelCount = 1;

		uint32_t baseArrayLayer = 0;
		uint32_t arrayLayerCount = 1;
	};
}
