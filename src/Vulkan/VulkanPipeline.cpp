#include "VulkanPipeline.h"

#include "VulkanShader.h"
#include "VulkanConvertions.h"

namespace RealRHI {
    VulkanPipeline::VulkanPipeline(const VulkanDevice* device, const PipelineDesc& desc) 
        : m_Device(device) {
		const VulkanShader* shader = static_cast<VulkanShader*>(desc.shader.get());
		VkShaderModule shaderModule = shader->GetShaderModule();

		const std::vector<EntryPoint>& entryPoints = shader->GetEntryPoints();
        std::vector<VkPipelineShaderStageCreateInfo> shaderStages(entryPoints.size());
		for (size_t i = 0; i < entryPoints.size(); i++) {
            shaderStages[i] = VkPipelineShaderStageCreateInfo{
                .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage  = Utils::ShaderStageToVkShaderStage(entryPoints[i].stage),
                .module = shaderModule,
                .pName  = entryPoints[i].entryPoint
            };
        }

        VkVertexInputBindingDescription bindingDescription{
            .binding = 0,
            .stride = desc.vertexLayout.stride,
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
        };

        std::vector<VkVertexInputAttributeDescription> attributeDescriptions{};
        for (size_t i = 0; i < desc.vertexLayout.attributes.size(); i++) {
			const BufferAttribute attr = desc.vertexLayout.attributes[i];

            attributeDescriptions.push_back(VkVertexInputAttributeDescription{
                .location = static_cast<uint32_t>(i),
                .binding = 0,
                .format = Utils::BufferDataTypeToVkFormat(attr.type),
				.offset = attr.offset
            });
        }

        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputInfo.vertexBindingDescriptionCount = 1;
        vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
        vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
        vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssembly.primitiveRestartEnable = VK_FALSE;

        std::array<VkDynamicState, 2> dynamicStates { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamicState { 
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, 
            .dynamicStateCount = dynamicStates.size(),
            .pDynamicStates = dynamicStates.data()
        };

        VkPipelineViewportStateCreateInfo viewportState {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .scissorCount = 1,
        };

        VkPipelineRasterizationStateCreateInfo rasterizer {
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

        VkPipelineDepthStencilStateCreateInfo depthStencilState { 
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, 
            .depthTestEnable = desc.depthState.depthTestEnable, 
            .depthWriteEnable = desc.depthState.depthWriteEnable, 
			.depthCompareOp = Utils::CompareOpToVkCompareOp(desc.depthState.compareOp),
        };

        VkPipelineColorBlendAttachmentState colorBlendAttachment {
            .blendEnable = desc.blendState.enable,
			.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
			.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
			.colorBlendOp = VK_BLEND_OP_ADD,
			.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
			.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
			.alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        };

        VkPipelineColorBlendStateCreateInfo colorBlending {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .logicOpEnable = VK_FALSE,
            .attachmentCount = 1,
            .pAttachments = &colorBlendAttachment,
        };

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        };

        if (vkCreatePipelineLayout(device->GetDevice(), &pipelineLayoutInfo, nullptr, &m_PipelineLayout) != VK_SUCCESS) {
			// TODO: Implement proper error handling
            return;
        }

		const RenderTargetFormats& formats = desc.renderTargetFormats;
		std::vector<VkFormat> vkColorFormats(formats.colorFormats.size());
        for (size_t i = 0; i < formats.colorFormats.size(); i++) {
            vkColorFormats[i] = Utils::TextureFormatToVkFormat(formats.colorFormats[i]);
        }

        VkPipelineRenderingCreateInfo pipelineRenderingInfo {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = (uint32_t)vkColorFormats.size(),
			.pColorAttachmentFormats = vkColorFormats.data(),
			.depthAttachmentFormat = Utils::TextureFormatToVkFormat(formats.depthFormat),
        };

        VkGraphicsPipelineCreateInfo pipelineInfo{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &pipelineRenderingInfo, // Chain the dynamic rendering info
            .stageCount = (uint32_t)shaderStages.size(),
            .pStages = shaderStages.data(),
            .pVertexInputState = &vertexInputInfo,
            .pInputAssemblyState = &inputAssembly,
            .pViewportState = &viewportState,
            .pRasterizationState = &rasterizer,
            .pMultisampleState = &multisampling,
			.pDepthStencilState = &depthStencilState,
            .pColorBlendState = &colorBlending,
			.pDynamicState = &dynamicState,
            .layout = m_PipelineLayout,
            .renderPass = nullptr, // We're using dynamic rendering, so no render pass
        };

        if (vkCreateGraphicsPipelines(device->GetDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_Pipeline) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create graphics pipeline!");
        }
	}
    
    VulkanPipeline::~VulkanPipeline() {
        vkDestroyPipeline(m_Device->GetDevice(), m_Pipeline, nullptr);
		vkDestroyPipelineLayout(m_Device->GetDevice(), m_PipelineLayout, nullptr);
    }
}
