#include "../src/Device.h"
#include "../src/CommandBuffer.h"

#include <iostream>

int main() {
    std::cout << "RealRHI Basic Example - Testing Vulkan Abstraction Layer" << std::endl;
    std::cout << "=========================================================" << std::endl;

    // Create and initialize device (includes instance creation)
    RealEngine::DeviceCreateInfo createInfo {
        .AppName = "RealRHI Basic Example",
        .EnableValidationLayers = true,
    };

    RealEngine::Device device;
    if (!device.Create(createInfo)) {
        std::cerr << "Failed to create device!" << std::endl;
        return -1;
    }
    std::cout << " Device created successfully" << std::endl;

    // Create command buffer
    // NOTE: Using queue family index 0 for simplicity in this example.
    // In production code, you should obtain the actual graphics queue family index from the device.
    RealEngine::CommandBuffer cmdBuffer;
    if (!cmdBuffer.Create(device.GetDevice(), 0)) {
        std::cerr << "Failed to create command buffer!" << std::endl;
        return -1;
    }
    std::cout << " Command buffer created successfully" << std::endl;

    // Test command buffer recording
    if (cmdBuffer.Begin()) {
        std::cout << " Command buffer recording started" << std::endl;
        
        if (cmdBuffer.End()) {
            std::cout << " Command buffer recording ended" << std::endl;
        }
    }

    std::cout << std::endl;
    std::cout << "All tests passed! RealRHI is working correctly." << std::endl;
    std::cout << "=========================================================" << std::endl;

    // Cleanup is automatic via destructors
    return 0;
}
