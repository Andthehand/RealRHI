#pragma once
#include "Result.h"
#include "Shader.h"
#include "ShaderDesc.h"
#include "VulkanDevice.h"

namespace RealRHI {
	class VulkanShader : public Shader {
	public:
		VulkanShader(const VulkanDevice* device);
		~VulkanShader();

		static Result Create(const VulkanDevice* device,
			const CompiledShaderProgram& program,
			const char* debugName,
			Ref<VulkanShader>& outShader);
		Result Init(const CompiledShaderProgram& program, const char* debugName);

		const ShaderReflection& GetReflection() const override { return m_Reflection; }
		void Bind(VkCommandBuffer commands) const;

	private:
		const VulkanDevice* m_Device = nullptr;

		std::vector<VkShaderEXT> m_Shaders;
		std::vector<VkShaderStageFlagBits> m_Stages;
		ShaderReflection m_Reflection;
		uint32_t m_CountedStages = 0;
	};
}
