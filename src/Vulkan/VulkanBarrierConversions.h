#pragma once

#include "BarrierDesc.h"

#include <volk.h>

namespace RealRHI::Utils {
	constexpr VkPipelineStageFlags2 ToVkPipelineStages(PipelineStage stages) {
		const uint64_t bits = static_cast<uint64_t>(stages);
		VkPipelineStageFlags2 result = VK_PIPELINE_STAGE_2_NONE;
		if (bits & static_cast<uint64_t>(PipelineStage::AllCommands)) result |= VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		if (bits & static_cast<uint64_t>(PipelineStage::VertexShader)) result |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
		if (bits & static_cast<uint64_t>(PipelineStage::FragmentShader)) result |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
		if (bits & static_cast<uint64_t>(PipelineStage::ComputeShader)) result |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
		if (bits & static_cast<uint64_t>(PipelineStage::ColorAttachmentOutput)) result |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		if (bits & static_cast<uint64_t>(PipelineStage::EarlyFragmentTests)) result |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
		if (bits & static_cast<uint64_t>(PipelineStage::LateFragmentTests)) result |= VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
		if (bits & static_cast<uint64_t>(PipelineStage::Transfer)) result |= VK_PIPELINE_STAGE_2_TRANSFER_BIT;
		if (bits & static_cast<uint64_t>(PipelineStage::DrawIndirect)) result |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
		if (bits & static_cast<uint64_t>(PipelineStage::VertexInput)) result |= VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
		return result;
	}

	constexpr VkAccessFlags2 ToVkResourceAccess(ResourceAccess access) {
		const uint64_t bits = static_cast<uint64_t>(access);
		VkAccessFlags2 result = VK_ACCESS_2_NONE;
		if (bits & static_cast<uint64_t>(ResourceAccess::MemoryRead)) result |= VK_ACCESS_2_MEMORY_READ_BIT;
		if (bits & static_cast<uint64_t>(ResourceAccess::MemoryWrite)) result |= VK_ACCESS_2_MEMORY_WRITE_BIT;
		if (bits & static_cast<uint64_t>(ResourceAccess::ShaderRead)) result |= VK_ACCESS_2_SHADER_READ_BIT;
		if (bits & static_cast<uint64_t>(ResourceAccess::ShaderWrite)) result |= VK_ACCESS_2_SHADER_WRITE_BIT;
		if (bits & static_cast<uint64_t>(ResourceAccess::ColorAttachmentRead)) result |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
		if (bits & static_cast<uint64_t>(ResourceAccess::ColorAttachmentWrite)) result |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		if (bits & static_cast<uint64_t>(ResourceAccess::DepthStencilRead)) result |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
		if (bits & static_cast<uint64_t>(ResourceAccess::DepthStencilWrite)) result |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		if (bits & static_cast<uint64_t>(ResourceAccess::TransferRead)) result |= VK_ACCESS_2_TRANSFER_READ_BIT;
		if (bits & static_cast<uint64_t>(ResourceAccess::TransferWrite)) result |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
		if (bits & static_cast<uint64_t>(ResourceAccess::IndexRead)) result |= VK_ACCESS_2_INDEX_READ_BIT;
		if (bits & static_cast<uint64_t>(ResourceAccess::VertexAttributeRead)) result |= VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
		if (bits & static_cast<uint64_t>(ResourceAccess::IndirectCommandRead)) result |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
		return result;
	}
}
