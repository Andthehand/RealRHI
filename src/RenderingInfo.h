#pragma once
#include "TextureView.h"

#include <cstdint>
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

	enum class ResolveMode : uint32_t {
		None = 0,
		SampleZero = 1,
		Average = 2,
		Min = 4,
		Max = 8,
	};

	struct ColorAttachment {
		TextureView* target = nullptr;
		LoadOp loadOp = LoadOp::Clear;
		StoreOp storeOp = StoreOp::Store;
		ClearColorValue clearColor{};
		// Single-sampled, matching format/extent; integer colors resolve sample zero,
		// floating-point and normalized colors resolve the average.
		TextureView* resolveTarget = nullptr;
	};

	struct DepthStencilAttachment {
		TextureView* target = nullptr;
		LoadOp depthLoadOp = LoadOp::Clear;
		StoreOp depthStoreOp = StoreOp::Store;
		ClearDepthStencilValue clear{};
		bool readOnlyDepth = false;
		bool readOnlyStencil = false;
		LoadOp stencilLoadOp = LoadOp::Clear;
		StoreOp stencilStoreOp = StoreOp::Store;
		// Resolve writes use ColorAttachmentOutput/ColorAttachmentWrite synchronization,
		// including for depth/stencil images. Keep the target in DepthStencilAttachment layout.
		TextureView* resolveTarget = nullptr;
		ResolveMode depthResolveMode = ResolveMode::None;
		ResolveMode stencilResolveMode = ResolveMode::None;
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

		Rect renderArea{};
	};
}
