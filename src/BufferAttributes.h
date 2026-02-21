#pragma once
#include <cstdint>
#include <vector>

namespace RealRHI {
    enum class DataType : uint8_t {
        Float, Float2, Float3, Float4,
        Uint, Uint2, Uint3, Uint4,
        Int, Int2, Int3, Int4,
        Undefined
    };

    inline uint8_t ElementCount(DataType type) {
        switch (type) {
        case DataType::Float:
        case DataType::Uint:
        case DataType::Int:   return 1;

        case DataType::Float2:
        case DataType::Uint2:
        case DataType::Int2:  return 2;

        case DataType::Float3:
        case DataType::Uint3:
        case DataType::Int3:  return 3;

        case DataType::Float4:
        case DataType::Uint4:
        case DataType::Int4:  return 4;

        default: return 0;
        }
    }

    inline uint8_t BaseSize(DataType type) {
        switch (type) {
        case DataType::Float:
        case DataType::Float2:
        case DataType::Float3:
        case DataType::Float4:
            return sizeof(float);

        case DataType::Uint:
        case DataType::Uint2:
        case DataType::Uint3:
        case DataType::Uint4:
            return sizeof(uint32_t);

        case DataType::Int:
        case DataType::Int2:
        case DataType::Int3:
        case DataType::Int4:
            return sizeof(int32_t);

        default: return 0;
        }
    }

    struct BufferAttribute {
        DataType Type;
        uint32_t InstanceDivisor = 0;
        uint32_t Offset = 0;

        BufferAttribute(DataType type, uint32_t divisor = 0)
            : Type(type), InstanceDivisor(divisor) {
        }
    };

    struct BufferAttributes {
        std::vector<BufferAttribute> Attributes;
        uint32_t Stride = 0;

        BufferAttributes(std::initializer_list<BufferAttribute> types)
            : Attributes(types) {
            uint32_t offset = 0;

            for (auto& attr : Attributes) {
                attr.Offset = offset;
                offset += ElementCount(attr.Type) * BaseSize(attr.Type);
            }

            Stride = offset;
        }
    };
}