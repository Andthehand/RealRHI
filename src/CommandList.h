#pragma once
#include "RefCounted.h"
#include "Result.h"

#include "Texture.h"
#include "Pipeline.h"
#include "Buffer.h"

#include "RenderingInfo.h"

namespace RealRHI {
    class CommandList : public RefCounted {
    public:
        virtual ~CommandList() = default;

        virtual Result Begin() = 0;
        virtual Result End() = 0;

        virtual void BeginRendering(const RenderingInfo& renderingInfo) = 0;
        virtual void EndRendering() = 0;

        virtual void SetViewport(const Viewport& vp) = 0;
        virtual void SetScissor(const Rect& rect) = 0;

        virtual void BindPipeline(Pipeline* pipeline) = 0;
        virtual void BindVertexBuffer(Buffer* vertexBuffer) = 0;
		virtual void BindIndexBuffer(Buffer* indexBuffer) = 0;

        virtual void Draw(uint32_t vertexCount) = 0;
		virtual void DrawIndexed(uint32_t indexCount) = 0;
    };
}
