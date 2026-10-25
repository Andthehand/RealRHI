#pragma once
#include "Result.h"
#include "Shader.h"

#include <slang/slang.h>

namespace RealRHI {
	class SlangShaderReflection {
	public:
		static Result Reflect(slang::ProgramLayout* layout, ShaderReflection& outReflection, std::string& error);

	private:
		static bool ReflectVertexLayout(ShaderReflection& reflection, std::string& error);

		static bool ReflectVertexInputs(slang::VariableLayoutReflection* variable,
			size_t parentLocation,
			const std::string& path,
			std::vector<ShaderVertexInput>& inputs,
			std::string& error);
		static bool ReflectInputType(slang::TypeLayoutReflection* type,
			size_t location,
			const std::string& path,
			std::vector<ShaderVertexInput>& inputs,
			std::string& error);
		static bool ReflectPushData(slang::TypeLayoutReflection* type,
			ShaderPushData& pushData,
			bool& found,
			std::string& error);
	};
}
