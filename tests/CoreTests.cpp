#include "DeferredDeletionQueue.h"
#include "RefCounted.h"

#include <cstdlib>
#include <iostream>
#include <type_traits>

namespace {
	struct Base : RealRHI::RefCounted {
		inline static uint32_t s_Destroyed = 0;

	public:
		~Base() override { ++s_Destroyed; }
	};
	struct Derived : Base { };
	struct Unrelated : RealRHI::RefCounted { };

	void Expect(bool condition, const char* message) {
		if (!condition) {
			std::cerr << "Test failed: " << message << '\n';
			std::exit(1);
		}
	}
}

static_assert(std::is_constructible_v<RealRHI::Ref<Base>, RealRHI::Ref<Derived>>);
static_assert(!std::is_constructible_v<RealRHI::Ref<Derived>, RealRHI::Ref<Base>>);
static_assert(!std::is_assignable_v<RealRHI::Ref<Derived>&, RealRHI::Ref<Base>>);
static_assert(!std::is_assignable_v<RealRHI::Ref<Base>&, RealRHI::Ref<Unrelated>>);

int main() {
	{
		auto derived = RealRHI::Ref<Derived>::Create();
		RealRHI::Ref<Base> base = derived;
		base = std::move(derived);
		Expect(!derived && base->GetRefCount() == 1, "Converting alias move consumes exactly one reference");
		RealRHI::Ref<Base> alias = base;
		base = std::move(alias);
		Expect(!alias && base->GetRefCount() == 1, "Same-type alias move consumes exactly one reference");
		base = std::move(base);
		Expect(base && base->GetRefCount() == 1, "Self move preserves ownership");
		base.Release();
		Expect(!base && Base::s_Destroyed == 1, "Release destroys the final owner");
	}
	{
		auto first = RealRHI::Ref<Derived>::Create();
		auto second = RealRHI::Ref<Derived>::Create();
		RealRHI::Ref<Base> base = first;
		base = std::move(second);
		Expect(!second && first->GetRefCount() == 1 && base->GetRefCount() == 1,
			"Converting move releases the old object and transfers the new one");
		base.Reset(base.Raw());
		Expect(base->GetRefCount() == 1, "Reset to the current object preserves ownership");
	}
	Expect(Base::s_Destroyed == 3, "Each owned object is destroyed exactly once");
	{
		auto derived = RealRHI::Ref<Derived>::Create();
		RealRHI::Ref<Base> base;
		base = derived;
		Expect(base.Raw() == derived.Raw() && base->GetRefCount() == 2, "Converting copy shares ownership");
		base = derived;
		base = base;
		Expect(base->GetRefCount() == 2, "Alias and self copy preserve the reference count");
		RealRHI::Ref<Base> other = RealRHI::Ref<Base>::Create();
		other = base;
		Expect(Base::s_Destroyed == 4 && base->GetRefCount() == 3,
			"Copy assignment releases the old object and retains the replacement");
		other = nullptr;
		base.Reset();
		Expect(derived->GetRefCount() == 1, "Null assignment and Reset release their references");
		derived.Release();
		derived.Release();
		Expect(Base::s_Destroyed == 5, "Repeated release destroys the object exactly once");
	}
	{
		RealRHI::DeferredDeletionQueue deletions;
		const RealRHI::SubmissionToken first{.queue = {0, 0}, .value = 1};
		const RealRHI::SubmissionToken second{.queue = {1, 0}, .value = 2};
		uint32_t destroyed = 0;
		deletions.Enqueue({first, second}, [&] {
			++destroyed;
		});
		deletions.Process([&](const auto& token) {
			return token == first;
		});
		Expect(destroyed == 0, "Deletion waits for every queue dependency");
		deletions.Process([](const auto&) {
			return true;
		});
		deletions.Process([](const auto&) {
			return true;
		});
		Expect(destroyed == 1 && deletions.PendingCount() == 0, "Deletion runs exactly once");
	}
	std::cout << "CoreTests passed\n";
}
