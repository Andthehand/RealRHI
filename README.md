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
│   ├── BasicExample.cpp   # Basic usage example
│   └── CMakeLists.txt     # Examples build configuration
└── CMakeLists.txt         # Root build configuration
```

## Prerequisites

- CMake 3.15 or higher
- C++20 compatible compiler
- Vulkan SDK (download from https://vulkan.lunarg.com/sdk/home)

### Installing Vulkan SDK

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

## Building

```bash
mkdir build
cd build
cmake ..
cmake --build .
```

## Running Examples

After building, run the basic example:

```bash
./examples/BasicExample
```

## Usage

The simplified API makes it easy to get started with Vulkan:

```cpp
#include "Device.h"
#include "CommandBuffer.h"

// Create device (automatically creates Vulkan instance)
RealEngine::Device device;
if (!device.Create("MyApp", /*enableValidation=*/true)) {
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

## Features

- Vulkan instance management integrated with device creation
- Physical and logical device abstraction
- Command buffer abstraction
- Debug validation layer support