#pragma once
#include "RefCounted.h"

namespace RealRHI {
	class TextureView;

    class Texture : public RefCounted {
    public:
        virtual ~Texture() = default;

		virtual TextureView* GetTextureView() = 0;
		virtual const TextureView* GetTextureView() const = 0;
    };
}