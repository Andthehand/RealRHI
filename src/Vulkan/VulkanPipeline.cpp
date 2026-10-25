#include "VulkanPipeline.h"

#include "VulkanConvertions.h"
#include "VulkanCpuTimer.h"
#include "VulkanShader.h"

namespace RealRHI {
	VulkanPipeline::VulkanPipeline(const VulkanDevice* device)
		: m_Device(device) { }

	VulkanPipeline::~VulkanPipeline() {
		vkDestroyPipeline(m_Device->GetDevice(), m_Pipeline, nullptr);
		if (m_BindPoint != VK_PIPELINE_BIND_POINT_MAX_ENUM) {
			m_Device->RecordResourceDestroyed(VulkanProfiledResource::Pipeline);
		}
	}

	Result VulkanPipeline::CreateGraphics(const VulkanDevice* device,
		const PipelineDesc& desc,
		Ref<VulkanPipeline>& outPipeline) {
		Ref<VulkanPipeline> pipeline = Ref<VulkanPipeline>::Create(device);
		Result res = pipeline->InitGraphics(desc);
		if (res != Result::Success) {
			return res;
		}

		outPipeline = pipeline;
		return Result::Success;
	}

	Result VulkanPipeline::CreateCompute(const VulkanDevice* device,
		const ComputePipelineDesc& desc,
		Ref<VulkanPipeline>& outPipeline) {
		Ref<VulkanPipeline> pipeline = Ref<VulkanPipeline>::Create(device);
		const Result result = pipeline->InitCompute(desc);
		if (result != Result::Success) {
			return result;
		}

		outPipeline = pipeline;
		return Result::Success;
	}

	Result VulkanPipeline::InitCompute(const ComputePipelineDesc& desc) {
		const VulkanCpuTimer timer(m_Device);
		const auto* shader = static_cast<const VulkanShader*>(desc.shader);
		if (shader == nullptr) {
			return Result::Failed;
		}

		const auto& entryPoints = shader->GetEntryPoints();
		if (entryPoints.size() != 1 || entryPoints[0].stage != SLANG_STAGE_COMPUTE) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"Compute pipeline requires exactly one compute shader entry point.");
			return Result::Failed;
		}

		const VkPipelineShaderStageCreateInfo stage{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = shader->GetShaderModule(),
			// Slang's linked SPIR-V module exports the compute entry point as "main".
			.pName = "main",
		};
		const VkPipelineCreateFlags2CreateInfo flags{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
			.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
		};
		const VkComputePipelineCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.pNext = &flags,
			.stage = stage,
			.layout = VK_NULL_HANDLE,
		};

		if (vkCreateComputePipelines(m_Device->GetDevice(), VK_NULL_HANDLE, 1, &info, nullptr, &m_Pipeline) !=
			VK_SUCCESS) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan compute pipeline.");
			return Result::Failed;
		}

		m_BindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
		m_Device->RecordResourceCreated(VulkanProfiledResource::Pipeline);
		const Result nameResult = m_Device->SetDebugName(VK_OBJECT_TYPE_PIPELINE, m_Pipeline, desc.debugName);
		if (nameResult != Result::Success) {
			return nameResult;
		}
		timer.Finish(VulkanProfiledOperation::ComputePipeline);
		
		return Result::Success;
	}

	Result VulkanPipeline::InitGraphics(const PipelineDesc& desc) {
		const VulkanCpuTimer timer(m_Device);
		const auto* shader = static_cast<const VulkanShader*>(desc.shader);
		if (shader == nullptr || (desc.rasterState.fillMode == FillMode::Wireframe && !m_Device->SupportsWireframe())) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CreateGraphicsPipeline: Provide a Vulkan shader and a fill mode supported by the device.");

			return Result::InvalidParameter;
		}
		VkShaderModule shaderModule = shader->GetShaderModule();

		const std::vector<EntryPoint>& entryPoints = shader->GetEntryPoints();
		std::vector<VkPipelineShaderStageCreateInfo> shaderStages(entryPoints.size());
		for (uint32_t i = 0; i < entryPoints.size(); i++) {
			shaderStages[i] = VkPipelineShaderStageCreateInfo{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = Utils::SlangStageToVkShaderStage(entryPoints[i].stage),
				.module = shaderModule,
				.pName = entryPoints[i].entryPointName.c_str(),
			};
		}

		const BufferLayout& vertexLayout = shader->GetBufferLayout();
		VkVertexInputBindingDescription bindingDescription{
			.binding = 0,
			.stride = vertexLayout.stride,
			.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
		};

		const std::vector<BufferAttribute>& vertexAttributes = vertexLayout.attributes;
		std::vector<VkVertexInputAttributeDescription> attributeDescriptions(vertexAttributes.size());
		for (uint32_t i = 0; i < vertexAttributes.size(); i++) {
			const BufferAttribute& attr = vertexAttributes[i];

			attributeDescriptions[i] = VkVertexInputAttributeDescription{
				.location = i,
				.binding = 0,
				.format = Utils::ScalarTypeToVkFormat(attr.type),
				.offset = attr.offset,
			};
		}

		VkPipelineVertexInputStateCreateInfo vertexInputInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
			.vertexBindingDescriptionCount = vertexAttributes.empty() ? 0u : 1u,
			.pVertexBindingDescriptions = &bindingDescription,
			.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size()),
			.pVertexAttributeDescriptions = attributeDescriptions.data(),
		};

		VkPipelineInputAssemblyStateCreateInfo inputAssembly{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
			.primitiveRestartEnable = VK_FALSE,
		};

		std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo dynamicState{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
			.dynamicStateCount = dynamicStates.size(),
			.pDynamicStates = dynamicStates.data(),
		};

		VkPipelineViewportStateCreateInfo viewportState{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
			.viewportCount = 1,
			.scissorCount = 1,
		};

		VkPipelineRasterizationStateCreateInfo rasterizer{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			.depthClampEnable = VK_FALSE,
			.rasterizerDiscardEnable = VK_FALSE,
			.polygonMode = Utils::FillModeToVkPolygonMode(desc.rasterState.fillMode),
			.cullMode = Utils::CullModeToVkCullMode(desc.rasterState.cullMode),
			.frontFace = Utils::FrontFaceToVkFrontFace(desc.rasterState.frontCounterClockwise),
			.depthBiasEnable = VK_FALSE,
			.lineWidth = 1.0f,
		};

		VkPipelineMultisampleStateCreateInfo multisampling{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
			.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
			.sampleShadingEnable = VK_FALSE,
		};

		VkPipelineDepthStencilStateCreateInfo depthStencilState{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
			.depthTestEnable = desc.depthState.depthTestEnable,
			.depthWriteEnable = desc.depthState.depthWriteEnable,
			.depthCompareOp = Utils::CompareOpToVkCompareOp(desc.depthState.compareOp),
		};

		VkPipelineColorBlendAttachmentState colorBlendAttachment{
			.blendEnable = desc.blendState.enable,
			.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
			.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
			.colorBlendOp = VK_BLEND_OP_ADD,
			.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
			.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
			.alphaBlendOp = VK_BLEND_OP_ADD,
			.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
				VK_COLOR_COMPONENT_A_BIT,
		};

		const RenderTargetFormats& formats = desc.renderTargetFormats;
		std::vector<VkPipelineColorBlendAttachmentState> blendAttachments(
			formats.colorFormats.size(), colorBlendAttachment);
		VkPipelineColorBlendStateCreateInfo colorBlending{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.logicOpEnable = VK_FALSE,
			.attachmentCount = static_cast<uint32_t>(formats.colorFormats.size()),
			.pAttachments = blendAttachments.data(),
		};

		std::vector<VkFormat> vkColorFormats(formats.colorFormats.size());
		for (uint32_t i = 0; i < formats.colorFormats.size(); i++) {
			vkColorFormats[i] = Utils::TextureFormatToVkFormat(formats.colorFormats[i]);
		}

		const VkImageAspectFlags depthStencilAspects = Utils::TextureFormatToVkImageAspect(formats.depthFormat);
		VkFormat depthFormat = VK_FORMAT_UNDEFINED;
		VkFormat stencilFormat = VK_FORMAT_UNDEFINED;
		if ((depthStencilAspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0) {
			depthFormat = Utils::TextureFormatToVkFormat(formats.depthFormat);
		}

		if ((depthStencilAspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0) {
			stencilFormat = Utils::TextureFormatToVkFormat(formats.depthFormat);
		}

		VkPipelineRenderingCreateInfo pipelineRenderingInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
			.colorAttachmentCount = static_cast<uint32_t>(vkColorFormats.size()),
			.pColorAttachmentFormats = vkColorFormats.data(),
			.depthAttachmentFormat = depthFormat,
			.stencilAttachmentFormat = stencilFormat,
		};

		VkPipelineCreateFlags2CreateInfo pipelineCreateFlags2Info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
			.pNext = &pipelineRenderingInfo,
			.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
		};

		VkGraphicsPipelineCreateInfo pipelineInfo{
			.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			.pNext = &pipelineCreateFlags2Info,
			.stageCount = static_cast<uint32_t>(shaderStages.size()),
			.pStages = shaderStages.data(),
			.pVertexInputState = &vertexInputInfo,
			.pInputAssemblyState = &inputAssembly,
			.pViewportState = &viewportState,
			.pRasterizationState = &rasterizer,
			.pMultisampleState = &multisampling,
			.pDepthStencilState = &depthStencilState,
			.pColorBlendState = &colorBlending,
			.pDynamicState = &dynamicState,
		};

		if (vkCreateGraphicsPipelines(m_Device->GetDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_Pipeline) !=
			VK_SUCCESS) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan graphics pipelines.");
			return Result::Failed;
		}

		m_BindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		m_Device->RecordResourceCreated(VulkanProfiledResource::Pipeline);
		const Result nameResult = m_Device->SetDebugName(VK_OBJECT_TYPE_PIPELINE, m_Pipeline, desc.debugName);
		if (nameResult != Result::Success) {
			return nameResult;
		}
		timer.Finish(VulkanProfiledOperation::GraphicsPipeline);
		
		return Result::Success;
	}
}
