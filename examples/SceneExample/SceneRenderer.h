#pragma once
#include "Device.h"

#include <vector>

namespace RealRHI::Examples {
	class SceneRenderer {
	private:
		struct FrameTargets {
			Ref<Texture> color;
			Ref<Texture> depth;
			uint32_t width = 0;
			uint32_t height = 0;
			bool initialized = false;
		};

	public:
		Result Init(Device& device, TextureFormat outputFormat, uint32_t frameCount, UploadSubmission& outUpload);

		// Call after the frame slot's previous submission has completed and its list has been reset.
		Result ResizeTargets(uint32_t frameIndex, uint32_t width, uint32_t height);
		void Record(CommandList& commands,
			uint32_t frameIndex,
			TextureView* output,
			float rotation,
			const UploadSubmission& pendingUpload);

	private:
		Result CreatePipelines(TextureFormat outputFormat);
		Result UploadAssets(UploadSubmission& outUpload);
		Result CreateGeometry(UploadContext& uploads);
		Result CreatePattern(UploadContext& uploads);

		void BeginScene(CommandList& commands, FrameTargets& targets);
		void DrawScene(CommandList& commands, const FrameTargets& targets, float rotation);
		void DrawPostprocess(CommandList& commands, FrameTargets& targets, TextureView* output);

	private:
		Device* m_Device = nullptr;
		Ref<Pipeline> m_OpaquePipeline;
		Ref<Pipeline> m_TransparentPipeline;
		Ref<Pipeline> m_PostprocessPipeline;

		Ref<Buffer> m_VertexBuffer;
		Ref<Buffer> m_IndexBuffer;
		Ref<Texture> m_Pattern;
		Ref<Sampler> m_Sampler;

		std::vector<FrameTargets> m_Frames;
	};
}
