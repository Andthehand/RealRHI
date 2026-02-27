#pragma once

namespace RealRHI {
	class TextureView;

    class Texture {
    public:
        virtual ~Texture() = default;

		virtual TextureView* GetTextureView() = 0;
		virtual const TextureView* GetTextureView() const = 0;
    };
}