# RealRHI

A Vulkan abstraction layer for RealEngine game engine.

## Project Structure

```
RealRHI/
├── src/                    # Source files (.cpp and .h)
│   ├── RealRHI.h          # Main RHI class
│   ├── RealRHI.cpp
│   ├── Device.h           # Device abstraction
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
- C++17 compatible compiler
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

**macOS:**
```bash
# Using Homebrew
brew install vulkan-sdk
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

## Features

- Vulkan instance management
- Physical and logical device abstraction
- Command buffer abstraction
- Debug validation layer support