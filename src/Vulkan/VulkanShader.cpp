#include "VulkanShader.h"
#include <array>


namespace RealRHI {
	Result VulkanShader::Create(const VulkanDevice* device, const ShaderDesc& desc, Ref<Shader>& outShader) {
		InitializeSlang(device->GetShaderDirectory().string().c_str(), device->IsDebugEnabled());

		Slang::ComPtr<slang::IBlob> diagnostics;
		Slang::ComPtr<slang::IModule> slangModule(s_SlangSession->loadModule(desc.moduleName, diagnostics.writeRef()));
		if(!CheckSlangDiagnostics(device, diagnostics)) {
			return Result::Failed;
		}

		uint8_t entryPointCount = slangModule->getDefinedEntryPointCount();
		std::vector<Slang::ComPtr<slang::IEntryPoint>> entryPoints(entryPointCount);
		for (uint8_t i = 0; i < entryPointCount; i++) {
			slangModule->getDefinedEntryPoint(i, entryPoints[i].writeRef());
		}

		Slang::ComPtr<slang::IComponentType> composedProgram;
		s_SlangSession->createCompositeComponentType((slang::IComponentType**)entryPoints.data(), entryPoints.size(), composedProgram.writeRef(), diagnostics.writeRef());
		if (!CheckSlangDiagnostics(device, diagnostics)) {
			return Result::Failed;
		}

		// Reflect
		slang::ProgramLayout* programLayout = composedProgram->getLayout(0, diagnostics.writeRef());
		if (!CheckSlangDiagnostics(device, diagnostics)) {
			return Result::Failed;
		}

		entryPointCount = programLayout->getEntryPointCount();
		std::vector<EntryPoint> createEntryPoints(entryPointCount);
		for (uint8_t i = 0; i < entryPointCount; i++) {
			slang::EntryPointReflection* entryPointReflect = programLayout->getEntryPointByIndex(i);

			createEntryPoints[i].stage = entryPointReflect->getStage();
			createEntryPoints[i].entryPointName = entryPointReflect->getName();
		}

		Slang::ComPtr<slang::IComponentType> linkedProgram;
		slangModule->link(linkedProgram.writeRef(), diagnostics.writeRef());
		if (!CheckSlangDiagnostics(device, diagnostics)) {
			return Result::Failed;
		}

		Slang::ComPtr<slang::IBlob> spirv;
		linkedProgram->getTargetCode(0, spirv.writeRef());

		VkShaderModuleCreateInfo shaderModuleCI{ 
			.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 
			.codeSize = spirv->getBufferSize(), 
			.pCode = (uint32_t*)spirv->getBufferPointer() 
		};

		VkShaderModule shaderModule;
		if (vkCreateShaderModule(device->GetDevice(), &shaderModuleCI, nullptr, &shaderModule) != VK_SUCCESS) {
			device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan shader module.");
			return Result::Failed;
		}

		outShader = Ref<Shader>(new VulkanShader(device, slangModule, shaderModule, createEntryPoints));
		return Result::Success;
	}

	VulkanShader::VulkanShader(const VulkanDevice* device, Slang::ComPtr<slang::IModule> slangModule, VkShaderModule shaderModule, std::vector<EntryPoint> entryPoints)
		: m_Device(device), m_SlangModule(slangModule), m_ShaderModule(shaderModule), m_EntryPoints(std::move(entryPoints)) {
	}

	VulkanShader::~VulkanShader() {
		vkDestroyShaderModule(m_Device->GetDevice(), m_ShaderModule, nullptr);
	}

	void VulkanShader::InitializeSlang(const char* shaderDirectory, bool isDebugEnabled) {
		if (!s_SlangGlobalSession) {
			slang::createGlobalSession(s_SlangGlobalSession.writeRef());
			std::array<slang::TargetDesc, 1> slangTargets{
				std::to_array<slang::TargetDesc>({
					slang::TargetDesc{
						.format{SLANG_SPIRV}, // Compile to SPIR-V
						.profile{s_SlangGlobalSession->findProfile("spirv_1_4")}
					}
				})
			};

			SlangDebugInfoLevel debugInfoLevel = isDebugEnabled ? SLANG_DEBUG_INFO_LEVEL_STANDARD : SLANG_DEBUG_INFO_LEVEL_NONE;
			std::array<slang::CompilerOptionEntry, 2> slangOptions{
				std::to_array<slang::CompilerOptionEntry>({
					slang::CompilerOptionEntry{
						slang::CompilerOptionName::EmitSpirvDirectly, // Emit SPIR-V directly instead of generating source code that will be compiled by a downstream compiler.
						slang::CompilerOptionValue{slang::CompilerOptionValueKind::Int, 1} // Set to 1 to enable, 0 to disable. Default is false (0).
					},
					slang::CompilerOptionEntry{
						slang::CompilerOptionName::DebugInformation,
						slang::CompilerOptionValue{slang::CompilerOptionValueKind::Int, (int)debugInfoLevel}
					}
				})
			};
			slang::SessionDesc slangSessionDesc{
				.targets{slangTargets.data()},
				.targetCount{SlangInt(slangTargets.size())},
				.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR, // Because we use glm which uses column-major layout
				.searchPaths = &shaderDirectory,
				.searchPathCount = 1,
				.compilerOptionEntries{slangOptions.data()},
				.compilerOptionEntryCount{uint32_t(slangOptions.size())}
			};
			s_SlangGlobalSession->createSession(slangSessionDesc, s_SlangSession.writeRef());
		}
	}

	bool VulkanShader::CheckSlangDiagnostics(const VulkanDevice* device, const Slang::ComPtr<slang::IBlob>& diagnostics) {
		if (diagnostics) {
			device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::ShaderCompilation,
				static_cast<const char*>(diagnostics->getBufferPointer()));
			return false;
		}

		return true;
	}
}
