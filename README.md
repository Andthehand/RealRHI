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
│   ├── TriangleExample/   # Triangle rendering with SDL3
│   │   ├── TriangleExample.cpp
│   │   └── CMakeLists.txt
│   └── CMakeLists.txt     # Examples build configuration
└── CMakeLists.txt         # Root build configuration
```

## Prerequisites

- CMake 3.15 or higher
- C++20 compatible compiler
- Vulkan SDK (download from https://vulkan.lunarg.com/sdk/home)
- SDL3 (for window rendering examples)

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

#### SDL3

**Note:** SDL3 is included with the Vulkan SDK and will be automatically found by CMake using the find_library command. The SDL3 library should be located in `$VULKAN_SDK/lib`.

If you need to install SDL3 separately, you can build it from source:

```bash
git clone https://github.com/libsdl-org/SDL.git
cd SDL
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
sudo cmake --install .
```

## Cloning the Repository

```bash
git clone https://github.com/Andthehand/RealRHI.git
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

**Triangle Example** (renders a colorful triangle using SDL3):
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

### Window Rendering with SDL3

For rendering to a window, you need to provide the required extensions:

```cpp
#include "Device.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

// Initialize SDL3
SDL_Init(SDL_INIT_VIDEO);
SDL_Window* window = SDL_CreateWindow("My App", 800, 600, SDL_WINDOW_VULKAN);

// Get SDL3 required extensions
uint32_t extensionCount = 0;
const char* const* extensionNames = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
std::vector<const char*> extensions(extensionNames, extensionNames + extensionCount);

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
if (!SDL_Vulkan_CreateSurface(window, device.GetInstance(), nullptr, &surface)) {
    std::cerr << "Failed to create Vulkan surface: " << SDL_GetError() << std::endl;
    // Handle error
}

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
A complete rendering example using SDL3 that:
- Creates a window with SDL3
- Sets up a Vulkan swapchain
- Creates a graphics pipeline with embedded shaders
- Renders a colorful triangle with vertex colors
- Implements a proper render loop with synchronization

See `examples/TriangleExample/TriangleExample.cpp` for a complete, self-contained example of rendering with RealRHI.

### TextureExample
A rendering example using SDL3 that demonstrates texture creation and sampling:
- Creates a procedurally generated checkerboard texture
- Uploads pixel data to the GPU via a staging buffer
- Creates a sampler with nearest-neighbor filtering
- Creates a graphics pipeline with a descriptor set layout for a combined image sampler
- Renders a textured quad using index buffer and texture coordinates
- Demonstrates the `BindTexture` API for per-draw texture binding

See `examples/TextureExample/TextureExample.cpp` for a complete, self-contained example of textured rendering with RealRHI.