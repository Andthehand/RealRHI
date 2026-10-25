#pragma once
#include "BufferDesc.h"
#include "CommandList.h"
#include "DeviceDesc.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace RealRHI {
	class Device;
	struct RenderGraphResource {
		uint32_t index = UINT32_MAX;
		uint64_t graph = 0;
	};

	struct RenderGraphPass {
		uint32_t index = UINT32_MAX;
		uint64_t graph = 0;
	};

	enum class RenderGraphAccess {
		Read,
		Write,
		ReadWrite
	};

	struct RenderGraphUse {
		RenderGraphResource resource;
		ResourceState state;
		TextureLayout layout = TextureLayout::Undefined;
		RenderGraphAccess access = RenderGraphAccess::Read;
	};

	struct RenderGraphPassDesc {
		std::string name;
		std::vector<RenderGraphUse> resources;
		std::vector<RenderGraphPass> dependsOn;
	};

	struct RenderGraphStep {
		std::string name;
		BarrierDesc barriers;
	};

	// A small layer above CommandList: all passes record into one list on one queue.
	// Serialize building/recording; callbacks must not mutate the graph.
	// Resource versions follow pass insertion order. Explicit dependencies can reorder
	// independent passes. Imports must already belong to that queue; acquire ownership
	// and wait for external submissions before using the graph.
	class RenderGraph {
	public:
		explicit RenderGraph(DebugCallback callback = DefaultDebugCallback);
		RenderGraph(const RenderGraph&) = delete;
		RenderGraph& operator=(const RenderGraph&) = delete;

		// Deferred, uninitialized resources covering the complete buffer/image. Names
		// are copied; initialize data in a Write pass rather than BufferDesc::initialData.
		RenderGraphResource CreateTexture(const TextureDesc& desc);
		RenderGraphResource CreateBuffer(const BufferDesc& desc);
		// Borrowed until the next Execute/Reset/destruction; retain a Ref to keep an
		// output externally. Created resources resolve inside callbacks and after Execute.
		Texture* GetTexture(RenderGraphResource resource);
		Buffer* GetBuffer(RenderGraphResource resource);

		// Import non-overlapping ranges with their actual state at graph entry.
		// Undefined textures need a Write pass before reading. Imported buffer contents
		// are supplied by the caller. A zero buffer size selects the remaining bytes.
		// Texture ranges must fit the underlying image; the RHI validates their extents.
		RenderGraphResource ImportTexture(Texture* texture,
			TextureLayout layout,
			ResourceState state = {},
			TextureSubresourceRange subresources = {});
		RenderGraphResource ImportBuffer(Buffer* buffer,
			ResourceState state = {},
			uint64_t offset = 0,
			uint64_t size = 0);
		void SetFinalState(RenderGraphResource resource,
			ResourceState state,
			TextureLayout layout = TextureLayout::Undefined);

		// Callbacks record complete passes, beginning/ending their own rendering scopes.
		// Imported ranges must enter and leave the callback in the declared state/layout.
		// Declare every graph-managed resource here. Callbacks may also access resources
		// whose states/dependencies are managed externally. UseResource belongs in each
		// callback: Execute clears prior declarations at pass boundaries and on exit,
		// retaining their lifetimes through CommandList reset as usual.
		RenderGraphPass AddPass(RenderGraphPassDesc desc, std::function<void(CommandList&)> record);
		void AddDependency(RenderGraphPass pass, RenderGraphPass prerequisite);

		// Compile validates the entire graph without recording anything. Plans remain
		// valid until the graph changes; rebuild if external resource states change.
		// Created-resource barrier pointers resolve during Execute. Compile allocates nothing.
		bool Compile(std::string& error);
		const std::vector<RenderGraphStep>& GetExecutionPlan() const { return m_Plan; }
		const BarrierDesc& GetFinalBarriers() const { return m_FinalBarriers; }
		// Compile, allocate fresh resources, and record into an active list outside
		// rendering. Validation/allocation failures return before recording any passes.
		Result Execute(Device& device, CommandList& commands);
		void Reset();

	private:
		struct Resource {
			std::optional<TextureDesc> textureDesc;
			std::optional<BufferDesc> bufferDesc;
			std::string debugName;
			Ref<Texture> texture;
			Ref<Buffer> buffer;
			TextureSubresourceRange subresources;
			uint64_t offset = 0;
			uint64_t size = 0;
			ResourceState initialState;
			TextureLayout initialLayout = TextureLayout::Undefined;
			bool hasFinalState = false;
			ResourceState finalState;
			TextureLayout finalLayout = TextureLayout::Undefined;
		};
		struct Pass {
			RenderGraphPassDesc desc;
			std::function<void(CommandList&)> record;
		};
		struct Dependency {
			RenderGraphPass pass;
			RenderGraphPass prerequisite;
		};
		struct TrackedState {
			ResourceState state;
			TextureLayout layout = TextureLayout::Undefined;
			bool used = false;
			bool initialized = false;
		};

	private:
		static bool IsTexture(const Resource& resource);
		static bool IsCreated(const Resource& resource);
		void ReleaseCreatedResources();
		Result AllocateResources(Device& device);
		bool IsValid(RenderGraphResource resource) const;
		bool IsValid(RenderGraphPass pass) const;
		bool ValidateResources(std::string& error) const;
		bool ValidatePasses(std::string& error) const;
		bool BuildOrder(std::string& error);
		bool BuildBarriers(std::string& error);
		void Transition(uint32_t resource,
			ResourceState after,
			TextureLayout layout,
			TrackedState& before,
			BarrierDesc& barriers);
		void Invalidate();
		void Report(const std::string& error) const;

	private:
		DebugCallback m_Callback;
		uint64_t m_Id = 0;
		std::vector<Resource> m_Resources;
		std::vector<Pass> m_Passes;
		std::vector<Dependency> m_Dependencies;
		std::vector<uint32_t> m_Order;
		std::vector<RenderGraphStep> m_Plan;
		BarrierDesc m_FinalBarriers;
	};
}
