#include "VulkanGraphicsState.h"

#include "Detail/CpuProfiling.h"
#include "VulkanConversions.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
namespace RealRHI {
	Result VulkanGraphicsState::BuildVertexInputDescriptions(const VertexLayout& layout,
		const Shader* shader,
		std::vector<VkVertexInputBindingDescription>& bindingDescriptions,
		std::vector<VkVertexInputAttributeDescription>& attributeDescriptions) const {
		auto reject = [&](const std::string& reason) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, ("Graphics state: " + reason).c_str());
			return Result::InvalidParameter;
		};

		const auto& limits = m_Device->GetCapabilities().limits;
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

			if (!m_Device->SupportsVertexFormat(attribute.format)) {
				return reject("Vertex attribute format is unsupported by this device.");
			}

			attributeDescriptions.push_back({attribute.location, attribute.binding, format.format, attribute.offset});
		}

		if (shader) {
			for (const auto& input : shader->GetReflection().vertexInputs) {
				const auto attribute =
					std::find_if(layout.attributes.begin(), layout.attributes.end(), [&](const auto& value) {
						return value.location == input.location;
					});

				if (attribute == layout.attributes.end()) {
					return reject("Missing vertex attribute for " + input.name + " at location " +
						std::to_string(input.location));
				}

				const auto format = Utils::GetVertexFormatInfo(attribute->format);
				if (format.scalarType != input.scalarType || format.components != input.componentCount) {
					return reject("Vertex format does not match shader input " + input.name);
				}
			}
		}

		return Result::Success;
	}

	Result VulkanGraphicsState::BuildColorBlendDescriptions(const DrawDescription& desc,
		std::vector<VkPipelineColorBlendAttachmentState>& attachments) const {
		auto reject = [&](const std::string& reason) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, ("Graphics state: " + reason).c_str());
			return Result::InvalidParameter;
		};

		const size_t count = desc.renderTargetFormats.colorFormats.size();
		if (!desc.colorBlends.empty() && desc.colorBlends.size() != count) {
			return reject("Provide exactly one colorBlends entry per color target, or leave it empty to use blend.");
		}

		if (count > m_Device->GetCapabilities().limits.maxColorAttachments) {
			return reject("Color target count exceeds maxColorAttachments.");
		}

		for (float value : desc.blendConstants) {
			if (!std::isfinite(value)) {
				return reject("Blend constants must be finite.");
			}
		}

		for (size_t i = 0; i < count; ++i) {
			const BlendState* state = &desc.blend;
			if (!desc.colorBlends.empty()) {
				state = &desc.colorBlends[i];

				if (!m_Device->SupportsIndependentBlend() && *state != desc.colorBlends[0]) {
					return reject(
						"Different color target blend states or write masks require independentBlend support.");
				}
			}

			VkPipelineColorBlendAttachmentState attachment{
				.blendEnable = state->enable,
				.srcColorBlendFactor = Utils::BlendFactorToVkBlendFactor(state->sourceColorFactor),
				.dstColorBlendFactor = Utils::BlendFactorToVkBlendFactor(state->destinationColorFactor),
				.colorBlendOp = Utils::BlendOperationToVkBlendOp(state->colorOperation),
				.srcAlphaBlendFactor = Utils::BlendFactorToVkBlendFactor(state->sourceAlphaFactor),
				.dstAlphaBlendFactor = Utils::BlendFactorToVkBlendFactor(state->destinationAlphaFactor),
				.alphaBlendOp = Utils::BlendOperationToVkBlendOp(state->alphaOperation),
				.colorWriteMask = static_cast<VkColorComponentFlags>(state->writeMask),
			};

			if (attachment.srcColorBlendFactor == VK_BLEND_FACTOR_MAX_ENUM ||
				attachment.dstColorBlendFactor == VK_BLEND_FACTOR_MAX_ENUM ||
				attachment.srcAlphaBlendFactor == VK_BLEND_FACTOR_MAX_ENUM ||
				attachment.dstAlphaBlendFactor == VK_BLEND_FACTOR_MAX_ENUM ||
				attachment.colorBlendOp == VK_BLEND_OP_MAX_ENUM || attachment.alphaBlendOp == VK_BLEND_OP_MAX_ENUM ||
				(attachment.colorWriteMask & ~static_cast<VkColorComponentFlags>(ColorWriteMask::All)) != 0) {
				return reject("Invalid blend factor, operation, or write mask for color target " + std::to_string(i));
			}

			if (desc.renderTargetFormats.colorFormats[i] != TextureFormat::Unknown && state->enable &&
				!m_Device->GetTextureFormatSupport(desc.renderTargetFormats.colorFormats[i]).colorBlending) {
				return reject("Blending is unsupported for color target " + std::to_string(i) + "'s format.");
			}

			attachments.push_back(attachment);
		}

		return Result::Success;
	}

	Result VulkanGraphicsState::Prepare(const DrawDescription& desc) {
		REALRHI_CPU_ZONE("RealRHI: Prepare graphics state");
		m_Bindings.clear();
		m_Attributes.clear();
		m_VertexBindings.clear();
		m_BlendEnables.clear();
		m_BlendEquations.clear();
		m_ColorWriteMasks.clear();
		const auto* shader = desc.shader;
		const auto result = ValidateState(desc);
		if (result != Result::Success) {
			return result;
		}
		auto reject = [&](const char* reason) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, (std::string("Graphics state: ") + reason).c_str());
			return Result::InvalidParameter;
		};
		const auto samples = desc.multisample.sampleCount;
		for (const auto format : desc.renderTargetFormats.colorFormats) {
			const auto support = m_Device->GetTextureFormatSupport(format);
			if (!(support.colorAttachmentSampleCounts & samples)) {
				return reject("Color format does not support the requested attachment sample count.");
			}
		}

		const auto depthFormatValue = desc.renderTargetFormats.depthFormat;
		const auto aspects = Utils::TextureFormatToVkImageAspect(depthFormatValue);
		if (depthFormatValue != TextureFormat::Unknown &&
			!(m_Device->GetTextureFormatSupport(depthFormatValue).depthStencilAttachmentSampleCounts & samples)) {
			return reject("Depth/stencil format does not support the requested attachment sample count.");
		}
		if (desc.renderTargetFormats.colorFormats.empty() && depthFormatValue == TextureFormat::Unknown &&
			!(m_Device->GetCapabilities().limits.noAttachmentSampleCounts & samples)) {
			return reject("Sample count is unsupported for rendering without attachments.");
		}
		if (desc.stencil.enable && !(aspects & VK_IMAGE_ASPECT_STENCIL_BIT)) {
			return reject("Stencil testing requires a depth/stencil format with a stencil aspect.");
		}

		std::vector<VkVertexInputBindingDescription> bindingDescriptions;
		std::vector<VkVertexInputAttributeDescription> attributeDescriptions;
		const VertexLayout* vertexLayout = &desc.vertexLayout;
		if (!m_ExplicitVertexLayout && shader) {
			vertexLayout = &shader->GetReflection().vertexLayout;
		}

		const Result layoutResult =
			BuildVertexInputDescriptions(*vertexLayout, shader, bindingDescriptions, attributeDescriptions);
		if (layoutResult != Result::Success) {
			return layoutResult;
		}
		for (const auto& attribute : vertexLayout->attributes) {
			if (std::find(m_VertexBindings.begin(), m_VertexBindings.end(), attribute.binding) ==
				m_VertexBindings.end()) {
				m_VertexBindings.push_back(attribute.binding);
			}
		}
		std::vector<VkPipelineColorBlendAttachmentState> blendAttachments;
		const Result blendResult = BuildColorBlendDescriptions(desc, blendAttachments);
		if (blendResult != Result::Success) {
			return blendResult;
		}
		for (const auto& binding : bindingDescriptions) {
			m_Bindings.push_back({
				.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,
				.binding = binding.binding,
				.stride = binding.stride,
				.inputRate = binding.inputRate,
				.divisor = 1,
			});
		}
		for (const auto& attribute : attributeDescriptions) {
			m_Attributes.push_back({
				.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
				.location = attribute.location,
				.binding = attribute.binding,
				.format = attribute.format,
				.offset = attribute.offset,
			});
		}
		for (const auto& blend : blendAttachments) {
			m_BlendEnables.push_back(blend.blendEnable);
			m_BlendEquations.push_back(
				{blend.srcColorBlendFactor,
					blend.dstColorBlendFactor,
					blend.colorBlendOp,
					blend.srcAlphaBlendFactor,
					blend.dstAlphaBlendFactor,
					blend.alphaBlendOp});
			m_ColorWriteMasks.push_back(blend.colorWriteMask);
		}
		m_SampleCount = samples;
		m_DepthWrites = desc.depth.depthTestEnable && desc.depth.depthWriteEnable;
		auto writesStencil = [](const StencilFaceState& face) {
			return face.writeMask != 0 &&
				(face.failOp != StencilOp::Keep || face.passOp != StencilOp::Keep ||
					face.depthFailOp != StencilOp::Keep);
		};
		m_StencilWrites =
			desc.stencil.enable && (writesStencil(desc.stencil.front) || writesStencil(desc.stencil.back));

		return Result::Success;
	}

	Result VulkanGraphicsState::ValidateState(const GraphicsState& desc) const {
		if (Utils::FillModeToVkPolygonMode(desc.raster.fillMode) == VK_POLYGON_MODE_MAX_ENUM ||
			(desc.raster.cullMode != CullMode::None && desc.raster.cullMode != CullMode::Front &&
				desc.raster.cullMode != CullMode::Back) ||
			(desc.raster.fillMode == FillMode::Wireframe && !m_Device->SupportsWireframe())) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "SetGraphicsState: Unsupported raster state.");
			return Result::InvalidParameter;
		}
		const VkPrimitiveTopology topology = Utils::PrimitiveTopologyToVkPrimitiveTopology(desc.topology);
		if (topology == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"Graphics state: Provide a supported primitive topology.");
			return Result::InvalidParameter;
		}

		if (desc.primitiveRestartEnable && desc.topology != PrimitiveTopology::LineStrip &&
			desc.topology != PrimitiveTopology::TriangleStrip && desc.topology != PrimitiveTopology::TriangleFan) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"Graphics state: Primitive restart requires a line strip, triangle strip, or triangle fan.");
			return Result::InvalidParameter;
		}

		const auto& bias = desc.raster.depthBias;
		if (!std::isfinite(bias.constantFactor) || !std::isfinite(bias.clamp) || !std::isfinite(bias.slopeFactor) ||
			(bias.enable && bias.clamp != 0.f && !m_Device->SupportsDepthBiasClamp())) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"Graphics state: Depth bias values must be finite; nonzero enabled clamp requires "
				"depthBiasClamp support.");
			return Result::InvalidParameter;
		}

		auto reject = [&](const char* reason) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, (std::string("Graphics state: ") + reason).c_str());
			return Result::InvalidParameter;
		};

		const auto samples = desc.multisample.sampleCount;
		if (Utils::CompareOpToVkCompareOp(desc.depth.compareOp) == VK_COMPARE_OP_MAX_ENUM) {
			return reject("Depth comparison must be valid.");
		}
		if (!Utils::IsValidSampleCount(samples)) {
			return reject("Sample count must be a power of two between 1 and 64.");
		}

		auto convertFace = [](const StencilFaceState& face) {
			return VkStencilOpState{
				Utils::StencilOpToVk(face.failOp),
				Utils::StencilOpToVk(face.passOp),
				Utils::StencilOpToVk(face.depthFailOp),
				Utils::CompareOpToVkCompareOp(face.compareOp),
				face.compareMask,
				face.writeMask,
				face.reference
			};
		};
		const auto front = convertFace(desc.stencil.front);
		const auto back = convertFace(desc.stencil.back);
		for (const auto& face : {front, back}) {
			if (face.failOp == VK_STENCIL_OP_MAX_ENUM || face.passOp == VK_STENCIL_OP_MAX_ENUM ||
				face.depthFailOp == VK_STENCIL_OP_MAX_ENUM || face.compareOp == VK_COMPARE_OP_MAX_ENUM) {
				return reject("Stencil faces require valid operations and comparisons.");
			}
		}

		DrawDescription blendDesc;
		static_cast<GraphicsState&>(blendDesc) = desc;
		blendDesc.renderTargetFormats.colorFormats.resize(
			std::max(size_t{1}, desc.colorBlends.size()), TextureFormat::Unknown);
		std::vector<VkPipelineColorBlendAttachmentState> blends;
		return BuildColorBlendDescriptions(blendDesc, blends);
	}
	Result VulkanGraphicsState::SetState(const GraphicsState& state) {
		if (m_HasState && m_State == state) {
			return Result::Success;
		}
		const auto result = ValidateState(state);
		if (result != Result::Success) {
			return result;
		}
		if (!m_HasState || m_State != state) {
			m_State = state;
			m_Dirty = true;
		}
		m_HasState = true;
		return Result::Success;
	}
	Result VulkanGraphicsState::SetVertexLayout(const VertexLayout& layout) {
		if (m_ExplicitVertexLayout && m_Layout == layout) {
			return Result::Success;
		}
		std::vector<VkVertexInputBindingDescription> bindings;
		std::vector<VkVertexInputAttributeDescription> attributes;
		const auto result = BuildVertexInputDescriptions(layout, nullptr, bindings, attributes);
		if (result != Result::Success) {
			return result;
		}
		if (!m_ExplicitVertexLayout || !(m_Layout == layout)) {
			m_Layout = layout;
			m_Dirty = true;
		}
		m_ExplicitVertexLayout = true;
		return Result::Success;
	}
	bool VulkanGraphicsState::ValidateAndApply(VkCommandBuffer commands,
		const VulkanShader* shader,
		const VulkanRenderingFormats& formats,
		uint32_t samples,
		bool readOnlyDepth,
		bool readOnlyStencil,
		const char* operation) {
		auto reject = [&](const char* reason) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, (std::string(operation) + ": " + reason).c_str());
			return false;
		};
		if (!m_HasState) {
			return reject("Set graphics state before drawing.");
		}
		if (m_Dirty || shader != m_ValidatedShader || formats.colorFormats != m_ValidatedFormats.colorFormats ||
			formats.depthFormat != m_ValidatedFormats.depthFormat) {
			DrawDescription desc;
			static_cast<GraphicsState&>(desc) = m_State;
			desc.shader = shader;
			desc.vertexLayout = m_Layout;
			desc.renderTargetFormats = formats;
			if (Prepare(desc) != Result::Success) {
				return false;
			}
			m_EffectiveLayout = m_ExplicitVertexLayout ? m_Layout : shader->GetReflection().vertexLayout;
			m_ValidatedShader = shader;
			m_ValidatedFormats = formats;
			m_Dirty = false;
		}
		if (samples && m_SampleCount != samples) {
			return reject("Graphics sample count must match the source attachments.");
		}
		if (readOnlyDepth && m_DepthWrites) {
			return reject("Depth writes require a writable depth attachment.");
		}
		if (readOnlyStencil && m_StencilWrites) {
			return reject("Stencil writes require a writable stencil attachment.");
		}
		Apply(commands);
		return true;
	}
	void VulkanGraphicsState::Apply(VkCommandBuffer commands) {
		const auto& state = m_State;
		const auto* previous = m_AppliedState ? &*m_AppliedState : nullptr;
		// Initialize invariant state once per recording. Compute bindings do not disturb graphics state.
		if (!previous) {
			vkCmdSetRasterizerDiscardEnable(commands, VK_FALSE);
			vkCmdSetDepthClampEnableEXT(commands, VK_FALSE);
			vkCmdSetDepthBoundsTestEnable(commands, VK_FALSE);
			vkCmdSetLogicOpEnableEXT(commands, VK_FALSE);
			vkCmdSetAlphaToOneEnableEXT(commands, VK_FALSE);
			vkCmdSetLineWidth(commands, 1.f);
		}
		if (!m_AppliedLayout || m_EffectiveLayout != *m_AppliedLayout) {
			vkCmdSetVertexInputEXT(commands,
				static_cast<uint32_t>(m_Bindings.size()),
				m_Bindings.data(),
				static_cast<uint32_t>(m_Attributes.size()),
				m_Attributes.data());
			m_AppliedLayout = m_EffectiveLayout;
		}
		if (!previous || state.topology != previous->topology) {
			vkCmdSetPrimitiveTopology(commands, Utils::PrimitiveTopologyToVkPrimitiveTopology(state.topology));
		}
		if (!previous || state.primitiveRestartEnable != previous->primitiveRestartEnable) {
			vkCmdSetPrimitiveRestartEnable(commands, state.primitiveRestartEnable);
		}
		if (!previous || state.raster != previous->raster) {
			const auto& raster = state.raster;
			vkCmdSetPolygonModeEXT(commands, Utils::FillModeToVkPolygonMode(raster.fillMode));
			vkCmdSetCullMode(commands, Utils::CullModeToVkCullMode(raster.cullMode));
			vkCmdSetFrontFace(commands, Utils::FrontFaceToVkFrontFace(raster.frontCounterClockwise));
			vkCmdSetDepthBiasEnable(commands, raster.depthBias.enable);
			if (raster.depthBias.enable) {
				vkCmdSetDepthBias(
					commands, raster.depthBias.constantFactor, raster.depthBias.clamp, raster.depthBias.slopeFactor);
			}
		}
		if (!previous || state.depth != previous->depth) {
			vkCmdSetDepthTestEnable(commands, state.depth.depthTestEnable);
			vkCmdSetDepthWriteEnable(commands, state.depth.depthWriteEnable);
			vkCmdSetDepthCompareOp(commands, Utils::CompareOpToVkCompareOp(state.depth.compareOp));
		}
		if (!previous || state.stencil != previous->stencil) {
			vkCmdSetStencilTestEnable(commands, state.stencil.enable);
			auto setFace = [&](VkStencilFaceFlags face, const StencilFaceState& stencil) {
				vkCmdSetStencilOp(commands,
					face,
					Utils::StencilOpToVk(stencil.failOp),
					Utils::StencilOpToVk(stencil.passOp),
					Utils::StencilOpToVk(stencil.depthFailOp),
					Utils::CompareOpToVkCompareOp(stencil.compareOp));
				vkCmdSetStencilCompareMask(commands, face, stencil.compareMask);
				vkCmdSetStencilWriteMask(commands, face, stencil.writeMask);
				vkCmdSetStencilReference(commands, face, stencil.reference);
			};
			setFace(VK_STENCIL_FACE_FRONT_BIT, state.stencil.front);
			setFace(VK_STENCIL_FACE_BACK_BIT, state.stencil.back);
		}
		if (!previous || state.multisample != previous->multisample) {
			const auto& multisample = state.multisample;
			const auto samples = static_cast<VkSampleCountFlagBits>(multisample.sampleCount);
			const VkSampleMask mask[]{
				static_cast<uint32_t>(multisample.sampleMask), static_cast<uint32_t>(multisample.sampleMask >> 32)
			};
			vkCmdSetRasterizationSamplesEXT(commands, samples);
			vkCmdSetSampleMaskEXT(commands, samples, mask);
			vkCmdSetAlphaToCoverageEnableEXT(commands, multisample.alphaToCoverageEnable);
		}
		if (!previous || state.blend != previous->blend || state.colorBlends != previous->colorBlends ||
			m_BlendEnables.size() != m_AppliedColorCount) {
			const auto count = static_cast<uint32_t>(m_BlendEnables.size());
			if (count) {
				vkCmdSetColorBlendEnableEXT(commands, 0, count, m_BlendEnables.data());
				vkCmdSetColorBlendEquationEXT(commands, 0, count, m_BlendEquations.data());
				vkCmdSetColorWriteMaskEXT(commands, 0, count, m_ColorWriteMasks.data());
			}
		}
		if (!previous || state.blendConstants != previous->blendConstants) {
			vkCmdSetBlendConstants(commands, state.blendConstants.data());
		}
		m_AppliedState = m_State;
		m_AppliedColorCount = m_BlendEnables.size();
	}
}
