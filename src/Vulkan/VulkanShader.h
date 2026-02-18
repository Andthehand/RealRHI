#pragma once
#include "Shader.h"
#include "VulkanDevice.h"

#include <filesystem>
#include <slang/slang.h>
#include <slang/slang-com-ptr.h>

namespace RealRHI {
	class VulkanShader : public Shader {
	public:
		VulkanShader(const VulkanDevice* device, const char* moduleName);
		virtual ~VulkanShader();

		VkShaderModule GetShaderModule() const { return m_ShaderModule; }
	private:
		static void InitializeSlang(const char* shaderDirectory);

		bool CheckSlangDiagnostics(const Slang::ComPtr<slang::IBlob> diagnostics) const;
	private:
		const VulkanDevice* m_Device;
		Slang::ComPtr<slang::IModule> m_SlangModule;
		VkShaderModule m_ShaderModule;

		inline static Slang::ComPtr<slang::IGlobalSession> s_SlangGlobalSession;
		inline static Slang::ComPtr<slang::ISession> s_SlangSession;
	};
}
