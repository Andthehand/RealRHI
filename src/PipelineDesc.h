#pragma once
#include "Shader.h"
#include "TextureFormat.h"
#include "BufferAttributes.h"

#include <cstdint>
#include <vector>
#include <memory>

namespace RealRHI {
    enum class CullMode {
        None,
        Front,
        Back,
    };

    enum class FillMode {
        Solid,
        Wireframe,
    };

    struct RasterState {
        CullMode cullMode = CullMode::Back;
        FillMode fillMode = FillMode::Solid;
        bool frontCounterClockwise = false;
    };

    enum class CompareOp {
        Never,
        Less,
        Equal,
        LessEqual,
        Greater,
        NotEqual,
        GreaterEqual,
        Always,
    };

    struct DepthState {
        bool depthTestEnable = true;
        bool depthWriteEnable = true;
        CompareOp compareOp = CompareOp::Less;
    };

    struct BlendState {
        bool enable = false;
    };

    struct RenderTargetFormats {
        std::vector<TextureFormat> colorFormats;
        TextureFormat depthFormat = TextureFormat::Unknown;
    };

    struct PipelineDesc {
        std::unique_ptr<Shader> shader;

        BufferAttributes vertexLayout;

        RasterState rasterState;
        DepthState depthState;
        BlendState blendState;

        RenderTargetFormats renderTargetFormats;
    };
}
