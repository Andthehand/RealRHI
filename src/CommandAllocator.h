#pragma once
#include "RefCounted.h"
#include "Result.h"

namespace RealRHI {
	// Owns command-buffer allocation storage. Reset is only valid when no
	// command lists are allocated from this allocator. Lists from one allocator
	// must be recorded on one thread at a time.
	class CommandAllocator : public RefCounted {
	public:
		virtual ~CommandAllocator() = default;
		virtual Result Reset() = 0;
	};
}
