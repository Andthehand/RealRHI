#pragma once
#include "RefCounted.h"

#include "Result.h"

namespace RealRHI {
	class TextureView;

    class Texture : public RefCounted {
    public:
        virtual ~Texture() = default;

		virtual TextureView* GetTextureView() = 0;
		virtual const TextureView* GetTextureView() const = 0;

		virtual Result UploadData(const void* data, uint32_t size) = 0;
    };
}