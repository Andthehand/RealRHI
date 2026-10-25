#include "SlangShaderReflection.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace RealRHI {
	namespace {
		bool HasCategory(slang::TypeLayoutReflection* type, slang::ParameterCategory category) {
			for (unsigned i = 0; i < type->getCategoryCount(); ++i) {
				if (type->getCategoryByIndex(i) == category) {
					return true;
				}
			}

			return false;
		}

		bool GetScalarType(slang::TypeReflection::ScalarType type, ShaderScalarType& outType) {
			switch (type) {
				case slang::TypeReflection::ScalarType::Float16:
					outType = ShaderScalarType::Float16;
					return true;
				case slang::TypeReflection::ScalarType::Float32:
					outType = ShaderScalarType::Float32;
					return true;
				case slang::TypeReflection::ScalarType::Float64:
					outType = ShaderScalarType::Float64;
					return true;
				case slang::TypeReflection::ScalarType::Int32:
					outType = ShaderScalarType::Int32;
					return true;
				case slang::TypeReflection::ScalarType::UInt32:
					outType = ShaderScalarType::UInt32;
					return true;
				default:
					return false;
			}
		}
	}

	bool SlangShaderReflection::ReflectVertexInputs(slang::VariableLayoutReflection* variable,
		size_t parentLocation,
		const std::string& path,
		std::vector<ShaderVertexInput>& inputs,
		std::string& error) {
		const char* semantic = variable->getSemanticName();
		if (semantic && std::strlen(semantic) >= 3 && std::toupper(semantic[0]) == 'S' &&
			std::toupper(semantic[1]) == 'V' && semantic[2] == '_') {
			return true;
		}

		auto* type = variable->getTypeLayout();
		if (!HasCategory(type, slang::ParameterCategory::VaryingInput)) {
			return true;
		}

		const size_t offset = variable->getOffset(slang::ParameterCategory::VaryingInput);
		if (offset > UINT32_MAX || parentLocation > UINT32_MAX - offset) {
			error = path + ": unresolved or overflowing input location";
			return false;
		}

		return ReflectInputType(type, parentLocation + offset, path, inputs, error);
	}

	bool SlangShaderReflection::ReflectInputType(slang::TypeLayoutReflection* type,
		size_t location,
		const std::string& path,
		std::vector<ShaderVertexInput>& inputs,
		std::string& error) {
		using Kind = slang::TypeReflection::Kind;

		switch (type->getKind()) {
			case Kind::Struct:
				for (unsigned i = 0; i < type->getFieldCount(); ++i) {
					auto* field = type->getFieldByIndex(i);
					if (!ReflectVertexInputs(field, location, path + "." + field->getName(), inputs, error)) {
						return false;
					}
				}

				return true;
			case Kind::Array: {
				const size_t count = type->getElementCount();
				const size_t stride = type->getElementStride(SLANG_PARAMETER_CATEGORY_VARYING_INPUT);
				if (!count || count > UINT32_MAX || !stride || stride > UINT32_MAX ||
					count > (UINT32_MAX - location) / stride) {
					error = path + ": unresolved or oversized input array";
					return false;
				}

				for (size_t i = 0; i < count; ++i) {
					if (!ReflectInputType(type->getElementTypeLayout(),
							location + i * stride,
							path + "[" + std::to_string(i) + "]",
							inputs,
							error)) {
						return false;
					}
				}

				return true;
			}

			case Kind::Matrix: {
				const uint32_t rows = type->getRowCount();
				const uint32_t columns = type->getColumnCount();
				if (type->getMatrixLayoutMode() != SLANG_MATRIX_LAYOUT_COLUMN_MAJOR ||
					type->getScalarType() != slang::TypeReflection::ScalarType::Float32 || rows < 1 || rows > 4 ||
					columns < 1 || columns > 4 || type->getSize(SLANG_PARAMETER_CATEGORY_VARYING_INPUT) != columns ||
					location > UINT32_MAX - columns) {
					error = path + ": only column-major float32 matrix vertex inputs are supported";
					return false;
				}

				for (uint32_t column = 0; column < columns; ++column) {
					inputs.push_back(
						{.name = path + "[column " + std::to_string(column) + "]",
							.location = static_cast<uint32_t>(location + column),
							.scalarType = ShaderScalarType::Float32,
							.componentCount = rows});
				}

				return true;
			}

			case Kind::Scalar:
			case Kind::Vector: {
				ShaderVertexInput input{.name = path, .location = static_cast<uint32_t>(location)};
				input.componentCount = 1;
				if (type->getKind() == Kind::Vector) {
					input.componentCount = static_cast<uint32_t>(type->getElementCount());
				}

				if (location > UINT32_MAX || !GetScalarType(type->getScalarType(), input.scalarType) ||
					input.componentCount < 1 || input.componentCount > 4) {
					error = path + ": unsupported vertex input numeric type";
					return false;
				}

				inputs.push_back(std::move(input));
				return true;
			}

			default:
				error = path + ": unsupported vertex input type";
				return false;
		}
	}

	bool SlangShaderReflection::ReflectPushData(slang::TypeLayoutReflection* type,
		ShaderPushData& pushData,
		bool& found,
		std::string& error) {
		// Binding ranges identify actual push blocks without treating every ConstantBuffer as push data.
		for (SlangInt i = 0; i < type->getBindingRangeCount(); ++i) {
			const auto binding = type->getBindingRangeType(i);
			auto* leaf = type->getBindingRangeLeafTypeLayout(i);
			auto* element = leaf ? leaf->getElementTypeLayout() : nullptr;
			if (binding == slang::BindingType::ParameterBlock || binding == slang::BindingType::ConstantBuffer) {
				if (element && !ReflectPushData(element, pushData, found, error)) {
					return false;
				}

				continue;
			}

			if (binding != slang::BindingType::PushConstant) {
				continue;
			}

			if (!element || type->getBindingRangeBindingCount(i) != 1 || element->getSize() > UINT32_MAX) {
				error = "push-data block has an unresolved or overflowing size";
				return false;
			}

			// Vulkan push blocks share byte address zero; reject more than one declaration.
			if (found) {
				error = "multiple push-data blocks are unsupported";
				return false;
			}

			found = true;
			pushData = {.offset = 0, .size = static_cast<uint32_t>(element->getSize())};
		}

		return true;
	}

	bool SlangShaderReflection::ReflectVertexLayout(ShaderReflection& reflection, std::string& error) {
		// Assume one interleaved, tightly packed vertex stream in reflected location order.
		// TODO(C++26): Accept a reference to a C++ vertex struct and use C++26 reflection
		// to derive its member formats, actual member offsets, and sizeof(T) stride instead.
		uint32_t stride = 0;

		for (const auto& input : reflection.vertexInputs) {
			const VertexFormat* formats = nullptr;
			static constexpr VertexFormat floatFormats[] = {
				VertexFormat::Float, VertexFormat::Float2, VertexFormat::Float3, VertexFormat::Float4
			};
			static constexpr VertexFormat intFormats[] = {
				VertexFormat::Int, VertexFormat::Int2, VertexFormat::Int3, VertexFormat::Int4
			};
			static constexpr VertexFormat uintFormats[] = {
				VertexFormat::UInt, VertexFormat::UInt2, VertexFormat::UInt3, VertexFormat::UInt4
			};

			switch (input.scalarType) {
				case ShaderScalarType::Float32:
					formats = floatFormats;
					break;
				case ShaderScalarType::Int32:
					formats = intFormats;
					break;
				case ShaderScalarType::UInt32:
					formats = uintFormats;
					break;
				default:
					error = input.name + ": no vertex format for the reflected numeric type";
					return false;
			}

			const uint32_t size = input.componentCount * sizeof(uint32_t);
			if (stride > UINT32_MAX - size) {
				error = input.name + ": tightly packed vertex stride overflows";
				return false;
			}

			reflection.vertexLayout.attributes.push_back({
				.location = input.location,
				.format = formats[input.componentCount - 1],
				.offset = stride,
			});
			stride += size;
		}

		if (!reflection.vertexInputs.empty()) {
			reflection.vertexLayout.bindings.push_back({.stride = stride});
		}

		return true;
	}

	Result SlangShaderReflection::Reflect(slang::ProgramLayout* layout,
		ShaderReflection& outReflection,
		std::string& error) {
		if (!layout) {
			error = "missing linked program layout";
			return Result::Failed;
		}

		ShaderReflection reflection;
		bool foundPushData = false;
		if (!ReflectPushData(layout->getGlobalParamsTypeLayout(), reflection.pushData, foundPushData, error)) {
			return Result::Failed;
		}

		for (unsigned i = 0; i < layout->getEntryPointCount(); ++i) {
			auto* entry = layout->getEntryPointByIndex(i);
			ShaderEntryPoint point{.name = entry->getName()};

			switch (entry->getStage()) {
				case SLANG_STAGE_VERTEX:
					point.stage = ShaderStage::Vertex;
					break;
				case SLANG_STAGE_FRAGMENT:
					point.stage = ShaderStage::Fragment;
					break;
				case SLANG_STAGE_COMPUTE:
					point.stage = ShaderStage::Compute;
					break;
				default:
					error = point.name + ": unsupported shader stage";
					return Result::Failed;
			}

			if (point.stage == ShaderStage::Vertex) {
				for (unsigned j = 0; j < entry->getParameterCount(); ++j) {
					auto* variable = entry->getParameterByIndex(j);
					const std::string path = point.name + "." + variable->getName();
					if (!ReflectVertexInputs(variable, 0, path, reflection.vertexInputs, error)) {
						return Result::Failed;
					}
				}
			}

			if (point.stage == ShaderStage::Compute) {
				SlangUInt size[3]{};
				entry->getComputeThreadGroupSize(3, size);

				for (unsigned axis = 0; axis < 3; ++axis) {
					if (size[axis] == 0 || size[axis] > UINT32_MAX) {
						error = point.name + ": unresolved compute group size";
						return Result::Failed;
					}

					point.computeGroupSize[axis] = static_cast<uint32_t>(size[axis]);
				}
			}

			if (!ReflectPushData(entry->getTypeLayout(), reflection.pushData, foundPushData, error)) {
				return Result::Failed;
			}

			reflection.entryPoints.push_back(std::move(point));
		}

		std::sort(reflection.vertexInputs.begin(), reflection.vertexInputs.end(), [](const auto& a, const auto& b) {
			return a.location < b.location;
		});

		for (size_t i = 1; i < reflection.vertexInputs.size(); ++i) {
			if (reflection.vertexInputs[i - 1].location == reflection.vertexInputs[i].location) {
				error = reflection.vertexInputs[i].name + ": duplicate vertex input location";
				return Result::Failed;
			}
		}

		if (!ReflectVertexLayout(reflection, error)) {
			return Result::Failed;
		}

		outReflection = std::move(reflection);
		return Result::Success;
	}
}
