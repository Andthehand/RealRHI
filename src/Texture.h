#pragma once

namespace RealRHI {
	class TextureView;

    class Texture {
    public:
        virtual ~Texture() = default;

		virtual const TextureView* GetTextureView() const = 0;
    };
}