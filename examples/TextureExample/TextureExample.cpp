#include <SDL3/SDL.h>

#include "Device.h"
#include "FrameContext.h"

#include <iostream>
#include <vector>
#include <array>

constexpr uint32_t WIDTH = 800;
constexpr uint32_t HEIGHT = 600;

// Quad vertex: position (x, y) + texture coordinates (u, v)
struct Vertex {
    float pos[2];
    float texCoord[2];
};

// Full-screen quad (two triangles)
const std::vector<Vertex> vertices = {
    {{-0.5f, -0.5f}, {0.0f, 0.0f}},
    {{ 0.5f, -0.5f}, {1.0f, 0.0f}},
    {{ 0.5f,  0.5f}, {1.0f, 1.0f}},
    {{-0.5f,  0.5f}, {0.0f, 1.0f}},
};

const std::vector<uint32_t> indices = {0, 1, 2, 2, 3, 0};

std::unique_ptr<RealRHI::Device> device;
RealRHI::Ref<RealRHI::Window> window;
RealRHI::Ref<RealRHI::Swapchain> swapchain;
RealRHI::Ref<RealRHI::Pipeline> pipeline;
RealRHI::Ref<RealRHI::Buffer> vertexBuffer;
RealRHI::Ref<RealRHI::Buffer> indexBuffer;
RealRHI::Ref<RealRHI::Texture> texture;
RealRHI::Ref<RealRHI::Sampler> sampler;
std::vector<RealRHI::Ref<RealRHI::CommandList>> commandLists;

void CreateSwapChain() {
    RealRHI::Result result = device->CreateWindow({
        .Title = "RealRHI Texture Example",
        .Width = WIDTH,
        .Height = HEIGHT
    }, window);
    if (result != RealRHI::Result::Success) {
        std::cerr << "Failed to create window" << std::endl;
    }

    result = device->CreateSwapchain({
        .window = window.Raw(),
    }, swapchain);
    if (result != RealRHI::Result::Success) {
        std::cerr << "Failed to create swapchain" << std::endl;
    }
}

void CreateGraphicsPipeline() {
    const RealRHI::ShaderDesc shaderDesc{
        .moduleName = "shader",
    };

    RealRHI::Ref<RealRHI::Shader> shader;
    RealRHI::Result result = device->CreateShader(shaderDesc, shader);
    if (result != RealRHI::Result::Success) {
        std::cerr << "Failed to create shader" << std::endl;
    }

    RealRHI::PipelineDesc desc{
        .shader = shader.Raw(),
        .rasterState = {
            .cullMode = RealRHI::CullMode::None,
            .fillMode = RealRHI::FillMode::Solid,
            .frontCounterClockwise = false,
        },
        .depthState = {
            .depthTestEnable = false,
            .depthWriteEnable = false,
            .compareOp = RealRHI::CompareOp::Less,
        },
        .blendState = {
            .enable = false,
        },
        .renderTargetFormats = {
            .colorFormats = { swapchain->GetBackBufferFormat() },
            .depthFormat = RealRHI::TextureFormat::Unknown,
        },
        .descriptorBindings = {
            {
                .binding = 0,
                .type = RealRHI::DescriptorType::CombinedImageSampler,
                .count = 1,
            }
        },
    };

    result = device->CreateGraphicsPipeline(desc, pipeline);
    if (result != RealRHI::Result::Success) {
        std::cerr << "Failed to create pipeline" << std::endl;
    }
}

void CreateBuffers() {
    RealRHI::Result result = device->CreateBuffer({
        .size = static_cast<uint32_t>(sizeof(vertices[0]) * vertices.size()),
        .usage = RealRHI::BufferUsage::Vertex,
        .memoryUsage = RealRHI::MemoryUsage::CPUToGPU,
        .initialData = vertices.data(),
    }, vertexBuffer);
    if (result != RealRHI::Result::Success) {
        std::cerr << "Failed to create vertex buffer" << std::endl;
    }

    result = device->CreateBuffer({
        .size = static_cast<uint32_t>(sizeof(indices[0]) * indices.size()),
        .usage = RealRHI::BufferUsage::Index,
        .memoryUsage = RealRHI::MemoryUsage::CPUToGPU,
        .initialData = indices.data(),
    }, indexBuffer);
    if (result != RealRHI::Result::Success) {
        std::cerr << "Failed to create index buffer" << std::endl;
    }
}

void CreateCheckerboardTexture() {
    constexpr uint32_t TEX_SIZE = 8;

    // Generate a checkerboard pattern with magenta and cyan
    std::vector<uint8_t> pixels(TEX_SIZE * TEX_SIZE * 4);
    for (uint32_t y = 0; y < TEX_SIZE; y++) {
        for (uint32_t x = 0; x < TEX_SIZE; x++) {
            const bool isLight = (x + y) % 2 == 0;
            const uint32_t idx = (y * TEX_SIZE + x) * 4;
            pixels[idx + 0] = isLight ? 255 : 0;    // R
            pixels[idx + 1] = isLight ? 0   : 200;  // G
            pixels[idx + 2] = isLight ? 200 : 255;  // B
            pixels[idx + 3] = 255;                   // A
        }
    }

    RealRHI::Result result = device->CreateTexture({
        .width = TEX_SIZE,
        .height = TEX_SIZE,
        .depth = 1,
        .format = RealRHI::TextureFormat::RGBA8_UNorm,
        .usage = RealRHI::TextureUsage::ShaderResource | RealRHI::TextureUsage::TransferDst,
    }, texture);
    if (result != RealRHI::Result::Success) {
        std::cerr << "Failed to create texture" << std::endl;
        return;
    }

    result = texture->UploadData(pixels.data(), static_cast<uint32_t>(pixels.size()));
    if (result != RealRHI::Result::Success) {
        std::cerr << "Failed to upload texture data" << std::endl;
    }

    result = device->CreateSampler({
        .minFilter = RealRHI::SamplerFilter::Nearest,
        .magFilter = RealRHI::SamplerFilter::Nearest,
        .addressModeU = RealRHI::SamplerAddressMode::Repeat,
        .addressModeV = RealRHI::SamplerAddressMode::Repeat,
    }, sampler);
    if (result != RealRHI::Result::Success) {
        std::cerr << "Failed to create sampler" << std::endl;
    }
}

void CreateCommandLists() {
    commandLists.resize(swapchain->GetMaxFramesInFlight());
    for (auto& cmd : commandLists) {
        RealRHI::Result result = device->CreateCommandList(cmd);
        if (result != RealRHI::Result::Success) {
            std::cerr << "Failed to create command list" << std::endl;
        }
    }
}

void RecordCommandBuffer(RealRHI::CommandList* cmd, const RealRHI::FrameContext& frame) {
    if (cmd->Begin() != RealRHI::Result::Success) {
        std::cerr << "Failed to begin command list" << std::endl;
        return;
    }

    RealRHI::RenderingInfo renderingInfo{
        .colorAttachments = {
            {
                .target = frame.backBufferView,
                .loadOp = RealRHI::LoadOp::Clear,
                .storeOp = RealRHI::StoreOp::Store,
                .clearColor = {0.1f, 0.1f, 0.1f, 1.0f}
            }
        },
        .renderArea = {
            .x = 0,
            .y = 0,
            .width = frame.width,
            .height = frame.height
        }
    };

    cmd->BeginRendering(renderingInfo);
    cmd->BindPipeline(pipeline.Raw());

    RealRHI::Viewport viewport{
        .x = 0.0f,
        .y = 0.0f,
        .width = static_cast<float>(frame.width),
        .height = static_cast<float>(frame.height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    cmd->SetViewport(viewport);

    RealRHI::Rect scissor{
        .x = 0,
        .y = 0,
        .width = frame.width,
        .height = frame.height,
    };
    cmd->SetScissor(scissor);

    cmd->BindTexture(0, texture.Raw(), sampler.Raw());
    cmd->BindVertexBuffer(vertexBuffer.Raw());
    cmd->BindIndexBuffer(indexBuffer.Raw());
    cmd->DrawIndexed(static_cast<uint32_t>(indices.size()));
    cmd->EndRendering();

    if (cmd->End() != RealRHI::Result::Success) {
        std::cerr << "Failed to end command list" << std::endl;
    }
}

void DrawFrame() {
    RealRHI::FrameContext frame = swapchain->BeginFrame();

    auto* cmd = commandLists[frame.frameIndex].Raw();
    RecordCommandBuffer(cmd, frame);

    device->Submit(cmd, swapchain.Raw(), frame);
    swapchain->Present(frame);
}

void Cleanup() {
    device->WaitIdle();

    pipeline.Reset();
    vertexBuffer.Reset();
    indexBuffer.Reset();
    texture.Reset();
    sampler.Reset();
    commandLists.clear();
    swapchain.Reset();
    device.reset();
}

int main() {
    std::cout << "RealRHI Texture Example - Textured Quad Rendering" << std::endl;
    std::cout << "==================================================" << std::endl;

    RealRHI::DeviceDesc createInfo {
        .applicationName = "RealRHI Texture Example",
        .shaderDirectory = "shaders",
        .enableDebug = true,
        .enableValidationLayers = true,
    };

    RealRHI::Result result = RealRHI::Device::Create(createInfo, device);
    if (result != RealRHI::Result::Success) {
        std::cerr << " Failed to create device" << std::endl;
        return -1;
    }
    std::cout << " Device created successfully" << std::endl;

    CreateSwapChain();
    std::cout << " Swapchain created" << std::endl;

    CreateGraphicsPipeline();
    std::cout << " Graphics pipeline created" << std::endl;

    CreateBuffers();
    std::cout << " Vertex and index buffers created" << std::endl;

    CreateCheckerboardTexture();
    std::cout << " Checkerboard texture created and uploaded" << std::endl;

    CreateCommandLists();
    std::cout << " Command lists created" << std::endl;

    std::cout << std::endl << "Rendering textured quad..." << std::endl;

    bool running = true;
    SDL_Event event;
    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
        }
        DrawFrame();
    }

    Cleanup();

    std::cout << std::endl << "Program completed successfully!" << std::endl;
    return 0;
}
