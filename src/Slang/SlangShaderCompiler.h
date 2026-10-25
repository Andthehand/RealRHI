#pragma once
#include "DeviceDesc.h"
#include "Result.h"
#include "ShaderDesc.h"

#include <slang/slang-com-ptr.h>
#include <slang/slang.h>

#include <filesystem>
#include <mutex>
#include <unordered_map>

namespace RealRHI {
	struct CompiledShaderProgram {
		// Unique within this compiler; unchanged on a cache hit.
		uint64_t cacheId = 0;
		std::vector<uint32_t> spirv;
		std::vector<std::string> exportedEntryPoints;
		ShaderReflection reflection;
	};

	class SlangShaderCompiler {
	public:
		SlangShaderCompiler(const std::filesystem::path& directory, bool debug, DebugCallback callback);
		Result Compile(const ShaderDesc& desc, CompiledShaderProgram& outProgram);
		void ClearCache();

	private:
		struct Dependency {
			std::filesystem::path path;
			std::string contents;
		};
		struct CachedProgram {
			CompiledShaderProgram program;
			std::vector<Dependency> dependencies;
		};

	private:
		static std::string BuildCacheKey(const ShaderDesc& desc);
		static bool ReadDependency(const std::filesystem::path& path, std::string& contents);
		static bool DependenciesMatch(const CachedProgram& cached);
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
		std::vector<slang::CompilerOptionEntry> m_CompilerOptions;
		std::unordered_map<std::string, CachedProgram> m_Cache;
		uint64_t m_NextCacheId = 1;
	};
}
