#include "VulkanPipeline.h"

#include "VulkanShader.h"
#include "VulkanConvertions.h"

namespace RealRHI {
    VulkanPipeline::VulkanPipeline(const VulkanDevice* device) : m_Device(device) {
    }

    VulkanPipeline::~VulkanPipeline() {
        vkDestroyPipeline(m_Device->GetDevice(), m_Pipeline, nullptr);
        vkDestroyPipelineLayout(m_Device->GetDevice(), m_PipelineLayout, nullptr);
        if (m_DescriptorSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(m_Device->GetDevice(), m_DescriptorSetLayout, nullptr);
        }
    }

    Result VulkanPipeline::Create(const VulkanDevice* device, const PipelineDesc& desc, Ref<VulkanPipeline>& outPipeline) {
        Ref<VulkanPipeline> pipeline = Ref<VulkanPipeline>::Create(device);
        Result res = pipeline->Init(desc);
        if (res != Result::Success) {
            return res;
        }

        outPipeline = pipeline;
        return Result::Success;
	}

    Result VulkanPipeline::Init(const PipelineDesc& desc) {
        const VulkanShader* shader = static_cast<VulkanShader*>(desc.shader);
        VkShaderModule shaderModule = shader->GetShaderModule();

        const std::vector<EntryPoint>& entryPoints = shader->GetEntryPoints();
        std::vector<VkPipelineShaderStageCreateInfo> shaderStages(entryPoints.size());
        for (uint32_t i = 0; i < entryPoints.size(); i++) {
            shaderStages[i] = VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = Utils::SlangStageToVkShaderStage(entryPoints[i].stage),
                .module = shaderModule,
                .pName = entryPoints[i].entryPointName.c_str()
            };
        }

        // Vertex input is taken from the shader reflection data
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
                .offset = attr.offset
            };
        }

        VkPipelineVertexInputStateCreateInfo vertexInputInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = &bindingDescription,
            .vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size()),
            .pVertexAttributeDescriptions = attributeDescriptions.data(),
        };

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .primitiveRestartEnable = VK_FALSE,
        };

        std::array<VkDynamicState, 2> dynamicStates{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamicState{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = dynamicStates.size(),
            .pDynamicStates = dynamicStates.data()
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
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        };

        VkPipelineColorBlendStateCreateInfo colorBlending{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .logicOpEnable = VK_FALSE,
            .attachmentCount = 1,
            .pAttachments = &colorBlendAttachment,
        };

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        };

        if (!desc.descriptorBindings.empty()) {
            std::vector<VkDescriptorSetLayoutBinding> layoutBindings(desc.descriptorBindings.size());
            for (uint32_t i = 0; i < desc.descriptorBindings.size(); i++) {
                const auto& binding = desc.descriptorBindings[i];
                layoutBindings[i] = VkDescriptorSetLayoutBinding{
                    .binding = binding.binding,
                    .descriptorType = Utils::DescriptorTypeToVkDescriptorType(binding.type),
                    .descriptorCount = binding.count,
                    .stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS,
                };
            }

            VkDescriptorSetLayoutCreateInfo layoutInfo{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .bindingCount = static_cast<uint32_t>(layoutBindings.size()),
                .pBindings = layoutBindings.data(),
            };

            if (vkCreateDescriptorSetLayout(m_Device->GetDevice(), &layoutInfo, nullptr, &m_DescriptorSetLayout) != VK_SUCCESS) {
                m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan descriptor set layout.");
                return Result::Failed;
            }

            pipelineLayoutInfo.setLayoutCount = 1;
            pipelineLayoutInfo.pSetLayouts = &m_DescriptorSetLayout;
        }

        if (vkCreatePipelineLayout(m_Device->GetDevice(), &pipelineLayoutInfo, nullptr, &m_PipelineLayout) != VK_SUCCESS) {
            m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan pipeline layout.");
            return Result::Failed;
        }

        const RenderTargetFormats& formats = desc.renderTargetFormats;
        std::vector<VkFormat> vkColorFormats(formats.colorFormats.size());
        for (uint32_t i = 0; i < formats.colorFormats.size(); i++) {
            vkColorFormats[i] = Utils::TextureFormatToVkFormat(formats.colorFormats[i]);
        }

        VkPipelineRenderingCreateInfo pipelineRenderingInfo{
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

        if (vkCreateGraphicsPipelines(m_Device->GetDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_Pipeline) != VK_SUCCESS) {
            m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan graphics pipelines.");
            return Result::Failed;
        }

        return Result::Success;
    }
}
