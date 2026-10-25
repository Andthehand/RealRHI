#pragma once
#include "DeviceDesc.h"
#include "Result.h"
#include "ShaderDesc.h"

#include <slang/slang-com-ptr.h>
#include <slang/slang.h>

#include <filesystem>
#include <mutex>

namespace RealRHI {
	struct CompiledShaderProgram {
		std::vector<uint32_t> spirv;
		std::vector<std::string> exportedEntryPoints;
		ShaderReflection reflection;
	};

	class SlangShaderCompiler {
	public:
		SlangShaderCompiler(const std::filesystem::path& directory, bool debug, DebugCallback callback);
		Result Compile(const ShaderDesc& desc, CompiledShaderProgram& outProgram);

	private:
		bool Initialize(bool enableDebugInfo);
		Result ReflectExportedEntryPoints(CompiledShaderProgram& program, const std::string& context) const;
		bool Check(SlangResult result,
			const Slang::ComPtr<slang::IBlob>& diagnostics,
			const std::string& context) const;

	private:
		std::string m_Directory;
		DebugCallback m_Callback;

		std::mutex m_Mutex;
		Slang::ComPtr<slang::IGlobalSession> m_GlobalSession;
		Slang::ComPtr<slang::ISession> m_Session;
	};
}
