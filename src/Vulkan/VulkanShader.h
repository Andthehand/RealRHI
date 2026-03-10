#pragma once
#include "Shader.h"
#include "ShaderDesc.h"
#include "VulkanDevice.h"
#include "Result.h"

#include <filesystem>

namespace RealRHI {
	class VulkanShader : public Shader {
	public:
		static Result Create(const VulkanDevice* device, const ShaderDesc& desc, Ref<Shader>& outShader);
		virtual ~VulkanShader();

		VkShaderModule GetShaderModule() const { return m_ShaderModule; }
		const std::vector<EntryPoint>& GetEntryPoints() const { return m_EntryPoints; }
	private:
		static void InitializeSlang(const char* shaderDirectory, bool isDebugEnabled);

		static bool CheckSlangDiagnostics(const VulkanDevice* device, const Slang::ComPtr<slang::IBlob>& diagnostics);
	private:
		VulkanShader(const VulkanDevice* device, Slang::ComPtr<slang::IModule> slangModule, VkShaderModule shaderModule, std::vector<EntryPoint> entryPoints);

		const VulkanDevice* m_Device;
		Slang::ComPtr<slang::IModule> m_SlangModule;
		VkShaderModule m_ShaderModule;

		std::vector<EntryPoint> m_EntryPoints;

		inline static Slang::ComPtr<slang::IGlobalSession> s_SlangGlobalSession;
		inline static Slang::ComPtr<slang::ISession> s_SlangSession;
	};
}
