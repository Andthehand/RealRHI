#include "VulkanShader.h"

#include "../Slang/SlangShaderCompiler.h"
#include "VulkanConversions.h"

namespace RealRHI {
	VulkanShader::VulkanShader(const VulkanDevice* device)
		: m_Device(device) { }

	VulkanShader::~VulkanShader() {
		for (const auto shader : m_Shaders) {
			vkDestroyShaderEXT(m_Device->GetDevice(), shader, nullptr);
		}
		for (uint32_t i = 0; i < m_CountedStages; ++i) {
			m_Device->RecordResourceDestroyed(VulkanResourceKind::Shader);
		}
	}

	Result VulkanShader::Create(const VulkanDevice* device,
		const CompiledShaderProgram& program,
		const char* debugName,
		Ref<VulkanShader>& outShader) {
		auto shader = Ref<VulkanShader>::Create(device);
		const Result result = shader->Init(program, debugName);
		if (result == Result::Success) {
			outShader = shader;
		}
		return result;
	}

	Result VulkanShader::Init(const CompiledShaderProgram& program, const char* debugName) {
		const auto& push = program.reflection.pushData;
		if (push.offset > m_Device->GetMaxPushDataSize() || push.size > m_Device->GetMaxPushDataSize() - push.offset) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::ShaderCompilation,
				"CreateShader: Reflected push-data range exceeds descriptor heap maxPushDataSize.");
			return Result::Failed;
		}

		const auto& entries = program.reflection.entryPoints;
		if (entries.empty() || entries.size() != program.exportedEntryPoints.size()) {
			return Result::InvalidParameter;
		}
		bool hasFragment = false;
		for (const auto& entry : entries) {
			hasFragment |= entry.stage == ShaderStage::Fragment;
		}
		std::vector<VkShaderCreateInfoEXT> infos;
		for (size_t i = 0; i < entries.size(); ++i) {
			const auto stage = Utils::ShaderStageToVkShaderStage(entries[i].stage);
			m_Stages.push_back(stage);
			infos.push_back({
				.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
				.flags = VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT |
					(stage != VK_SHADER_STAGE_COMPUTE_BIT ? VK_SHADER_CREATE_LINK_STAGE_BIT_EXT : 0u),
				.stage = stage,
				.nextStage = stage == VK_SHADER_STAGE_VERTEX_BIT && hasFragment ? VK_SHADER_STAGE_FRAGMENT_BIT : 0u,
				.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
				.codeSize = program.spirv.size() * sizeof(uint32_t),
				.pCode = program.spirv.data(),
				.pName = program.exportedEntryPoints[i].c_str(),
			});
		}
		m_Shaders.resize(infos.size(), VK_NULL_HANDLE);
		if (vkCreateShadersEXT(
				m_Device->GetDevice(), static_cast<uint32_t>(infos.size()), infos.data(), nullptr, m_Shaders.data()) !=
			VK_SUCCESS) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CreateShader: Failed to create Vulkan shader objects.");
			return Result::Failed;
		}

		for (size_t i = 0; i < m_Shaders.size(); ++i) {
			m_Device->RecordResourceCreated(VulkanResourceKind::Shader);
			++m_CountedStages;
		}
		m_Reflection = program.reflection;
		for (const auto shader : m_Shaders) {
			const auto result = m_Device->SetDebugName(VK_OBJECT_TYPE_SHADER_EXT, shader, debugName);
			if (result != Result::Success) {
				return result;
			}
		}
		return Result::Success;
	}

	void VulkanShader::Bind(VkCommandBuffer commands) const {
		// Both graphics stages must be explicitly bound, including an absent fragment stage.
		if (m_Stages[0] != VK_SHADER_STAGE_COMPUTE_BIT) {
			const VkShaderStageFlagBits stages[]{VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
			VkShaderEXT shaders[]{VK_NULL_HANDLE, VK_NULL_HANDLE};
			for (size_t i = 0; i < m_Stages.size(); ++i) {
				shaders[m_Stages[i] == VK_SHADER_STAGE_VERTEX_BIT ? 0 : 1] = m_Shaders[i];
			}
			vkCmdBindShadersEXT(commands, 2, stages, shaders);
		}
		else {
			vkCmdBindShadersEXT(commands, static_cast<uint32_t>(m_Stages.size()), m_Stages.data(), m_Shaders.data());
		}
	}
}
