#include <SDL3/SDL.h>

#include "Device.h"
#include "FrameContext.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

constexpr uint32_t WIDTH = 800;
constexpr uint32_t HEIGHT = 600;
constexpr uint32_t TEXTURE_WIDTH = 256;
constexpr uint32_t TEXTURE_HEIGHT = 256;
constexpr uint32_t TEXTURE_COUNT = 2;

struct Vertex {
    float pos[2];
    float uv[2];
    uint32_t texIdx;
    uint32_t samplerIdx;
};

std::vector<Vertex> BuildQuadVertices(float minX, float maxX, uint32_t textureIndex, uint32_t samplerIndex) {
    return {
        {{minX, -0.75f}, {0.0f, 1.0f}, textureIndex, samplerIndex},
        {{maxX, -0.75f}, {1.0f, 1.0f}, textureIndex, samplerIndex},
        {{maxX,  0.75f}, {1.0f, 0.0f}, textureIndex, samplerIndex},
        {{minX, -0.75f}, {0.0f, 1.0f}, textureIndex, samplerIndex},
        {{maxX,  0.75f}, {1.0f, 0.0f}, textureIndex, samplerIndex},
        {{minX,  0.75f}, {0.0f, 0.0f}, textureIndex, samplerIndex},
    };
}

std::vector<Vertex> vertices;

std::unique_ptr<RealRHI::Device> device;
RealRHI::Ref<RealRHI::Window> window;
RealRHI::Ref<RealRHI::Swapchain> swapchain;
RealRHI::Ref<RealRHI::Pipeline> pipeline;
RealRHI::Ref<RealRHI::Buffer> vertexBuffer;
std::vector<RealRHI::Ref<RealRHI::Texture>> textures;
std::vector<RealRHI::Ref<RealRHI::CommandList>> commandLists;

std::vector<uint8_t> BuildCheckerboardTexture() {
    std::vector<uint8_t> pixels(TEXTURE_WIDTH * TEXTURE_HEIGHT * 4);

    for (uint32_t y = 0; y < TEXTURE_HEIGHT; ++y) {
        for (uint32_t x = 0; x < TEXTURE_WIDTH; ++x) {
            const bool checker = ((x / 32) + (y / 32)) % 2 == 0;
            const uint8_t red = checker ? 255 : 40;
            const uint8_t green = checker ? static_cast<uint8_t>((x * 255) / (TEXTURE_WIDTH - 1)) : 180;
            const uint8_t blue = checker ? static_cast<uint8_t>((y * 255) / (TEXTURE_HEIGHT - 1)) : 255;

            const size_t pixelIndex = static_cast<size_t>((y * TEXTURE_WIDTH + x) * 4);
            pixels[pixelIndex + 0] = red;
            pixels[pixelIndex + 1] = green;
            pixels[pixelIndex + 2] = blue;
            pixels[pixelIndex + 3] = 255;
        }
    }

    return pixels;
}

std::vector<uint8_t> BuildStripeTexture() {
    std::vector<uint8_t> pixels(TEXTURE_WIDTH * TEXTURE_HEIGHT * 4);

    for (uint32_t y = 0; y < TEXTURE_HEIGHT; ++y) {
        for (uint32_t x = 0; x < TEXTURE_WIDTH; ++x) {
            const bool stripe = ((x / 16) % 2) == 0;
            const uint8_t red = stripe ? 40 : 250;
            const uint8_t green = stripe ? static_cast<uint8_t>((y * 255) / (TEXTURE_HEIGHT - 1)) : 100;
            const uint8_t blue = stripe ? 255 : static_cast<uint8_t>((x * 255) / (TEXTURE_WIDTH - 1));

            const size_t pixelIndex = static_cast<size_t>((y * TEXTURE_WIDTH + x) * 4);
            pixels[pixelIndex + 0] = red;
            pixels[pixelIndex + 1] = green;
            pixels[pixelIndex + 2] = blue;
            pixels[pixelIndex + 3] = 255;
        }
    }

    return pixels;
}

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
        .moduleName = "texture",
    };

    RealRHI::Ref<RealRHI::Shader> shader;
    RealRHI::Result result = device->CreateShader(shaderDesc, shader);
    if (result != RealRHI::Result::Success) {
        std::cerr << "Failed to create shader" << std::endl;
    }

    RealRHI::PipelineDesc desc{
        .shader = shader.Raw(),
        .rasterState = {
            .cullMode = RealRHI::CullMode::Back,
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
    };

    result = device->CreateGraphicsPipeline(desc, pipeline);
    if (result != RealRHI::Result::Success) {
        std::cerr << "Failed to create pipeline" << std::endl;
    }
}

void CreateVertexBuffer() {
    if (textures.size() != TEXTURE_COUNT) {
        std::cerr << "Failed to create vertex buffer: textures not created" << std::endl;
        return;
    }

    vertices.clear();
    vertices.reserve(TEXTURE_COUNT * 6);

    struct QuadDesc {
        float minX;
        float maxX;
    };

    const QuadDesc quadDescs[TEXTURE_COUNT] = {
        { -0.95f, -0.05f },
        { 0.05f, 0.95f },
    };

    for (uint32_t i = 0; i < TEXTURE_COUNT; ++i) {
        if (textures[i].Raw() == nullptr) {
            std::cerr << "Failed to create vertex buffer: texture not created" << std::endl;
            return;
        }

        const uint32_t textureIndex = textures[i]->GetSampledImageDescriptorIndex();
        const uint32_t samplerIndex = textures[i]->GetSamplerDescriptorIndex();
        if (textureIndex == std::numeric_limits<uint32_t>::max() || samplerIndex == std::numeric_limits<uint32_t>::max()) {
            std::cerr << "Failed to create vertex buffer: invalid texture or sampler descriptor index" << std::endl;
            return;
        }

        std::vector<Vertex> quadVertices = BuildQuadVertices(quadDescs[i].minX, quadDescs[i].maxX, textureIndex, samplerIndex);
        vertices.insert(vertices.end(), quadVertices.begin(), quadVertices.end());
    }

    RealRHI::Result result = device->CreateBuffer({
        .size = sizeof(Vertex) * vertices.size(),
        .usage = RealRHI::BufferUsage::Vertex,
        .memoryUsage = RealRHI::MemoryUsage::CPUToGPU,
        .initialData = vertices.data(),
    }, vertexBuffer);
    if (result != RealRHI::Result::Success) {
        std::cerr << "Failed to create vertex buffer" << std::endl;
    }
}

void CreateTextures() {
    textures.resize(TEXTURE_COUNT);

    const std::vector<uint8_t> texturePixels[TEXTURE_COUNT] = {
        BuildCheckerboardTexture(),
        BuildStripeTexture(),
    };

    for (uint32_t i = 0; i < TEXTURE_COUNT; ++i) {
        RealRHI::Result result = device->CreateTexture({
            .width = TEXTURE_WIDTH,
            .height = TEXTURE_HEIGHT,
            .depth = 1,
            .mipLevels = 1,
            .arrayLayers = 1,
            .format = RealRHI::TextureFormat::RGBA8_UNorm,
            .usage = RealRHI::TextureUsage::ShaderResource | RealRHI::TextureUsage::TransferDst,
        }, textures[i]);
        if (result != RealRHI::Result::Success) {
            std::cerr << "Failed to create texture" << std::endl;
            return;
        }

        result = textures[i]->UploadData(texturePixels[i].data(), static_cast<uint32_t>(texturePixels[i].size()));
        if (result != RealRHI::Result::Success) {
            std::cerr << "Failed to upload texture data" << std::endl;
            return;
        }
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
                .clearColor = {0.08f, 0.10f, 0.14f, 1.0f}
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

    cmd->BindVertexBuffer(vertexBuffer.Raw());
    cmd->Draw(static_cast<uint32_t>(vertices.size()));
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

    for (auto& texture : textures) {
        texture.Reset();
    }
    textures.clear();
    pipeline.Reset();
    vertexBuffer.Reset();
    commandLists.clear();
    swapchain.Reset();
    device.reset();
}

int main() {
    std::cout << "RealRHI Texture Example - Raw CPU Buffer Upload" << std::endl;
    std::cout << "================================================" << std::endl;

    RealRHI::DeviceDesc createInfo{
        .applicationName = "RealRHI Texture Example",
        .shaderDirectory = "shaders",
        .enableDebug = true,
        .enableValidationLayers = true,
    };

    RealRHI::Result result = RealRHI::Device::Create(createInfo, device);
    if (result != RealRHI::Result::Success) {
        std::cerr << "Failed to create device" << std::endl;
        return -1;
    }
    std::cout << "Device created successfully" << std::endl;

    CreateSwapChain();
    CreateGraphicsPipeline();
    CreateTextures();
    CreateVertexBuffer();
    CreateCommandLists();

    std::cout << "Rendering procedural textures..." << std::endl;

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
    return 0;
}
