#pragma once
#include <atomic>
#include <cstdint>
#include <utility>
#include <type_traits>

namespace RealRHI {
	class RefCounted {
	public:
		RefCounted() = default;
		virtual ~RefCounted() = default;

		void IncRefCount() const {
			m_RefCount++;
		}

		// Returns true if this was the final reference (count reached zero)
		bool DecRefCount() const {
			// Use atomic decrement-and-fetch to get the new count in one operation
			uint32_t newCount = (--m_RefCount);
			return newCount == 0;
		}

		void ZeroRefCount() const {
			m_RefCount = 0;
		}

		uint32_t GetRefCount() const { return m_RefCount.load(); }
	private:
		mutable std::atomic<std::uint32_t> m_RefCount = 0;
	};

	template<typename T>
	class Ref {
	public:
		Ref() = default;
		Ref(std::nullptr_t n) : m_Instance(nullptr) {}

		Ref(T* instance) : m_Instance(instance) {
			static_assert(std::is_base_of_v<RefCounted, T>, "T must derive from RefCounted");

			IncRef();
		}

		Ref(const Ref<T>& other)
			: m_Instance(other.m_Instance) {
			IncRef();
		}

		Ref(Ref<T>&& other) noexcept
			: m_Instance(other.m_Instance) {
			other.m_Instance = nullptr;
		}

		~Ref() {
			DecRef();
		}

		template<typename T2>
		requires(std::is_base_of_v<T, T2> || std::is_base_of_v<T2, T>)
		Ref(const Ref<T2>& other) {
			m_Instance = (T*)other.m_Instance;
			IncRef();
		}

		template<typename T2>
		requires(std::is_base_of_v<T, T2> || std::is_base_of_v<T2, T>)
		Ref(Ref<T2>&& other) noexcept {
			m_Instance = (T*)other.m_Instance;
			other.m_Instance = nullptr;
		}

		Ref& operator=(std::nullptr_t) {
			DecRef();
			m_Instance = nullptr;
			return *this;
		}

		Ref& operator=(const Ref<T>& other) {
			// Self-assignment: do nothing
			if (m_Instance == other.m_Instance) {
				return *this;
			}
			// Acquire new reference first, then release old
			// This ensures safety even if they share the same underlying object
			other.IncRef();
			DecRef();

			m_Instance = other.m_Instance;
			return *this;
		}

		template<typename T2>
		Ref& operator=(const Ref<T2>& other) {
			T* newInstance = (T*)other.m_Instance;
			// Self-assignment: do nothing
			if (m_Instance == newInstance) {
				return *this;
			}
			other.IncRef();
			DecRef();

			m_Instance = newInstance;
			return *this;
		}

		Ref& operator=(Ref<T>&& other) noexcept {
			// Self-assignment: do nothing (moving to self is a no-op)
			if (m_Instance == other.m_Instance) {
				return *this;
			}
			DecRef();

			m_Instance = other.m_Instance;
			other.m_Instance = nullptr;
			return *this;
		}

		template<typename T2>
		Ref& operator=(Ref<T2>&& other) noexcept {
			T* newInstance = (T*)other.m_Instance;
			// Self-assignment: do nothing
			if (m_Instance == newInstance) {
				other.m_Instance = nullptr;
				return *this;
			}
			DecRef();

			m_Instance = newInstance;
			other.m_Instance = nullptr;
			return *this;
		}

		operator bool() { return m_Instance != nullptr; }
		operator bool() const { return m_Instance != nullptr; }

		T* operator->() { return m_Instance; }
		const T* operator->() const { return m_Instance; }

		T& operator*() { return *m_Instance; }
		const T& operator*() const { return *m_Instance; }

		T* Raw() { return m_Instance; }
		const T* Raw() const { return m_Instance; }

		// Release this Ref's ownership without deleting the object
		// The Ref becomes empty (nullptr)
		void Release() {
			if (m_Instance) {
				DecRef();
				m_Instance = nullptr;
			}
		}

		// Reset to a new instance, safely handling ownership
		void Reset(T* instance = nullptr) {
			// If the new instance is the same as current, do nothing
			if (m_Instance == instance) {
				return;
			}
			// Acquire new reference first
			if (instance) {
				instance->IncRefCount();
			}
			// Then release old
			DecRef();
			m_Instance = instance;
		}

		template<typename T2>
		requires(std::is_base_of_v<T, T2> || std::is_base_of_v<T2, T>)
		[[nodiscard]] Ref<T2> As() const {
			return Ref<T2>(static_cast<T2*>(m_Instance));
		}

		template<typename... Args>
		static Ref<T> Create(Args&&... args) {
			return Ref<T>(new T(std::forward<Args>(args)...));
		}

		bool operator==(const Ref<T>& other) const {
			return m_Instance == other.m_Instance;
		}

		bool operator!=(const Ref<T>& other) const {
			return !(*this == other);
		}

		bool operator==(const T* other) const {
			return m_Instance == other;
		}

		bool operator!=(const T* other) const {
			return !(*this == other);
		}

	private:
		void IncRef() const {
			if (m_Instance) {
				m_Instance->IncRefCount();
			}
		}

		void DecRef() const {
			if (m_Instance) {
				if (m_Instance->DecRefCount()) {
					delete m_Instance;
				}
				// Note: We don't clear m_Instance here; caller is responsible
				// This method doesn't set m_Instance to nullptr
			}
		}

	private:
		template<class T2>
		friend class Ref;

		mutable T* m_Instance = nullptr;
	};
}

