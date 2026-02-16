#pragma once
#include "Shader.h"

#include <filesystem>

namespace RealRHI {
	class VulkanShader : public Shader {
	public:
		VulkanShader(std::filesystem::path& slangFile);
		virtual ~VulkanShader() = default;
	};
}
