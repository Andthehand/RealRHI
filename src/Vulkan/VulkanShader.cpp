#include "VulkanShader.h"
#include <array>


namespace RealRHI {
	VulkanShader::VulkanShader(const VulkanDevice* device, const char* moduleName) 
		: m_Device(device) {
		InitializeSlang(m_Device->GetShaderDirectory().string().c_str(), device->IsDebugEnabled());

		Slang::ComPtr<slang::IBlob> diagnostics;
		m_SlangModule = s_SlangSession->loadModule(moduleName, diagnostics.writeRef());
		if(CheckSlangDiagnostics(diagnostics)) {
			return;
		}

		Slang::ComPtr<slang::IComponentType> linkedProgram;
		Slang::ComPtr<slang::IBlob> diagnosticBlob;
		m_SlangModule->link(linkedProgram.writeRef(), diagnosticBlob.writeRef());
		if (CheckSlangDiagnostics(diagnostics)) {
			return;
		}

		Slang::ComPtr<slang::IBlob> spirv;
		linkedProgram->getTargetCode(0, spirv.writeRef());

		VkShaderModuleCreateInfo shaderModuleCI{ 
			.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 
			.codeSize = spirv->getBufferSize(), 
			.pCode = (uint32_t*)spirv->getBufferPointer() 
		};

		if (vkCreateShaderModule(m_Device->GetDevice(), &shaderModuleCI, nullptr, &m_ShaderModule) != VK_SUCCESS) {
			// TODO: Actually handle this error
			return;
		}
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

	bool VulkanShader::CheckSlangDiagnostics(const Slang::ComPtr<slang::IBlob> diagnostics) const {
		if (diagnostics) {
			DebugMessage message{
				.severity = DebugSeverity::Error,
				.type = DebugMessageType::ShaderCompilation,
				.message = static_cast<const char*>(diagnostics->getBufferPointer())
			};
			m_Device->GetDebugCallback()(message);
			return true;
		}

		return false;
	}
}
