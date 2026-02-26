#pragma once
#include "Texture.h"

#include <vector>

namespace RealRHI {
	enum class LoadOp {
		Load,
		Clear,
		DontCare
	};

	enum class StoreOp {
		Store,
		DontCare
	};

	struct ClearColorValue {
		float r = 0.f;
		float g = 0.f;
		float b = 0.f;
		float a = 1.f;
	};

	struct ClearDepthStencilValue {
		float depth = 1.f;
		uint32_t stencil = 0;
	};

	struct ColorAttachment {
		Texture* target = nullptr;      // direction: switch to TextureView* later
		LoadOp loadOp = LoadOp::Clear;
		StoreOp storeOp = StoreOp::Store;
		ClearColorValue clearColor{};
	};

	struct DepthStencilAttachment {
		Texture* target = nullptr;      // direction: switch to TextureView* later
		LoadOp depthLoadOp = LoadOp::Clear;
		StoreOp depthStoreOp = StoreOp::Store;
		ClearDepthStencilValue clear{};
		bool readOnlyDepth = false;
		bool readOnlyStencil = true;
	};

	struct Viewport {
		float x = 0.f;
		float y = 0.f;
		float width = 0.f;
		float height = 0.f;
		float minDepth = 0.f;
		float maxDepth = 1.f;
	};

	struct Rect {
		int32_t x = 0;
		int32_t y = 0;
		uint32_t width = 0;
		uint32_t height = 0;
	};

	struct RenderingInfo {
		std::vector<ColorAttachment> colorAttachments;
		bool hasDepth = false;
		DepthStencilAttachment depthAttachment{};

		bool hasScissor = false;
		Rect scissor{};
	};
}
