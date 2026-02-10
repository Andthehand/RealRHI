# RealRHI

A Vulkan abstraction layer for RealEngine game engine.

## Project Structure

```
RealRHI/
├── src/                    # Source files (.cpp and .h)
│   ├── Device.h           # Device and instance management
│   ├── Device.cpp
│   ├── CommandBuffer.h    # Command buffer abstraction
│   ├── CommandBuffer.cpp
│   └── CMakeLists.txt     # Library build configuration
├── examples/              # Examples for testing/developing the API
│   ├── BasicExample/      # Basic usage example
│   │   ├── BasicExample.cpp
│   │   └── CMakeLists.txt
│   ├── TriangleExample/   # Triangle rendering with GLFW
│   │   ├── TriangleExample.cpp
│   │   └── CMakeLists.txt
│   └── CMakeLists.txt     # Examples build configuration
├── external/              # External dependencies
│   └── glfw/              # GLFW submodule
└── CMakeLists.txt         # Root build configuration
```

## Prerequisites

- CMake 3.15 or higher
- C++20 compatible compiler
- Vulkan SDK (download from https://vulkan.lunarg.com/sdk/home)
- Git (for cloning with submodules)

### Installing Dependencies

#### Vulkan SDK

**Windows:**
Download and run the installer from [LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home)

**Linux:**
```bash
# Ubuntu/Debian (replace 'jammy' with your Ubuntu version: focal for 20.04, jammy for 22.04, noble for 24.04)
wget -qO - https://packages.lunarg.com/lunarg-signing-key-pub.asc | sudo apt-key add -
sudo wget -qO /etc/apt/sources.list.d/lunarg-vulkan-jammy.list https://packages.lunarg.com/vulkan/lunarg-vulkan-jammy.list
sudo apt update
sudo apt install vulkan-sdk
```

## Cloning the Repository

This project uses GLFW as a git submodule. Clone with submodules using:

```bash
git clone --recursive https://github.com/Andthehand/RealRHI.git
```

Or if you've already cloned the repository:

```bash
git submodule update --init --recursive
```

## Building

```bash
mkdir build
cd build
cmake ..
cmake --build .
```

## Running Examples

After building, you can run the examples:

**Basic Example** (tests the API without rendering):
```bash
./examples/BasicExample
```

**Triangle Example** (renders a colorful triangle using GLFW):
```bash
./examples/TriangleExample
```

**Note:** Both examples require a Vulkan-compatible GPU to run. In headless or GPU-less environments (such as CI systems), the examples will fail during device creation. This is expected behavior as Vulkan requires actual GPU hardware.

## Usage

### Basic API Usage

The simplified API makes it easy to get started with Vulkan:

```cpp
#include "Device.h"
#include "CommandBuffer.h"

// Create device (automatically creates Vulkan instance)
RealEngine::Device device;
RealEngine::DeviceCreateInfo createInfo {
    .AppName = "MyApp",
    .EnableValidationLayers = true,
};

if (!device.Create(createInfo)) {
    // Handle error
}

// Create command buffer
RealEngine::CommandBuffer cmd;
cmd.Create(device.GetDevice(), 0);

// Record commands
cmd.Begin();
// ... record Vulkan commands ...
cmd.End();

// Cleanup is automatic via destructors
```

### Window Rendering with GLFW

For rendering to a window, you need to provide the required extensions:

```cpp
#include "Device.h"
#include <GLFW/glfw3.h>

// Initialize GLFW
glfwInit();
glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
GLFWwindow* window = glfwCreateWindow(800, 600, "My App", nullptr, nullptr);

// Get GLFW required extensions
uint32_t glfwExtensionCount = 0;
const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

// Create device with surface support
RealEngine::DeviceCreateInfo createInfo {
    .AppName = "MyApp",
    .EnableValidationLayers = true,
    .RequiredExtensions = extensions,
};

RealEngine::Device device;
if (!device.Create(createInfo)) {
    // Handle error
}

// Create surface
VkSurfaceKHR surface;
glfwCreateWindowSurface(device.GetInstance(), window, nullptr, &surface);

// ... create swapchain, pipeline, etc. (see TriangleExample.cpp for full example)
```

## Features

- Vulkan instance management integrated with device creation
- Physical and logical device abstraction
- Command buffer abstraction
- Debug validation layer support
- Surface extension support for window rendering
- Swapchain device extension support

## Examples

### BasicExample
A simple headless example that demonstrates basic device and command buffer creation without rendering.

### TriangleExample
A complete rendering example using GLFW that:
- Creates a window with GLFW
- Sets up a Vulkan swapchain
- Creates a graphics pipeline with embedded shaders
- Renders a colorful triangle with vertex colors
- Implements a proper render loop with synchronization

See `examples/TriangleExample/TriangleExample.cpp` for a complete, self-contained example of rendering with RealRHI.