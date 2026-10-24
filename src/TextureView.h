#pragma once
#include "RefCounted.h"

#include <cstdint>

namespace RealRHI {
	class TextureView : public RefCounted {
	public:
		virtual ~TextureView() = default;

		virtual uint32_t GetSampledImageDescriptorIndex() const { return UINT32_MAX; }
		virtual uint32_t GetStorageImageDescriptorIndex() const { return UINT32_MAX; }
	};
}
