#pragma once
#include "Shader.h"
#include "ShaderDesc.h"
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
	private:
		static Slang::ComPtr<slang::IModule> LoadModule(
			const VulkanDevice* device,
			const ShaderDesc& desc,
			Slang::ComPtr<slang::IBlob>& diagnostics);

		static Slang::ComPtr<slang::IComponentType> ComposeProgram(
			const VulkanDevice* device,
			slang::IModule* module,
			Slang::ComPtr<slang::IBlob>& diagnostics);

		static BufferLayout ReflectLayout(
			const VulkanDevice* device,
			slang::IComponentType* composedProgram,
			std::vector<EntryPoint>& outEntryPoints,
			Slang::ComPtr<slang::IBlob>& diagnostics);

		static Slang::ComPtr<slang::IComponentType> LinkProgram(
			const VulkanDevice* device,
			slang::IModule* module,
			Slang::ComPtr<slang::IBlob>& diagnostics);

		static VkShaderModule CreateVkShaderModule(
			const VulkanDevice* device,
			slang::IComponentType* linkedProgram);

		static std::vector<BufferAttribute> CalculateCumulativeOffset(slang::TypeLayoutReflection* typeReflection, uint32_t* offset);

		static void InitializeSlang(const char* shaderDirectory, bool isDebugEnabled);

		static bool CheckSlangDiagnostics(const VulkanDevice* device, const Slang::ComPtr<slang::IBlob>& diagnostics);
	private:
		const VulkanDevice* m_Device = nullptr;
		Slang::ComPtr<slang::IModule> m_SlangModule;
		VkShaderModule m_ShaderModule = VK_NULL_HANDLE;

		std::vector<EntryPoint> m_EntryPoints;
		BufferLayout m_BufferLayout;

		inline static Slang::ComPtr<slang::IGlobalSession> s_SlangGlobalSession;
		inline static Slang::ComPtr<slang::ISession> s_SlangSession;
	};
}
