#include "SlangShaderCompiler.h"

#include "SlangShaderReflection.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <spirv/unified1/spirv.h>

namespace RealRHI {
	SlangShaderCompiler::SlangShaderCompiler(const std::filesystem::path& directory, bool debug, DebugCallback callback)
		: m_Directory(std::filesystem::absolute(directory).lexically_normal().string()), m_Callback(callback) {
		if (!m_Callback) {
			m_Callback = DefaultDebugCallback;
		}

		if (!Initialize(debug)) {
			m_GlobalSession = nullptr;
		}
	}

	bool SlangShaderCompiler::Check(SlangResult result,
		const Slang::ComPtr<slang::IBlob>& diagnostics,
		const std::string& context) const {
		if (diagnostics || SLANG_FAILED(result)) {
			std::string message = context + ": ";
			if (diagnostics) {
				message.append(static_cast<const char*>(diagnostics->getBufferPointer()), diagnostics->getBufferSize());
			}
			else {
				message += "Slang operation failed without diagnostics.";
			}

			DebugSeverity severity = DebugSeverity::Warning;
			if (SLANG_FAILED(result)) {
				severity = DebugSeverity::Error;
			}

			m_Callback({severity, DebugMessageType::ShaderCompilation, message.c_str()});
		}

		return SLANG_SUCCEEDED(result);
	}

	bool SlangShaderCompiler::Initialize(bool enableDebugInfo) {
		if (!Check(slang::createGlobalSession(m_GlobalSession.writeRef()), {}, "Create global session")) {
			return false;
		}

		const auto capability = m_GlobalSession->findCapability("spvDescriptorHeapEXT");
		if (capability == SLANG_CAPABILITY_UNKNOWN) {
			return Check(SLANG_FAIL, {}, "Installed Slang compiler lacks spvDescriptorHeapEXT");
		}

		m_CompilerOptions = {
			{slang::CompilerOptionName::EmitSpirvDirectly, {slang::CompilerOptionValueKind::Int, 1}},
			{slang::CompilerOptionName::Capability, {slang::CompilerOptionValueKind::Int, capability}},
		};
		if (enableDebugInfo) {
			m_CompilerOptions.push_back(
				{slang::CompilerOptionName::DebugInformation,
					{slang::CompilerOptionValueKind::Int, SLANG_DEBUG_INFO_LEVEL_STANDARD}});
		}
		return true;
	}

	std::string SlangShaderCompiler::BuildCacheKey(const ShaderDesc& desc) {
		std::string key = std::to_string(std::strlen(desc.moduleName)) + ":" + desc.moduleName;
		for (const auto& entry : desc.entryPoints) {
			key += std::to_string(entry.name.size()) + ":" + entry.name;
			if (entry.stage) {
				key += ":" + std::to_string(static_cast<int>(*entry.stage));
			}
			else {
				key += ":auto";
			}
			key += ";";
		}
		return key;
	}

	bool SlangShaderCompiler::ReadDependency(const std::filesystem::path& path, std::string& contents) {
		std::ifstream file(path, std::ios::binary);
		if (!file) {
			return false;
		}
		contents.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
		return !file.bad();
	}

	bool SlangShaderCompiler::DependenciesMatch(const CachedProgram& cached) {
		for (const auto& dependency : cached.dependencies) {
			std::string contents;
			if (!ReadDependency(dependency.path, contents) || contents != dependency.contents) {
				return false;
			}
		}
		return true;
	}

	void SlangShaderCompiler::ClearCache() {
		std::scoped_lock lock(m_Mutex);
		m_Cache.clear();
	}

	Result SlangShaderCompiler::Compile(const ShaderDesc& desc, CompiledShaderProgram& outProgram) {
		std::scoped_lock lock(m_Mutex);
		const std::string context = std::string("CreateShader(") + (desc.moduleName ? desc.moduleName : "<null>") + ")";
		if (!desc.moduleName || !desc.moduleName[0]) {
			Check(SLANG_FAIL, {}, context + " requires a module name");
			return Result::InvalidParameter;
		}

		if (!m_GlobalSession) {
			Check(SLANG_FAIL, {}, context + " compilation session initialization failed");
			return Result::Failed;
		}

		// The compiler options and search directory are immutable and scoped to this cache.
		const auto key = BuildCacheKey(desc);
		const auto cached = m_Cache.find(key);
		if (cached != m_Cache.end()) {
			if (DependenciesMatch(cached->second)) {
				outProgram = cached->second.program;
				return Result::Success;
			}
			m_Cache.erase(cached);
		}

		// Slang sessions memoize loaded modules. A fresh session on a miss prevents stale
		// imports/includes after source edits or a previously failed compilation.
		const slang::TargetDesc target{.format = SLANG_SPIRV, .profile = m_GlobalSession->findProfile("spirv_1_6")};
		const char* directory = m_Directory.c_str();
		const slang::SessionDesc sessionDesc{
			.targets = &target,
			.targetCount = 1,
			.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR,
			.searchPaths = &directory,
			.searchPathCount = 1,
			.compilerOptionEntries = m_CompilerOptions.data(),
			.compilerOptionEntryCount = static_cast<uint32_t>(m_CompilerOptions.size()),
		};
		Slang::ComPtr<slang::ISession> session;
		if (!Check(m_GlobalSession->createSession(sessionDesc, session.writeRef()), {}, context + " create session")) {
			return Result::Failed;
		}

		Slang::ComPtr<slang::IBlob> diagnostics;
		auto module = Slang::ComPtr<slang::IModule>(session->loadModule(desc.moduleName, diagnostics.writeRef()));
		if (!Check(module ? SLANG_OK : SLANG_FAIL, diagnostics, context + " load module")) {
			return Result::Failed;
		}

		size_t entryCount = desc.entryPoints.size();
		if (desc.entryPoints.empty()) {
			entryCount = module->getDefinedEntryPointCount();
		}

		if (entryCount == 0) {
			Check(SLANG_FAIL, {}, context + " contains no shader entry points");
			return Result::InvalidParameter;
		}

		std::vector<Slang::ComPtr<slang::IEntryPoint>> entries(entryCount);
		std::vector<slang::IComponentType*> components{module.get()};

		for (size_t i = 0; i < entries.size(); ++i) {
			SlangResult result;
			std::string entryContext = context + " discover entry point " + std::to_string(i);
			if (desc.entryPoints.empty()) {
				result = module->getDefinedEntryPoint(static_cast<SlangInt32>(i), entries[i].writeRef());
			}
			else {
				entryContext = context + " find entry point " + desc.entryPoints[i].name;
				result = module->findEntryPointByName(desc.entryPoints[i].name.c_str(), entries[i].writeRef());
			}

			if (!Check(result, {}, entryContext) || !entries[i]) {
				return Result::Failed;
			}

			components.push_back(entries[i].get());
		}

		Slang::ComPtr<slang::IComponentType> composed;
		auto result = session->createCompositeComponentType(
			components.data(), components.size(), composed.writeRef(), diagnostics.writeRef());
		if (!Check(result, diagnostics, context + " compose program") || !composed) {
			return Result::Failed;
		}

		Slang::ComPtr<slang::IComponentType> linked;
		result = composed->link(linked.writeRef(), diagnostics.writeRef());
		if (!Check(result, diagnostics, context + " link program") || !linked) {
			return Result::Failed;
		}

		auto* layout = linked->getLayout(0, diagnostics.writeRef());
		if (!Check(layout ? SLANG_OK : SLANG_FAIL, diagnostics, context + " query layout")) {
			return Result::Failed;
		}

		CompiledShaderProgram program;
		std::string error;
		if (SlangShaderReflection::Reflect(layout, program.reflection, error) != Result::Success) {
			Check(SLANG_FAIL, {}, context + " reflect: " + error);
			return Result::Failed;
		}

		if (program.reflection.entryPoints.size() != entries.size()) {
			Check(SLANG_FAIL, {}, context + " reflected entry-point count differs from selection");
			return Result::Failed;
		}

		for (size_t i = 0; i < desc.entryPoints.size(); ++i) {
			if (desc.entryPoints[i].stage && program.reflection.entryPoints[i].stage != *desc.entryPoints[i].stage) {
				Check(SLANG_FAIL, {}, context + " stage mismatch for " + desc.entryPoints[i].name);
				return Result::InvalidParameter;
			}
		}

		bool vertex = false;
		bool fragment = false;
		bool compute = false;

		for (const auto& entry : program.reflection.entryPoints) {
			bool* present = nullptr;

			switch (entry.stage) {
				case ShaderStage::Vertex:
					present = &vertex;
					break;
				case ShaderStage::Fragment:
					present = &fragment;
					break;
				case ShaderStage::Compute:
					present = &compute;
					break;
			}

			if (!present || *present) {
				Check(SLANG_FAIL,
					{},
					context + " contains duplicate or unsupported stages; select entry-point names explicitly");
				return Result::InvalidParameter;
			}

			*present = true;
		}

		if ((compute && (vertex || fragment)) || (!compute && !vertex)) {
			Check(SLANG_FAIL,
				{},
				context +
					" contains multiple programs or lacks a vertex stage; select a graphics or compute program using "
					"entryPoints");
			return Result::InvalidParameter;
		}

		Slang::ComPtr<slang::IBlob> code;
		result = linked->getTargetCode(0, code.writeRef(), diagnostics.writeRef());
		if (!Check(result, diagnostics, context + " emit SPIR-V") || !code || code->getBufferSize() < 20 ||
			code->getBufferSize() % 4) {
			return Result::Failed;
		}

		// SPIR-V consists of 32-bit words.
		program.spirv.resize(code->getBufferSize() / 4);
		std::memcpy(program.spirv.data(), code->getBufferPointer(), code->getBufferSize());

		if (ReflectExportedEntryPoints(program, context) != Result::Success) {
			return Result::Failed;
		}

		CachedProgram entry;
		bool cacheable = module->getDependencyFileCount() > 0;
		for (SlangInt32 i = 0; i < module->getDependencyFileCount(); ++i) {
			Dependency dependency;
			dependency.path = module->getDependencyFilePath(i);
			if (!ReadDependency(dependency.path, dependency.contents)) {
				cacheable = false;
				break;
			}
			entry.dependencies.push_back(std::move(dependency));
		}
		program.cacheId = m_NextCacheId++;
		if (cacheable) {
			entry.program = program;
			m_Cache.emplace(key, std::move(entry));
		}
		outProgram = std::move(program);
		return Result::Success;
	}

	Result SlangShaderCompiler::ReflectExportedEntryPoints(CompiledShaderProgram& program,
		const std::string& context) const {
		// Read exported names from the emitted module: Slang may rename a single entry point to main.
		program.exportedEntryPoints.resize(program.reflection.entryPoints.size());

		for (size_t offset = 5; offset < program.spirv.size();) {
			const uint32_t instruction = program.spirv[offset];
			const uint32_t wordCount = instruction >> 16;
			if (!wordCount || wordCount > program.spirv.size() - offset) {
				Check(SLANG_FAIL, {}, context + " malformed SPIR-V instruction");
				return Result::Failed;
			}

			if ((instruction & 0xffff) == SpvOpEntryPoint && wordCount >= 4) {
				const uint32_t model = program.spirv[offset + 1];
				const char* name = reinterpret_cast<const char*>(&program.spirv[offset + 3]);
				const void* end = std::memchr(name, 0, (wordCount - 3) * 4);
				if (!end) {
					Check(SLANG_FAIL, {}, context + " malformed SPIR-V entry-point name");
					return Result::Failed;
				}

				for (size_t i = 0; i < program.reflection.entryPoints.size(); ++i) {
					const auto stage = program.reflection.entryPoints[i].stage;
					if ((model == SpvExecutionModelVertex && stage == ShaderStage::Vertex) ||
						(model == SpvExecutionModelFragment && stage == ShaderStage::Fragment) ||
						(model == SpvExecutionModelGLCompute && stage == ShaderStage::Compute)) {
						if (!program.exportedEntryPoints[i].empty()) {
							Check(SLANG_FAIL, {}, context + " emitted multiple entry points for one stage");
							return Result::Failed;
						}

						program.exportedEntryPoints[i].assign(name, static_cast<const char*>(end) - name);
					}
				}
			}

			offset += wordCount;
		}

		for (const auto& name : program.exportedEntryPoints) {
			if (name.empty()) {
				Check(SLANG_FAIL, {}, context + " missing emitted entry point");
				return Result::Failed;
			}
		}

		return Result::Success;
	}

}
