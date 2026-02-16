#pragma once
#include <cstdint>
#include <vector>

#include "TextureDesc.h"

namespace RealRHI {
    enum class VertexFormat {
        Float,
        Float2,
        Float3,
        Float4,
        UInt,
        UInt2,
        UInt3,
        UInt4,
    };

    struct VertexAttribute {
        uint32_t Location;
        VertexFormat Format;
        uint32_t Offset;
    };

    struct VertexLayout {
        uint32_t Stride;
        std::vector<VertexAttribute> Attributes;
    };

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
        CullMode CullMode = CullMode::Back;
        FillMode FillMode = FillMode::Solid;
        bool FrontCounterClockwise = false;
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
        bool DepthTestEnable = true;
        bool DepthWriteEnable = true;
        CompareOp CompareOp = CompareOp::Less;
    };

    struct BlendState {
        bool Enable = false;
    };

    struct RenderTargetFormats {
        std::vector<Format> ColorFormats;
        Format DepthFormat = Format::Unknown;
    };

    struct PipelineDesc {
        // TODO: Replace with shader objects
        std::vector<uint32_t> VertexShader;
        std::vector<uint32_t> FragmentShader;

        VertexLayout VertexLayout;

        RasterState RasterState;
        DepthState DepthState;
        BlendState BlendState;

        RenderTargetFormats RenderTargetFormats;
    };
}
