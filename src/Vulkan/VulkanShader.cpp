#include "VulkanShader.h"

#include "../Slang/SlangShaderCompiler.h"

namespace RealRHI {
	VulkanShader::VulkanShader(const VulkanDevice* device)
		: m_Device(device) { }

	VulkanShader::~VulkanShader() {
		vkDestroyShaderModule(m_Device->GetDevice(), m_ShaderModule, nullptr);
	}

	Result VulkanShader::Create(const VulkanDevice* device, const ShaderDesc& desc, Ref<VulkanShader>& outShader) {
		auto shader = Ref<VulkanShader>::Create(device);
		const Result result = shader->Init(desc);
		if (result == Result::Success) {
			outShader = shader;
		}
		return result;
	}

	Result VulkanShader::Init(const ShaderDesc& desc) {
		CompiledShaderProgram program;
		const Result result = m_Device->GetShaderCompiler().Compile(desc, program);
		if (result != Result::Success) {
			return result;
		}

		const auto& push = program.reflection.pushData;
		if (push.offset > m_Device->GetMaxPushDataSize() || push.size > m_Device->GetMaxPushDataSize() - push.offset) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::ShaderCompilation,
				"CreateShader: Reflected push-data range exceeds descriptor heap maxPushDataSize.");
			return Result::Failed;
		}

		const VkShaderModuleCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			.codeSize = program.spirv.size() * sizeof(uint32_t),
			.pCode = program.spirv.data(),
		};
		if (vkCreateShaderModule(m_Device->GetDevice(), &info, nullptr, &m_ShaderModule) != VK_SUCCESS) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CreateShader: Failed to create Vulkan shader module.");
			return Result::Failed;
		}

		m_Reflection = std::move(program.reflection);
		m_ExportedEntryPoints = std::move(program.exportedEntryPoints);

		return m_Device->SetDebugName(VK_OBJECT_TYPE_SHADER_MODULE, m_ShaderModule, desc.debugName);
	}
}
