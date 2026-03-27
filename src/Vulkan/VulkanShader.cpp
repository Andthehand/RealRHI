#include "VulkanShader.h"
#include <array>

#include "VulkanConvertions.h"

namespace RealRHI {
	VulkanShader::VulkanShader(const VulkanDevice* device) : m_Device(device) {
	}

	VulkanShader::~VulkanShader() {
		vkDestroyShaderModule(m_Device->GetDevice(), m_ShaderModule, nullptr);
	}

	Result VulkanShader::Create(const VulkanDevice* device, const ShaderDesc& desc, Ref<VulkanShader>& outShader) {
		Ref<VulkanShader> shader = Ref<VulkanShader>::Create(device);
		Result res = shader->Init(desc);
		if (res != Result::Success) {
			return res;
		}

		outShader = shader;
		return Result::Success;
	}

	Result VulkanShader::Init(const ShaderDesc& desc) {
		InitializeSlang(m_Device->GetShaderDirectory().string().c_str(), m_Device->IsDebugEnabled());

		Slang::ComPtr<slang::IBlob> diagnostics;

		// 1. Load module
		m_SlangModule = LoadModule(m_Device, desc, diagnostics);
		if (!m_SlangModule) return Result::Failed;

		// 2. Compose program (entry points)
		auto composedProgram = ComposeProgram(m_Device, m_SlangModule, diagnostics);
		if (!composedProgram) return Result::Failed;

		// 3. Reflect layout
		m_BufferLayout = ReflectLayout(m_Device, composedProgram, m_EntryPoints, diagnostics);

		// 4. Link program
		auto linkedProgram = LinkProgram(m_Device, composedProgram, diagnostics);
		if (!linkedProgram) return Result::Failed;

		linkedProgram->getLayout()->toJson(diagnostics.writeRef());
		CheckSlangDiagnostics(m_Device, diagnostics);

		// 5. Create Vulkan module
		m_ShaderModule = CreateVkShaderModule(m_Device, linkedProgram);
		if (!m_ShaderModule) return Result::Failed;
		
		return Result::Success;
	}

	Slang::ComPtr<slang::IModule> VulkanShader::LoadModule(
			const VulkanDevice* device,
			const ShaderDesc& desc,
			Slang::ComPtr<slang::IBlob>& diagnostics) {
		Slang::ComPtr<slang::IModule> module(
			s_SlangSession->loadModule(
				desc.moduleName,
				diagnostics.writeRef()
			)
		);

		if (!CheckSlangDiagnostics(device, diagnostics))
			return nullptr;

		return module;
	}

	Slang::ComPtr<slang::IComponentType>
		VulkanShader::ComposeProgram(
			const VulkanDevice* device,
			slang::IModule* module,
			Slang::ComPtr<slang::IBlob>& diagnostics) {
		const uint32_t entryCount = module->getDefinedEntryPointCount();
		std::vector<Slang::ComPtr<slang::IEntryPoint>> entryPoints(entryCount);
		for (uint32_t i = 0; i < entryCount; ++i) {
			module->getDefinedEntryPoint(i, entryPoints[i].writeRef());
		}

		Slang::ComPtr<slang::IComponentType> composedProgram;

		s_SlangSession->createCompositeComponentType(
			reinterpret_cast<slang::IComponentType**>(entryPoints.data()),
			entryPoints.size(),
			composedProgram.writeRef(),
			diagnostics.writeRef());

		if (!CheckSlangDiagnostics(device, diagnostics))
			return nullptr;

		return composedProgram;
	}

	BufferLayout VulkanShader::ReflectLayout(
		const VulkanDevice* device,
		slang::IComponentType* composedProgram,
		std::vector<EntryPoint>& outEntryPoints,
		Slang::ComPtr<slang::IBlob>& diagnostics) {
		slang::ProgramLayout* programLayout = composedProgram->getLayout(0, diagnostics.writeRef());

		if (!CheckSlangDiagnostics(device, diagnostics))
			return {};

		const uint32_t entryCount = programLayout->getEntryPointCount();
		outEntryPoints.resize(entryCount);

		BufferLayout bufferLayout;
		for (uint32_t i = 0; i < entryCount; ++i) {
			auto* entryReflect = programLayout->getEntryPointByIndex(i);

			outEntryPoints[i].stage = entryReflect->getStage();
			outEntryPoints[i].entryPointName = entryReflect->getName();

			if (outEntryPoints[i].stage == SlangStage::SLANG_STAGE_VERTEX) {
				const uint32_t parameterCount = entryReflect->getParameterCount();

				for (uint32_t j = 0; j < parameterCount; ++j) {
					auto* parameterReflect = entryReflect->getParameterByIndex(j)->getTypeLayout();

					uint32_t offset = 0;
					bufferLayout.attributes = CalculateCumulativeOffset(parameterReflect, &offset);

					bufferLayout.stride = offset;
				}
			}
		}

		return bufferLayout;
	}

	Slang::ComPtr<slang::IComponentType>
		VulkanShader::LinkProgram(
			const VulkanDevice* device,
			slang::IComponentType* composed,
			Slang::ComPtr<slang::IBlob>& diagnostics) {
		Slang::ComPtr<slang::IComponentType> linkedProgram;

		composed->link(
			linkedProgram.writeRef(),
			diagnostics.writeRef()
		);

		if (!CheckSlangDiagnostics(device, diagnostics))
			return nullptr;

		return linkedProgram;
	}

	VkShaderModule VulkanShader::CreateVkShaderModule(const VulkanDevice* device, slang::IComponentType* linkedProgram) {
		Slang::ComPtr<slang::IBlob> spirv;
		linkedProgram->getTargetCode(0, spirv.writeRef());

		VkShaderModuleCreateInfo ci{
			.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			.codeSize = spirv->getBufferSize(),
			.pCode = reinterpret_cast<const uint32_t*>(spirv->getBufferPointer())
		};

		VkShaderModule shaderModule = VK_NULL_HANDLE;
		if (vkCreateShaderModule(device->GetDevice(), &ci, nullptr, &shaderModule) != VK_SUCCESS) {
			device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan shader module.");

			return VK_NULL_HANDLE;
		}

		return shaderModule;
	}

	std::vector<BufferAttribute> VulkanShader::CalculateCumulativeOffset(slang::TypeLayoutReflection* typeReflection, uint32_t* offset) {
		std::vector<BufferAttribute> attributes;
		
		uint8_t fieldCount = typeReflection->getFieldCount();
		for (uint8_t i = 0; i < fieldCount; i++) {
			auto fieldTypeLayout = typeReflection->getFieldByIndex(i)->getTypeLayout();

			ScalarType fieldType = ScalarType::None; // float, int, uint, vec, etc...
			slang::TypeReflection::Kind fieldKind = fieldTypeLayout->getKind(); // Struct, Array, Vector, etc...
			uint8_t elementCount = 1; // Default to 1 for non-array/vector types
			switch (fieldKind) {
				case slang::TypeReflection::Kind::Struct: {
					// Recursively calculate offsets for nested structs
					std::vector<BufferAttribute> otherAttribs = CalculateCumulativeOffset(fieldTypeLayout, offset);
					attributes.insert(attributes.end(), otherAttribs.begin(), otherAttribs.end());
					continue;
				}
				case slang::TypeReflection::Kind::Array: {
					// TODO: Fix.. Arrays are handled where each element is in a different location so we need to push a new BufferAttribute for every array element
					fieldType = (ScalarType)fieldTypeLayout->getElementTypeLayout()->getScalarType();
					elementCount = fieldTypeLayout->getElementCount(); // Usualy float[3]
					break;
				}
				case slang::TypeReflection::Kind::Vector: {
					elementCount = fieldTypeLayout->getElementCount(); // Usualy float3, float4, etc...
					fieldType = Utils::SlangVectorToRealRHIScalarType(fieldTypeLayout->getElementTypeLayout()->getScalarType(), elementCount);
					break;
				}
				case slang::TypeReflection::Kind::Scalar: {
					fieldType = (ScalarType)fieldTypeLayout->getScalarType(); // This should always be a 1-1 mapping
					break;
				}
			}

			attributes.push_back(BufferAttribute{
				.type = fieldType,
				.offset = *offset
			});
			*offset += elementCount * Utils::ScalarTypeToSizeOf(fieldType);
		}

		return attributes;
	}

	void VulkanShader::InitializeSlang(const char* shaderDirectory, bool isDebugEnabled) {
		if (!s_SlangGlobalSession) {
			slang::createGlobalSession(s_SlangGlobalSession.writeRef());
			std::array<slang::TargetDesc, 1> slangTargets{
				std::to_array<slang::TargetDesc>({
					slang::TargetDesc{
						.format{SLANG_SPIRV}, // Compile to SPIR-V
						.profile{s_SlangGlobalSession->findProfile("spirv_1_5")}
					}
				})
			};

			std::array<slang::CompilerOptionEntry, 2> slangOptions{
				std::to_array<slang::CompilerOptionEntry>({
					slang::CompilerOptionEntry{
						slang::CompilerOptionName::EmitSpirvDirectly, // Emit SPIR-V directly instead of generating source code that will be compiled by a downstream compiler.
						slang::CompilerOptionValue{slang::CompilerOptionValueKind::Int, 1} // Set to 1 to enable, 0 to disable. Default is false (0).
					},
					slang::CompilerOptionEntry{
						slang::CompilerOptionName::DebugInformation,
						slang::CompilerOptionValue{slang::CompilerOptionValueKind::Int, SLANG_DEBUG_INFO_LEVEL_STANDARD}
					}
				})
			};

			uint32_t slangOptionsCount = isDebugEnabled ? 2 : 1;
			slang::SessionDesc slangSessionDesc{
				.targets{slangTargets.data()},
				.targetCount{SlangInt(slangTargets.size())},
				.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR, // Because we use glm which uses column-major layout
				.searchPaths = &shaderDirectory,
				.searchPathCount = 1,
				.compilerOptionEntries{slangOptions.data()},
				.compilerOptionEntryCount{slangOptionsCount}
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
