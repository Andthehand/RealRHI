#pragma once
#include "RefCounted.h"

#include <slang/slang.h>
#include <slang/slang-com-ptr.h>

#include <string>
#include <vector>

namespace RealRHI {
	struct EntryPoint {
		std::string entryPointName;
		SlangStage stage;
	};

	enum ScalarType {
		None = SLANG_SCALAR_TYPE_NONE,
		Void = SLANG_SCALAR_TYPE_VOID,
		Bool = SLANG_SCALAR_TYPE_BOOL,
		Int32 = SLANG_SCALAR_TYPE_INT32,
		UInt32 = SLANG_SCALAR_TYPE_UINT32,
		Int64 = SLANG_SCALAR_TYPE_INT64,
		UInt64 = SLANG_SCALAR_TYPE_UINT64,
		Float16 = SLANG_SCALAR_TYPE_FLOAT16,
		Float32 = SLANG_SCALAR_TYPE_FLOAT32,
		Float64 = SLANG_SCALAR_TYPE_FLOAT64,
		Int8 = SLANG_SCALAR_TYPE_INT8,
		UInt8 = SLANG_SCALAR_TYPE_UINT8,
		Int16 = SLANG_SCALAR_TYPE_INT16,
		UInt16 = SLANG_SCALAR_TYPE_UINT16,

		// My Types
		Vec2,
		Vec3,
		Vec4
	};

	struct BufferAttribute {
		ScalarType type;
		uint32_t offset;
	};

	struct BufferLayout {
		std::vector<BufferAttribute> attributes;
		uint32_t stride;
	};

	class Shader : public RefCounted {
	public:
		virtual ~Shader() = default;
	};
}