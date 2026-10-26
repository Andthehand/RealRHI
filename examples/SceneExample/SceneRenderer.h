#pragma once
#include "Device.h"
#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/TransientResourcePool.h"

#include <vector>

namespace RealRHI::Examples {
	class SceneRenderer {
	private:
		enum class GeometryPass {
			Shadow,
			Lighting
		};

		struct FrameTargets {
			Texture* color = nullptr;
			Texture* depth = nullptr;
			Texture* shadow = nullptr;
			uint32_t width = 0;
			uint32_t height = 0;
		};

		struct FrameSize {
			uint32_t width = 0;
			uint32_t height = 0;
		};

	public:
		Result Init(Device& device, TextureFormat outputFormat, uint32_t frameCount, UploadSubmission& outUpload);

		// Set dimensions for the next frame recording; allocation happens inside Record.
		Result ResizeTargets(uint32_t frameIndex, uint32_t width, uint32_t height);
		Result Record(CommandList& commands,
			uint32_t frameIndex,
			TextureView* output,
			float rotation,
			const UploadSubmission& pendingUpload,
			bool shadowsEnabled = true);

		Result Submit(Swapchain& swapchain,
			const FrameContext& frame,
			float rotation,
			const UploadSubmission& pendingUpload,
			RenderGraphSubmission& outSubmission,
			bool shadowsEnabled = true);

	private:
		Result BuildGraph(RenderGraph& graph,
			uint32_t frameIndex,
			TextureView* output,
			float rotation,
			const UploadSubmission& pendingUpload,
			bool shadowsEnabled,
			std::optional<RenderGraphResource> outputResource = {});
		Result SelectDepthFormat();
		Result CreatePipelines(TextureFormat outputFormat);
		Result UploadAssets(UploadSubmission& outUpload);
		Result CreateGeometry(UploadContext& uploads);
		Result CreatePattern(UploadContext& uploads);

		void DrawShadowMap(CommandList& commands, FrameTargets& targets, float rotation, bool shadowsEnabled);
		void DrawGeometry(CommandList& commands,
			const FrameTargets& targets,
			float rotation,
			GeometryPass pass,
			bool shadowsEnabled);

		void BeginScene(CommandList& commands, FrameTargets& targets);
		void DrawScene(CommandList& commands, FrameTargets& targets, float rotation, bool shadowsEnabled);
		void DrawPostprocess(CommandList& commands, FrameTargets& targets, TextureView* output);

	private:
		Device* m_Device = nullptr;
		std::unique_ptr<TransientResourcePool> m_TransientResources;
		Ref<Pipeline> m_ShadowPipeline;
		Ref<Pipeline> m_OpaquePipeline;
		Ref<Pipeline> m_TransparentPipeline;
		Ref<Pipeline> m_PostprocessPipeline;

		Ref<Buffer> m_VertexBuffer;
		Ref<Buffer> m_IndexBuffer;
		Ref<Texture> m_Pattern;
		Ref<Sampler> m_Sampler;
		Ref<Sampler> m_ShadowSampler;

		TextureFormat m_DepthFormat = TextureFormat::Unknown;
		uint32_t m_ShadowResolution = 1024;

		std::vector<FrameSize> m_Frames;
	};
}
