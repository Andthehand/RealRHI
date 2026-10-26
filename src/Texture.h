#pragma once
#include "RefCounted.h"
#include "Result.h"
#include "TextureView.h"

#include <cstdint>
#include <limits>

namespace RealRHI {
	class Texture : public RefCounted {
	public:
		virtual ~Texture() = default;

		// Native allocation size, including alignment; external images report zero.
		virtual uint64_t GetAllocationSize() const = 0;
		virtual uint32_t GetSampleCount() const = 0;

		// Borrowed default view; keep the texture alive while using it.
		// Sampled-only textures expose all mips; storage/attachment defaults select the base mip.
		// CreateTextureView provides an independently owned view of a selected range.
		virtual TextureView* GetTextureView() = 0;
		virtual const TextureView* GetTextureView() const = 0;

		virtual uint32_t GetSampledImageDescriptorIndex() const { return std::numeric_limits<uint32_t>::max(); }

		virtual uint32_t GetSamplerDescriptorIndex() const { return std::numeric_limits<uint32_t>::max(); }

		virtual uint32_t GetStorageImageDescriptorIndex() const { return std::numeric_limits<uint32_t>::max(); }

		// Uploads tightly packed color data to a single-mip texture with TransferDst usage.
		virtual Result UploadData(const void* data, uint32_t size) = 0;
	};
}
