#include "VulkanPipeline.h"

#include "VulkanConversions.h"
#include "VulkanCpuTimer.h"
#include "VulkanShader.h"

#include <algorithm>
#include <unordered_set>

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

		const auto& entryPoints = shader->GetReflection().entryPoints;
		if (entryPoints.size() != 1 || entryPoints[0].stage != ShaderStage::Compute) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"Compute pipeline requires exactly one compute shader entry point.");
			return Result::Failed;
		}

		const VkPipelineShaderStageCreateInfo stage{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = shader->GetShaderModule(),
			.pName = shader->GetExportedEntryPoints()[0].c_str(),
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

	Result VulkanPipeline::BuildVertexInputDescriptions(const VertexLayout& layout,
		const Shader& shader,
		std::vector<VkVertexInputBindingDescription>& bindingDescriptions,
		std::vector<VkVertexInputAttributeDescription>& attributeDescriptions) const {
		auto reject = [&](const std::string& reason) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, ("CreateGraphicsPipeline: " + reason).c_str());
			return Result::InvalidParameter;
		};

		const auto& entryPoints = shader.GetReflection().entryPoints;
		if (entryPoints.empty() || std::any_of(entryPoints.begin(), entryPoints.end(), [](const auto& entry) {
				return entry.stage == ShaderStage::Compute;
			})) {
			return reject("Requires a graphics shader program.");
		}

		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(m_Device->GetPhysicalDevice(), &properties);

		const auto& limits = properties.limits;
		if (layout.bindings.size() > limits.maxVertexInputBindings ||
			layout.attributes.size() > limits.maxVertexInputAttributes) {
			return reject("Vertex layout exceeds the device binding or attribute limits.");
		}

		std::unordered_set<uint32_t> bindings;
		for (const auto& binding : layout.bindings) {
			if (!bindings.insert(binding.binding).second || binding.binding >= limits.maxVertexInputBindings ||
				binding.stride == 0 || binding.stride > limits.maxVertexInputBindingStride ||
				(binding.inputRate != VertexInputRate::Vertex && binding.inputRate != VertexInputRate::Instance)) {
				return reject(
					"Vertex bindings require unique valid indices, nonzero supported strides, and valid input rates.");
			}

			VkVertexInputRate rate = VK_VERTEX_INPUT_RATE_VERTEX;
			if (binding.inputRate == VertexInputRate::Instance) {
				rate = VK_VERTEX_INPUT_RATE_INSTANCE;
			}

			bindingDescriptions.push_back({binding.binding, binding.stride, rate});
		}

		std::unordered_set<uint32_t> locations;
		for (const auto& attribute : layout.attributes) {
			const auto format = Utils::GetVertexFormatInfo(attribute.format);
			const auto binding = std::find_if(layout.bindings.begin(), layout.bindings.end(), [&](const auto& value) {
				return value.binding == attribute.binding;
			});

			if (!locations.insert(attribute.location).second || attribute.location >= limits.maxVertexInputAttributes ||
				binding == layout.bindings.end() || format.format == VK_FORMAT_UNDEFINED ||
				attribute.offset > limits.maxVertexInputAttributeOffset || attribute.offset > binding->stride ||
				format.size > binding->stride - attribute.offset) {
				return reject("Vertex attributes require unique valid locations, existing bindings, and valid formats "
							  "fitting their strides.");
			}

			VkFormatProperties support{};
			vkGetPhysicalDeviceFormatProperties(m_Device->GetPhysicalDevice(), format.format, &support);
			if (!(support.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
				return reject("Vertex attribute format is unsupported by this device.");
			}

			attributeDescriptions.push_back({attribute.location, attribute.binding, format.format, attribute.offset});
		}

		for (const auto& input : shader.GetReflection().vertexInputs) {
			const auto attribute =
				std::find_if(layout.attributes.begin(), layout.attributes.end(), [&](const auto& value) {
					return value.location == input.location;
				});

			if (attribute == layout.attributes.end()) {
				return reject(
					"Missing vertex attribute for " + input.name + " at location " + std::to_string(input.location));
			}

			const auto format = Utils::GetVertexFormatInfo(attribute->format);
			if (format.scalarType != input.scalarType || format.components != input.componentCount) {
				return reject("Vertex format does not match shader input " + input.name);
			}
		}

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

		const std::vector<ShaderEntryPoint>& entryPoints = shader->GetReflection().entryPoints;
		std::vector<VkPipelineShaderStageCreateInfo> shaderStages(entryPoints.size());
		for (uint32_t i = 0; i < entryPoints.size(); i++) {
			shaderStages[i] = VkPipelineShaderStageCreateInfo{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = Utils::ShaderStageToVkShaderStage(entryPoints[i].stage),
				.module = shaderModule,
				.pName = shader->GetExportedEntryPoints()[i].c_str(),
			};
		}

		std::vector<VkVertexInputBindingDescription> bindingDescriptions;
		std::vector<VkVertexInputAttributeDescription> attributeDescriptions;
		const VertexLayout* vertexLayout = &desc.vertexLayout;
		if (vertexLayout->bindings.empty() && vertexLayout->attributes.empty()) {
			vertexLayout = &shader->GetReflection().vertexLayout;
		}

		const Result layoutResult =
			BuildVertexInputDescriptions(*vertexLayout, *shader, bindingDescriptions, attributeDescriptions);
		if (layoutResult != Result::Success) {
			return layoutResult;
		}
		for (const auto& attribute : vertexLayout->attributes) {
			if (std::find(m_VertexBindings.begin(), m_VertexBindings.end(), attribute.binding) ==
				m_VertexBindings.end()) {
				m_VertexBindings.push_back(attribute.binding);
			}
		}
		VkPipelineVertexInputStateCreateInfo vertexInputInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
			.vertexBindingDescriptionCount = static_cast<uint32_t>(bindingDescriptions.size()),
			.pVertexBindingDescriptions = bindingDescriptions.data(),
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
