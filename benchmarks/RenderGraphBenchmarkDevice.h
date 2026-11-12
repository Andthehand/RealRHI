#pragma once

#include "Device.h"

#include <array>
#include <stdexcept>

namespace RealRHI::Benchmarks {
	inline void Check(bool condition, const char* message) {
		if (!condition) {
			throw std::runtime_error(message);
		}
	}

	[[noreturn]] inline void Unsupported() {
		throw std::runtime_error("Render graph benchmark attempted an unsupported backend operation");
	}

	// No native memory, driver calls, or GPU work. All simulated submissions finish
	// synchronously; the graph and input fixtures own resources throughout each call.
	class BenchmarkBuffer final : public Buffer {
	public:
		explicit BenchmarkBuffer(uint64_t size)
			: m_Size(size) { }

		uint64_t GetAllocationSize() const override { return m_Size; }
		uint64_t GetSize() const override { return m_Size; }
		uint32_t GetDescriptorIndex() const override { return 0; }

		Result WriteData(const void*, uint64_t, uint64_t) override { return Result::Failed; }
		Result ReadData(void*, uint64_t, uint64_t) const override { return Result::Failed; }

	private:
		uint64_t m_Size;
	};

	class BenchmarkTexture final : public Texture {
	public:
		uint64_t GetAllocationSize() const override { return 4096; }
		uint32_t GetSampleCount() const override { return 1; }
		TextureView* GetTextureView() override { return nullptr; }
		const TextureView* GetTextureView() const override { return nullptr; }
		Result UploadData(const void*, uint32_t) override { return Result::Failed; }
	};

	class BenchmarkAllocator final : public CommandAllocator {
	public:
		explicit BenchmarkAllocator(QueueId id)
			: queue(id) { }

		Result Reset() override { return Result::Success; }
		QueueId queue;
	};

	class BenchmarkCommands final : public CommandList {
	public:
		explicit BenchmarkCommands(QueueId queue = {})
			: m_Queue(queue) { }

		Result Begin() override {
			Check(m_State == CommandListState::Initial, "Begin requires an initial command list");
			m_State = CommandListState::Recording;
			return Result::Success;
		}

		Result End() override {
			Check(m_State == CommandListState::Recording, "End requires a recording command list");
			m_State = CommandListState::Executable;
			return Result::Success;
		}

		Result Reset() override {
			m_State = CommandListState::Initial;
			return Result::Success;
		}

		CommandListState GetState() const override { return m_State; }
		QueueId GetQueueId() const override { return m_Queue; }

		void BeginDebugLabel(const char*) override { }
		void EndDebugLabel() override { }
		void InsertDebugLabel(const char*) override { }

		void RetainResource(RefCounted* resource) override {
			Check(resource != nullptr, "Unresolved retained resource");
		}
		void ClearResourceDeclarations() override { }
		void UseResource(Texture* texture) override { Check(texture != nullptr, "Unresolved texture"); }
		void UseResource(Buffer* buffer) override { Check(buffer != nullptr, "Unresolved buffer"); }
		void UseResource(TextureView*) override { Unsupported(); }
		void UseResource(Sampler*) override { Unsupported(); }

		void Barrier(const BarrierDesc& barriers) override {
			Check(m_State == CommandListState::Recording, "Barriers require a recording command list");

			for (const auto& barrier : barriers.textures) {
				Check(barrier.texture != nullptr, "Unresolved texture barrier");
			}

			for (const auto& barrier : barriers.buffers) {
				Check(barrier.buffer != nullptr, "Unresolved buffer barrier");
			}
		}

		void BeginRendering(const RenderingInfo&) override { Unsupported(); }
		void EndRendering() override { Unsupported(); }
		void SetViewport(const Viewport&) override { Unsupported(); }
		void SetScissor(const Rect&) override { Unsupported(); }
		void BindShaderProgram(Shader*) override { Unsupported(); }
		Result SetGraphicsState(const GraphicsState&) override { return Result::Failed; }
		Result SetVertexLayout(const VertexLayout&) override { return Result::Failed; }
		void SetPushConstants(const void*, uint32_t, uint32_t) override { Unsupported(); }
		void DispatchCompute(uint32_t, uint32_t, uint32_t) override { Unsupported(); }
		void DispatchComputeIndirect(Buffer*, uint64_t) override { Unsupported(); }
		void DrawIndirect(Buffer*, uint64_t, uint32_t, uint32_t) override { Unsupported(); }
		void DrawIndexedIndirect(Buffer*, uint64_t, uint32_t, uint32_t) override { Unsupported(); }
		void BindVertexBuffer(Buffer*, uint32_t, uint64_t) override { Unsupported(); }
		void BindIndexBuffer(Buffer*) override { Unsupported(); }
		void CopyBuffer(Buffer*, Buffer*, const BufferCopyRegion&) override { Unsupported(); }
		void CopyBufferToTexture(Buffer*, Texture*, const BufferTextureCopyRegion&) override { Unsupported(); }
		void CopyTextureToBuffer(Texture*, Buffer*, const BufferTextureCopyRegion&) override { Unsupported(); }
		void CopyTexture(Texture*, Texture*, const TextureCopyRegion&) override { Unsupported(); }
		void GenerateMipmaps(Texture*, const MipGenerationDesc&) override { Unsupported(); }
		void Draw(uint32_t, uint32_t, uint32_t, uint32_t) override { Unsupported(); }
		void DrawIndexed(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) override { Unsupported(); }

	private:
		QueueId m_Queue;
		CommandListState m_State = CommandListState::Initial;
	};

	class BenchmarkQueue final : public CommandQueue {
	public:
		explicit BenchmarkQueue(QueueId id)
			: m_Id(id) { }

		QueueId GetQueueId() const override { return m_Id; }

		SubmissionToken Submit(const SubmitInfo& info) override {
			for (auto* list : info.commandLists) {
				Check(list && list->GetState() == CommandListState::Executable && list->GetQueueId() == m_Id,
					"Invalid simulated command-list submission");
			}

			return {m_Id, ++m_Value};
		}

		bool IsComplete(const SubmissionToken& token) override {
			return token.queue == m_Id && token.IsValid() && token.value <= m_Value;
		}

		void Wait(const SubmissionToken& token) override { Check(IsComplete(token), "Invalid completion token"); }
		void WaitIdle() override { }

	private:
		QueueId m_Id;
		uint64_t m_Value = 0;
	};

	class BenchmarkDevice final : public Device {
	public:
		const DeviceCapabilitiesInfo& GetCapabilities() const override { return m_Capabilities; }
		TextureFormatSupport GetTextureFormatSupport(TextureFormat) const override { return {}; }
		bool SupportsVertexFormat(VertexFormat) const override { return false; }
		bool SupportsTexture(const TextureDesc&) const override { return true; }
		std::filesystem::path GetShaderDirectory() const override { return {}; }
		bool IsDebugEnabled() const override { return false; }
		DeviceStatistics GetStatistics() const override { return {}; }

		Result CreateBuffer(const BufferDesc& desc, Ref<Buffer>& out) const override {
			out = Ref<BenchmarkBuffer>::Create(desc.size);
			return Result::Success;
		}

		Result CreateTexture(const TextureDesc&, Ref<Texture>& out) const override {
			out = Ref<BenchmarkTexture>::Create();
			return Result::Success;
		}

		Result CreateCommandAllocator(QueueType role, Ref<CommandAllocator>& out) const override {
			out = Ref<BenchmarkAllocator>::Create(GetCommandQueue(role)->GetQueueId());
			return Result::Success;
		}

		Result CreateCommandList(CommandAllocator* allocator, Ref<CommandList>& out) const override {
			out = Ref<BenchmarkCommands>::Create(static_cast<BenchmarkAllocator*>(allocator)->queue);
			return Result::Success;
		}

		CommandQueue* GetGraphicsCommandQueue() const override { return &m_Queues[0]; }
		CommandQueue* GetCommandQueue(QueueType role) const override { return &m_Queues.at(static_cast<size_t>(role)); }

		Result CreateWindow(const WindowDesc&, Ref<Window>&) const override { return Result::Failed; }
		Result CreateShader(const ShaderDesc&, Ref<Shader>&) const override { return Result::Failed; }
		void ClearShaderCache() const override { }
		Result CreateSwapchain(const SwapchainDesc&, Ref<Swapchain>&) const override { return Result::Failed; }
		Result CreateTextureView(Texture*, const TextureViewDesc&, Ref<TextureView>&) const override {
			return Result::Failed;
		}
		Result CreateSampler(const SamplerDesc&, Ref<Sampler>&) const override { return Result::Failed; }
		Result CreateUploadContext(const UploadContextDesc&, std::unique_ptr<UploadContext>&) const override {
			return Result::Failed;
		}

		Result Submit(CommandList*,
			Swapchain*,
			const FrameContext&,
			SubmissionToken*,
			std::span<const SubmissionToken>) override {
			return Result::Failed;
		}

		Result ImmediateSubmit(CommandList*) const override { return Result::Failed; }
		void WaitIdle() override { }
		void DeferDeletion(std::vector<SubmissionToken>, std::function<void()> destroy) const override { destroy(); }
		void ProcessDeferredDeletions() override { }

	private:
		DeviceCapabilitiesInfo m_Capabilities;
		mutable std::array<BenchmarkQueue, 3> m_Queues{
			BenchmarkQueue({0, 0}),
			BenchmarkQueue({1, 0}),
			BenchmarkQueue({2, 0})
		};
	};
}
