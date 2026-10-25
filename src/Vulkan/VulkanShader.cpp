#include "VulkanShader.h"

#include "VulkanConvertions.h"

#include <algorithm>
#include <array>
#include <utility>

namespace RealRHI {
	namespace {
		struct DescriptorSetBuilderContext {
			DescriptorsDesc& descriptors;
			const VulkanDevice* device = nullptr;
			uint32_t& pushDataSize;
		};

		DescriptorSetLayoutDesc& GetSet(DescriptorSetBuilderContext& context, uint32_t setIndex) {
			return context.descriptors.sets[setIndex];
		}

		uint32_t GetNextBindingIndex(DescriptorSetBuilderContext& context, uint32_t setIndex) {
			return static_cast<uint32_t>(GetSet(context, setIndex).bindings.size());
		}

		std::string JoinBindingName(const std::string& prefix, const char* name) {
			if (!name || !name[0]) {
				return prefix;
			}
			if (prefix.empty()) {
				return name;
			}
			return prefix + "." + name;
		}

		void CollectDescriptorBindingNames(slang::TypeLayoutReflection* typeLayout,
			const std::string& prefix,
			std::vector<std::string>& outNames) {
			const uint32_t fieldCount = static_cast<uint32_t>(typeLayout->getFieldCount());
			for (uint32_t fieldIndex = 0; fieldIndex < fieldCount; ++fieldIndex) {
				auto* fieldLayout = typeLayout->getFieldByIndex(fieldIndex);
				auto* fieldTypeLayout = fieldLayout->getTypeLayout();
				const std::string fieldName = JoinBindingName(prefix, fieldLayout->getName());

				const int rangeCount = fieldTypeLayout->getDescriptorSetDescriptorRangeCount(0);
				for (int rangeIndex = 0; rangeIndex < rangeCount; ++rangeIndex) {
					const slang::BindingType bindingType =
						fieldTypeLayout->getDescriptorSetDescriptorRangeType(0, rangeIndex);
					if (bindingType == slang::BindingType::PushConstant) {
						continue;
					}
					outNames.push_back(fieldName);
				}
			}
		}

		void AddDescriptorRanges(slang::TypeLayoutReflection* typeLayout,
			uint32_t setIndex,
			DescriptorSetBuilderContext& context,
			const std::string& prefix) {
			constexpr int relativeSetIndex = 0;
			const int rangeCount = typeLayout->getDescriptorSetDescriptorRangeCount(relativeSetIndex);
			std::vector<std::string> descriptorNames;
			CollectDescriptorBindingNames(typeLayout, prefix, descriptorNames);
			size_t descriptorNameIndex = 0;

			for (int rangeIndex = 0; rangeIndex < rangeCount; ++rangeIndex) {
				const slang::BindingType bindingType =
					typeLayout->getDescriptorSetDescriptorRangeType(relativeSetIndex, rangeIndex);
				if (bindingType == slang::BindingType::PushConstant) {
					continue;
				}

				std::string descriptorName;
				if (descriptorNameIndex < descriptorNames.size()) {
					descriptorName = descriptorNames[descriptorNameIndex++];
				}

				GetSet(context, setIndex)
					.bindings.push_back(DescriptorBinding{
						.name = std::move(descriptorName),
						.binding = GetNextBindingIndex(context, setIndex),
						.vkType = Utils::SlangBindingTypeToVkDescriptorType(bindingType),
						.descriptorCount = static_cast<uint32_t>(
							typeLayout->getDescriptorSetDescriptorRangeDescriptorCount(relativeSetIndex, rangeIndex)),
					});
			}
		}

		void AddRangesForParameterBlockElement(slang::TypeLayoutReflection* elementTypeLayout,
			uint32_t setIndex,
			DescriptorSetBuilderContext& context,
			const std::string& prefix);

		void CompactDescriptorSets(DescriptorsDesc& descriptors) {
			std::vector<DescriptorSetLayoutDesc> compactedSets;
			compactedSets.reserve(descriptors.sets.size());

			for (const auto& set : descriptors.sets) {
				if (set.bindings.empty()) {
					continue;
				}

				DescriptorSetLayoutDesc compactedSet = set;
				compactedSet.setIndex = static_cast<uint32_t>(compactedSets.size());
				compactedSets.push_back(std::move(compactedSet));
			}

			descriptors.sets = std::move(compactedSets);
		}

		void AddDescriptorSetForParameterBlock(slang::TypeLayoutReflection* parameterBlockTypeLayout,
			DescriptorSetBuilderContext& context,
			const std::string& prefix);

		void AddProgramParameters(slang::ProgramLayout* programLayout,
			uint32_t defaultSetIndex,
			DescriptorSetBuilderContext& context) {
			const uint32_t parameterCount = static_cast<uint32_t>(programLayout->getParameterCount());
			for (uint32_t parameterIndex = 0; parameterIndex < parameterCount; ++parameterIndex) {
				auto* parameterLayout = programLayout->getParameterByIndex(parameterIndex);
				auto* parameterTypeLayout = parameterLayout->getTypeLayout();
				const std::string parameterName = parameterLayout->getName();

				if (parameterTypeLayout->getKind() == slang::TypeReflection::Kind::ParameterBlock) {
					AddDescriptorSetForParameterBlock(parameterTypeLayout, context, parameterName);
					continue;
				}

				AddRangesForParameterBlockElement(parameterTypeLayout, defaultSetIndex, context, parameterName);
			}
		}

		void AddDescriptorSetForParameterBlock(slang::TypeLayoutReflection* parameterBlockTypeLayout,
			DescriptorSetBuilderContext& context,
			const std::string& prefix) {
			const uint32_t setIndex = static_cast<uint32_t>(context.descriptors.sets.size());
			context.descriptors.sets.push_back(DescriptorSetLayoutDesc{
				.setIndex = setIndex,
			});

			AddRangesForParameterBlockElement(
				parameterBlockTypeLayout->getElementTypeLayout(), setIndex, context, prefix);
		}

		void RecordPushDataSize(slang::TypeLayoutReflection* typeLayout, DescriptorSetBuilderContext& context) {
			if (typeLayout == nullptr) {
				return;
			}
			auto* element = typeLayout->getElementTypeLayout();
			if (element != nullptr) {
				context.pushDataSize = std::max(context.pushDataSize, static_cast<uint32_t>(element->getSize()));
			}
		}

		void AddSubObjectRanges(slang::TypeLayoutReflection* typeLayout,
			uint32_t setIndex,
			DescriptorSetBuilderContext& context,
			const std::string& prefix) {
			const uint32_t fieldCount = static_cast<uint32_t>(typeLayout->getFieldCount());
			for (uint32_t fieldIndex = 0; fieldIndex < fieldCount; ++fieldIndex) {
				auto* fieldLayout = typeLayout->getFieldByIndex(fieldIndex);
				auto* fieldTypeLayout = fieldLayout->getTypeLayout();
				const std::string fieldName = JoinBindingName(prefix, fieldLayout->getName());

				switch (fieldTypeLayout->getKind()) {
					case slang::TypeReflection::Kind::ParameterBlock:
						AddDescriptorSetForParameterBlock(fieldTypeLayout, context, fieldName);
						break;
					default:
						break;
				}

				const int subObjectRangeCount = fieldTypeLayout->getSubObjectRangeCount();
				for (int subObjectRangeIndex = 0; subObjectRangeIndex < subObjectRangeCount; ++subObjectRangeIndex) {
					const int bindingRangeIndex =
						fieldTypeLayout->getSubObjectRangeBindingRangeIndex(subObjectRangeIndex);
					const slang::BindingType bindingType = fieldTypeLayout->getBindingRangeType(bindingRangeIndex);
					slang::TypeLayoutReflection* leafTypeLayout =
						fieldTypeLayout->getBindingRangeLeafTypeLayout(bindingRangeIndex);

					switch (bindingType) {
						case slang::BindingType::ConstantBuffer:
							AddRangesForParameterBlockElement(
								leafTypeLayout->getElementTypeLayout(), setIndex, context, fieldName);
							break;
						case slang::BindingType::PushConstant:
							RecordPushDataSize(leafTypeLayout, context);
							break;
						default:
							break;
					}
				}
			}
		}

		void AddRangesForParameterBlockElement(slang::TypeLayoutReflection* elementTypeLayout,
			uint32_t setIndex,
			DescriptorSetBuilderContext& context,
			const std::string& prefix) {
			if (elementTypeLayout->getSize() > 0) {
				GetSet(context, setIndex)
					.bindings.push_back(DescriptorBinding{
						.name = prefix,
						.binding = GetNextBindingIndex(context, setIndex),
						.vkType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
						.descriptorCount = 1,
					});
			}

			AddDescriptorRanges(elementTypeLayout, setIndex, context, prefix);
			AddSubObjectRanges(elementTypeLayout, setIndex, context, prefix);
		}
	}

	VulkanShader::VulkanShader(const VulkanDevice* device)
		: m_Device(device) { }

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
		if (desc.moduleName == nullptr || !InitializeSlang(m_Device)) {
			return Result::Failed;
		}

		Slang::ComPtr<slang::IBlob> diagnostics;

		m_SlangModule = LoadModule(desc, diagnostics);
		if (!m_SlangModule) {
			return Result::Failed;
		}

		auto composedProgram = ComposeProgram(m_SlangModule, diagnostics);
		if (!composedProgram) {
			return Result::Failed;
		}

		m_BufferLayout = ReflectLayout(composedProgram, m_EntryPoints, diagnostics);

		m_DescriptorsDesc = ReflectDescriptors(composedProgram, diagnostics);
		if (m_PushDataSize > m_Device->GetMaxPushDataSize()) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::ShaderCompilation,
				"Shader push-constant data exceeds the device descriptor heap maxPushDataSize.");
			return Result::Failed;
		}

		auto linkedProgram = LinkProgram(composedProgram, diagnostics);
		if (!linkedProgram) {
			return Result::Failed;
		}

		m_ShaderModule = CreateVkShaderModule(linkedProgram);
		if (!m_ShaderModule) {
			return Result::Failed;
		}

		return m_Device->SetDebugName(VK_OBJECT_TYPE_SHADER_MODULE, m_ShaderModule, desc.debugName);
	}

	Slang::ComPtr<slang::IModule> VulkanShader::LoadModule(const ShaderDesc& desc,
		Slang::ComPtr<slang::IBlob>& diagnostics) {
		Slang::ComPtr<slang::IModule> module(s_SlangSession->loadModule(desc.moduleName, diagnostics.writeRef()));

		if (!module) {
			CheckSlangDiagnostics(diagnostics, SLANG_FAIL);
			return nullptr;
		}
		CheckSlangDiagnostics(diagnostics, SLANG_OK);

		return module;
	}

	Slang::ComPtr<slang::IComponentType> VulkanShader::ComposeProgram(slang::IModule* module,
		Slang::ComPtr<slang::IBlob>& diagnostics) {
		const uint32_t entryCount = module->getDefinedEntryPointCount();
		std::vector<Slang::ComPtr<slang::IComponentType>> componentsToLink(entryCount + 1);

		// Include the module so shared code is linked with its entry points.
		componentsToLink[0] = Slang::ComPtr<slang::IComponentType>(module);

		std::vector<Slang::ComPtr<slang::IEntryPoint>> entryPoints(entryCount);
		for (uint32_t i = 0; i < entryCount; ++i) {
			if (SLANG_FAILED(module->getDefinedEntryPoint(i, entryPoints[i].writeRef())) || !entryPoints[i]) {
				m_Device->SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::ShaderCompilation,
					"CreateShader: Failed to retrieve a Slang entry point.");

				return nullptr;
			}
			componentsToLink[i + 1] = Slang::ComPtr<slang::IComponentType>(entryPoints[i].get());
		}

		Slang::ComPtr<slang::IComponentType> composedProgram;

		const SlangResult result = s_SlangSession->createCompositeComponentType(
			reinterpret_cast<slang::IComponentType**>(componentsToLink.data()),
			componentsToLink.size(),
			composedProgram.writeRef(),
			diagnostics.writeRef());

		if (!CheckSlangDiagnostics(diagnostics, result)) {
			return nullptr;
		}

		return composedProgram;
	}

	BufferLayout VulkanShader::ReflectLayout(slang::IComponentType* composedProgram,
		std::vector<EntryPoint>& outEntryPoints,
		Slang::ComPtr<slang::IBlob>& diagnostics) {
		slang::ProgramLayout* programLayout = composedProgram->getLayout(0, diagnostics.writeRef());

		if (programLayout == nullptr) {
			CheckSlangDiagnostics(diagnostics, SLANG_FAIL);
			return {};
		}
		CheckSlangDiagnostics(diagnostics, SLANG_OK);

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

	DescriptorsDesc VulkanShader::ReflectDescriptors(slang::IComponentType* composedProgram,
		Slang::ComPtr<slang::IBlob>& diagnostics) {
		slang::ProgramLayout* programLayout = composedProgram->getLayout(0, diagnostics.writeRef());
		if (programLayout == nullptr) {
			CheckSlangDiagnostics(diagnostics, SLANG_FAIL);
			return {};
		}
		CheckSlangDiagnostics(diagnostics, SLANG_OK);

		DescriptorsDesc result;
		DescriptorSetBuilderContext context{
			.descriptors = result,
			.device = m_Device,
			.pushDataSize = m_PushDataSize,
		};

		result.sets.push_back(DescriptorSetLayoutDesc{
			.setIndex = 0,
		});
		const uint32_t defaultSetIndex = 0;

		AddProgramParameters(programLayout, defaultSetIndex, context);

		const uint32_t entryPointCount = programLayout->getEntryPointCount();
		for (uint32_t i = 0; i < entryPointCount; ++i) {
			auto* entryPointLayout = programLayout->getEntryPointByIndex(i);
			AddRangesForParameterBlockElement(
				entryPointLayout->getTypeLayout(), defaultSetIndex, context, entryPointLayout->getName());
		}

		CompactDescriptorSets(result);

		return result;
	}

	Slang::ComPtr<slang::IComponentType> VulkanShader::LinkProgram(slang::IComponentType* composed,
		Slang::ComPtr<slang::IBlob>& diagnostics) {
		Slang::ComPtr<slang::IComponentType> linkedProgram;

		const SlangResult result = composed->link(linkedProgram.writeRef(), diagnostics.writeRef());

		if (!CheckSlangDiagnostics(diagnostics, result)) {
			return nullptr;
		}

		return linkedProgram;
	}

	VkShaderModule VulkanShader::CreateVkShaderModule(slang::IComponentType* linkedProgram) {
		Slang::ComPtr<slang::IBlob> spirv;
		Slang::ComPtr<slang::IBlob> diagnostics;
		const SlangResult result = linkedProgram->getTargetCode(0, spirv.writeRef(), diagnostics.writeRef());
		if (!CheckSlangDiagnostics(diagnostics, result) || !spirv || spirv->getBufferSize() == 0) {
			return VK_NULL_HANDLE;
		}

		VkShaderModuleCreateInfo ci{
			.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			.codeSize = spirv->getBufferSize(),
			.pCode = reinterpret_cast<const uint32_t*>(spirv->getBufferPointer()),
		};

		VkShaderModule shaderModule = VK_NULL_HANDLE;
		if (vkCreateShaderModule(m_Device->GetDevice(), &ci, nullptr, &shaderModule) != VK_SUCCESS) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan shader module.");

			return VK_NULL_HANDLE;
		}

		return shaderModule;
	}

	std::vector<BufferAttribute> VulkanShader::CalculateCumulativeOffset(slang::TypeLayoutReflection* typeReflection,
		uint32_t* offset) {
		std::vector<BufferAttribute> attributes;

		uint8_t fieldCount = typeReflection->getFieldCount();
		for (uint8_t i = 0; i < fieldCount; i++) {
			auto fieldTypeLayout = typeReflection->getFieldByIndex(i)->getTypeLayout();

			ScalarType fieldType = ScalarType::None;
			slang::TypeReflection::Kind fieldKind = fieldTypeLayout->getKind();
			uint8_t elementCount = 1;
			switch (fieldKind) {
				case slang::TypeReflection::Kind::Struct: {
					std::vector<BufferAttribute> otherAttribs = CalculateCumulativeOffset(fieldTypeLayout, offset);
					attributes.insert(attributes.end(), otherAttribs.begin(), otherAttribs.end());
					continue;
				}
				case slang::TypeReflection::Kind::Array: {
					// TODO: Emit a separate vertex attribute location for each array element.
					fieldType = static_cast<ScalarType>(fieldTypeLayout->getElementTypeLayout()->getScalarType());
					elementCount = fieldTypeLayout->getElementCount();
					break;
				}
				case slang::TypeReflection::Kind::Vector: {
					elementCount = fieldTypeLayout->getElementCount();
					fieldType = Utils::SlangVectorToRealRHIScalarType(
						fieldTypeLayout->getElementTypeLayout()->getScalarType(), elementCount);
					break;
				}
				case slang::TypeReflection::Kind::Scalar: {
					fieldType = static_cast<ScalarType>(fieldTypeLayout->getScalarType());
					break;
				}
			}

			attributes.push_back(BufferAttribute{.type = fieldType, .offset = *offset});
			*offset += elementCount * Utils::ScalarTypeToSizeOf(fieldType);
		}

		return attributes;
	}

	bool VulkanShader::InitializeSlang(const VulkanDevice* device) {
		if (!s_SlangSession) {
			if (!s_SlangGlobalSession && SLANG_FAILED(slang::createGlobalSession(s_SlangGlobalSession.writeRef()))) {
				device->SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::ShaderCompilation,
					"CreateShader: Failed to create the Slang global session.");

				return false;
			}
			std::array<slang::TargetDesc, 1> slangTargets{
				slang::TargetDesc{
					.format = SLANG_SPIRV,
					.profile = s_SlangGlobalSession->findProfile("spirv_1_6"),
				},
			};

			SlangCapabilityID spvDescriptorHeapEXT = s_SlangGlobalSession->findCapability("spvDescriptorHeapEXT");
			if (spvDescriptorHeapEXT == SLANG_CAPABILITY_UNKNOWN) {
				device->SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::ShaderCompilation,
					"CreateShader: The installed Slang compiler does not support spvDescriptorHeapEXT.");
				return false;
			}

			std::array<slang::CompilerOptionEntry, 3> slangOptions{
				slang::CompilerOptionEntry{
					slang::CompilerOptionName::EmitSpirvDirectly,
					slang::CompilerOptionValue{slang::CompilerOptionValueKind::Int, 1},
				},
				slang::CompilerOptionEntry{
					slang::CompilerOptionName::Capability,
					slang::CompilerOptionValue{slang::CompilerOptionValueKind::Int, spvDescriptorHeapEXT},
				},
				slang::CompilerOptionEntry{
					slang::CompilerOptionName::DebugInformation,
					slang::CompilerOptionValue{slang::CompilerOptionValueKind::Int, SLANG_DEBUG_INFO_LEVEL_STANDARD},
				},
			};

			uint32_t slangOptionsCount = static_cast<uint32_t>(slangOptions.size());
			if (!device->IsDebugEnabled()) {
				--slangOptionsCount;
			}
			std::string shaderSearchPath = device->GetShaderDirectory().string();
			const char* shaderSearchPathCStr = shaderSearchPath.c_str();
			slang::SessionDesc slangSessionDesc{
				.targets = slangTargets.data(),
				.targetCount = static_cast<SlangInt>(slangTargets.size()),
				.defaultMatrixLayoutMode =
					SLANG_MATRIX_LAYOUT_COLUMN_MAJOR, // Match the renderer's column-major matrix convention.
				.searchPaths = &shaderSearchPathCStr,
				.searchPathCount = 1,
				.compilerOptionEntries = slangOptions.data(),
				.compilerOptionEntryCount = slangOptionsCount,
			};
			if (SLANG_FAILED(s_SlangGlobalSession->createSession(slangSessionDesc, s_SlangSession.writeRef()))) {
				device->SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::ShaderCompilation,
					"CreateShader: Failed to create the Slang compilation session.");
				return false;
			}
		}
		return s_SlangSession != nullptr;
	}

	bool VulkanShader::CheckSlangDiagnostics(const Slang::ComPtr<slang::IBlob>& diagnostics, SlangResult result) {
		if (diagnostics) {
			DebugSeverity severity = DebugSeverity::Warning;
			if (SLANG_FAILED(result)) {
				severity = DebugSeverity::Error;
			}
			m_Device->SendDebugMessage(severity,
				DebugMessageType::ShaderCompilation,
				static_cast<const char*>(diagnostics->getBufferPointer()));
		}
		else if (SLANG_FAILED(result)) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::ShaderCompilation,
				"CreateShader: Slang compilation operation failed without diagnostics.");
		}
		return SLANG_SUCCEEDED(result);
	}
}
