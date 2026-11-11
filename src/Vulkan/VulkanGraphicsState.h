#pragma once
#include "GraphicsState.h"
#include "VulkanDevice.h"
#include "VulkanShader.h"

#include <optional>

namespace RealRHI {
	struct VulkanRenderingFormats {
		std::vector<TextureFormat> colorFormats;
		TextureFormat depthFormat = TextureFormat::Unknown;
	};

	// Recording-local state translation and cache. Owns no shader or native pipeline object.
	class VulkanGraphicsState {
	public:
		explicit VulkanGraphicsState(const VulkanDevice* device)
			: m_Device(device) { }
		Result SetState(const GraphicsState& state);
		Result SetVertexLayout(const VertexLayout& layout);
		bool ValidateAndApply(VkCommandBuffer commands,
			const VulkanShader* shader,
			const VulkanRenderingFormats& formats,
			uint32_t samples,
			bool readOnlyDepth,
			bool readOnlyStencil,
			const char* operation);
		const std::vector<uint32_t>& GetVertexBindings() const { return m_VertexBindings; }

	private:
		struct DrawDescription : GraphicsState {
			const VulkanShader* shader = nullptr;
			VertexLayout vertexLayout;
			VulkanRenderingFormats renderTargetFormats;
		};
		Result Prepare(const DrawDescription& desc);
		Result ValidateState(const GraphicsState& desc) const;
		Result BuildColorBlendDescriptions(const DrawDescription& desc,
			std::vector<VkPipelineColorBlendAttachmentState>& attachments) const;
		Result BuildVertexInputDescriptions(const VertexLayout& layout,
			const Shader* shader,
			std::vector<VkVertexInputBindingDescription>& bindings,
			std::vector<VkVertexInputAttributeDescription>& attributes) const;
		void Apply(VkCommandBuffer commands);
		const VulkanDevice* m_Device;
		GraphicsState m_State;
		VertexLayout m_Layout, m_EffectiveLayout;
		bool m_HasState = false, m_ExplicitVertexLayout = false, m_Dirty = true;
		const VulkanShader* m_ValidatedShader = nullptr;
		VulkanRenderingFormats m_ValidatedFormats;
		std::optional<GraphicsState> m_AppliedState;
		std::optional<VertexLayout> m_AppliedLayout;
		size_t m_AppliedColorCount = 0;
		uint32_t m_SampleCount = 1;
		bool m_DepthWrites = false, m_StencilWrites = false;
		std::vector<uint32_t> m_VertexBindings;
		std::vector<VkVertexInputBindingDescription2EXT> m_Bindings;
		std::vector<VkVertexInputAttributeDescription2EXT> m_Attributes;
		std::vector<VkBool32> m_BlendEnables;
		std::vector<VkColorBlendEquationEXT> m_BlendEquations;
		std::vector<VkColorComponentFlags> m_ColorWriteMasks;
	};
}
