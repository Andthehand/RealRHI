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
- Vulkan SDK

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