#pragma once
#include "Pipeline.h"
#include "PipelineDesc.h"
#include "Result.h"
#include "VulkanDevice.h"

#include <volk.h>

namespace RealRHI {
	class VulkanPipeline : public Pipeline {
	public:
		VulkanPipeline(const VulkanDevice* device);
		~VulkanPipeline();

		static Result CreateGraphics(const VulkanDevice* device,
			const PipelineDesc& desc,
			Ref<VulkanPipeline>& outPipeline);
		Result InitGraphics(const PipelineDesc& desc);

		static Result CreateCompute(const VulkanDevice* device,
			const ComputePipelineDesc& desc,
			Ref<VulkanPipeline>& outPipeline);
		Result InitCompute(const ComputePipelineDesc& desc);

	protected:
		friend class VulkanCommandList;
		const std::vector<uint32_t>& GetVertexBindings() const { return m_VertexBindings; }
		VkPipeline GetPipeline() const { return m_Pipeline; }
		VkPipelineBindPoint GetBindPoint() const { return m_BindPoint; }

		const RenderTargetFormats& GetRenderTargetFormats() const { return m_RenderTargetFormats; }
		uint32_t GetSampleCount() const { return m_SampleCount; }
		bool WritesDepth() const { return m_DepthWrites; }
		bool WritesStencil() const { return m_StencilWrites; }

	private:
		Result BuildColorBlendDescriptions(const PipelineDesc& desc,
			std::vector<VkPipelineColorBlendAttachmentState>& attachments) const;

		Result BuildVertexInputDescriptions(const VertexLayout& layout,
			const Shader& shader,
			std::vector<VkVertexInputBindingDescription>& bindings,
			std::vector<VkVertexInputAttributeDescription>& attributes) const;

	private:
		RenderTargetFormats m_RenderTargetFormats;
		uint32_t m_SampleCount = 1;
		bool m_DepthWrites = false;
		bool m_StencilWrites = false;
		std::vector<uint32_t> m_VertexBindings;
		const VulkanDevice* m_Device = nullptr;

		VkPipeline m_Pipeline = VK_NULL_HANDLE;
		VkPipelineBindPoint m_BindPoint = VK_PIPELINE_BIND_POINT_MAX_ENUM;
	};
}
