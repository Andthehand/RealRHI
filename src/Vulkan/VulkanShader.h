#pragma once
#include "Shader.h"
#include "ShaderDesc.h"
#include "VulkanDevice.h"

#include <filesystem>
#include <slang/slang.h>
#include <slang/slang-com-ptr.h>

namespace RealRHI {
	class VulkanShader : public Shader {
	public:
		VulkanShader(const VulkanDevice* device, const ShaderDesc& desc);
		virtual ~VulkanShader();

		VkShaderModule GetShaderModule() const { return m_ShaderModule; }
		const std::vector<EntryPoint>& GetEntryPoints() const { return m_EntryPoints; }
	private:
		static void InitializeSlang(const char* shaderDirectory, bool isDebugEnabled);

		bool CheckSlangDiagnostics(const Slang::ComPtr<slang::IBlob> diagnostics) const;
	private:
		const VulkanDevice* m_Device;
		Slang::ComPtr<slang::IModule> m_SlangModule;
		VkShaderModule m_ShaderModule;

		std::vector<EntryPoint> m_EntryPoints; // Used by the pipeline

		inline static Slang::ComPtr<slang::IGlobalSession> s_SlangGlobalSession;
		inline static Slang::ComPtr<slang::ISession> s_SlangSession;
	};
}
