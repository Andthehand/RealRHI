#pragma once
#include "Result.h"
#include "Shader.h"
#include "ShaderDesc.h"
#include "VulkanDevice.h"

namespace RealRHI {
	class VulkanShader : public Shader {
	public:
		VulkanShader(const VulkanDevice* device);
		~VulkanShader();

		static Result Create(const VulkanDevice* device, const ShaderDesc& desc, Ref<VulkanShader>& outShader);
		Result Init(const ShaderDesc& desc);
		
		const ShaderReflection& GetReflection() const override { return m_Reflection; }
		VkShaderModule GetShaderModule() const { return m_ShaderModule; }
		const std::vector<std::string>& GetExportedEntryPoints() const { return m_ExportedEntryPoints; }

	private:
		const VulkanDevice* m_Device = nullptr;

		VkShaderModule m_ShaderModule = VK_NULL_HANDLE;
		ShaderReflection m_Reflection;
		std::vector<std::string> m_ExportedEntryPoints;
	};
}
