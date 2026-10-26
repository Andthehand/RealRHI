#include <SDL3/SDL.h>
#include <slang/slang.h>
#include <vma/vk_mem_alloc.h>

#include <cstdio>
#include <spirv/unified1/spirv.h>
#include <vulkan/vulkan.h>

#ifndef VK_EXT_descriptor_heap
#error RealRHI requires Vulkan headers with VK_EXT_descriptor_heap.
#endif
#if SDL_MAJOR_VERSION != 3
#error RealRHI requires SDL3.
#endif

int main() {
	VkPhysicalDeviceDescriptorHeapFeaturesEXT heap{};
	(void)heap;
	slang::IGlobalSession* session = nullptr;
	if (SLANG_FAILED(slang::createGlobalSession(&session)) || !session) {
		std::fputs("Could not initialize Slang.\n", stderr);
		return 1;
	}
	const auto capability = session->findCapability("spvDescriptorHeapEXT");
	std::printf("Slang %s; Vulkan headers %u; SDL %d.%d.%d",
		session->getBuildTagString(),
		VK_HEADER_VERSION,
		SDL_MAJOR_VERSION,
		SDL_MINOR_VERSION,
		SDL_MICRO_VERSION);
	session->release();
	if (capability == SLANG_CAPABILITY_UNKNOWN) {
		std::fputs("\nSlang lacks spvDescriptorHeapEXT.\n", stderr);
		return 2;
	}
	return 0;
}
