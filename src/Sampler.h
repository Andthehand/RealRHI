#pragma once
#include "RefCounted.h"

namespace RealRHI {
    class Sampler : public RefCounted {
    public:
        virtual ~Sampler() = default;
    };
}
