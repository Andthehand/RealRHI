#pragma once
#include "Texture.h"
#include "Pipeline.h"
#include "Buffer.h"

namespace RealRHI {
    class CommandList {
    public:
        virtual void Begin() = 0;
        virtual void End() = 0;

        virtual void BeginRenderPass(Texture* target) = 0;
        virtual void EndRenderPass() = 0;

        virtual void BindPipeline(Pipeline*) = 0;
        virtual void BindVertexBuffer(Buffer*) = 0;

        virtual void Draw(uint32_t vertexCount) = 0;
    };
}
