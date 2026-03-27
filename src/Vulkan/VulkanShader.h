#pragma once
#include "Shader.h"
#include "ShaderDesc.h"
#include "DescriptorsDesc.h"
#include "VulkanDevice.h"
#include "Result.h"

#include <filesystem>

namespace RealRHI {
	class VulkanShader : public Shader {
	public:
		VulkanShader(const VulkanDevice* device);
		~VulkanShader();

		static Result Create(const VulkanDevice* device, const ShaderDesc& desc, Ref<VulkanShader>& outShader);
		Result Init(const ShaderDesc& desc);

	protected:
		friend class VulkanPipeline;
		VkShaderModule GetShaderModule() const { return m_ShaderModule; }
		const std::vector<EntryPoint>& GetEntryPoints() const { return m_EntryPoints; }
		const BufferLayout& GetBufferLayout() const { return m_BufferLayout; }
		const DescriptorsDesc& GetDescriptorsDesc() const { return m_DescriptorsDesc; }
	private:
		Slang::ComPtr<slang::IModule> LoadModule(
			const ShaderDesc& desc,
			Slang::ComPtr<slang::IBlob>& diagnostics);

		Slang::ComPtr<slang::IComponentType> ComposeProgram(
			slang::IModule* module,
			Slang::ComPtr<slang::IBlob>& diagnostics);

		BufferLayout ReflectLayout(
			slang::IComponentType* composedProgram,
			std::vector<EntryPoint>& outEntryPoints,
			Slang::ComPtr<slang::IBlob>& diagnostics);

		DescriptorsDesc ReflectDescriptors(
			slang::IComponentType* composedProgram,
			Slang::ComPtr<slang::IBlob>& diagnostics);

		Slang::ComPtr<slang::IComponentType> LinkProgram(
			slang::IComponentType* composed,
			Slang::ComPtr<slang::IBlob>& diagnostics);

		VkShaderModule CreateVkShaderModule(
			slang::IComponentType* linkedProgram);

		std::vector<BufferAttribute> CalculateCumulativeOffset(slang::TypeLayoutReflection* typeReflection, uint32_t* offset);

		static void InitializeSlang(const char* shaderDirectory, bool isDebugEnabled);

		bool CheckSlangDiagnostics(const Slang::ComPtr<slang::IBlob>& diagnostics);
	private:
		const VulkanDevice* m_Device = nullptr;
		Slang::ComPtr<slang::IModule> m_SlangModule;
		VkShaderModule m_ShaderModule = VK_NULL_HANDLE;

		std::vector<EntryPoint> m_EntryPoints;
		BufferLayout m_BufferLayout;
		DescriptorsDesc m_DescriptorsDesc;

		inline static Slang::ComPtr<slang::IGlobalSession> s_SlangGlobalSession;
		inline static Slang::ComPtr<slang::ISession> s_SlangSession;
	};
}
