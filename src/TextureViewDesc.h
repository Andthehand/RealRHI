#pragma once
#include <cstdint>

namespace RealRHI {
	class Texture;

	enum class TextureViewType {
		View1D,
		View2D,
		View2DArray,
		View3D,
		Cube,
		CubeArray,
	};

	struct TextureViewDesc {
		const Texture* texture;

		TextureViewType type = TextureViewType::View2D;

		// Subresource range
		uint32_t baseMipLevel = 0;
		uint32_t mipLevelCount = 1;

		uint32_t baseArrayLayer = 0;
		uint32_t arrayLayerCount = 1;
	};
}
