#pragma once
#include <atomic>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace RealRHI {
	class RefCounted {
	public:
		RefCounted() = default;
		virtual ~RefCounted() = default;

		void IncRefCount() const { m_RefCount++; }

		// Returns true if this was the final reference (count reached zero)
		bool DecRefCount() const { return --m_RefCount == 0; }

		uint32_t GetRefCount() const { return m_RefCount.load(); }

	private:
		mutable std::atomic<std::uint32_t> m_RefCount = 0;
	};

	template <typename T>
	class Ref {
	public:
		Ref() = default;
		Ref(std::nullptr_t) { }

		Ref(T* instance)
			: m_Instance(instance) {
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

		~Ref() { DecRef(); }

		template <typename T2>
			requires(std::is_convertible_v<T2*, T*>)
		Ref(const Ref<T2>& other) {
			m_Instance = other.m_Instance;
			IncRef();
		}

		template <typename T2>
			requires(std::is_convertible_v<T2*, T*>)
		Ref(Ref<T2>&& other) noexcept {
			m_Instance = other.m_Instance;
			other.m_Instance = nullptr;
		}

		Ref& operator=(std::nullptr_t) {
			Reset();
			return *this;
		}

		Ref& operator=(const Ref<T>& other) {
			Reset(other.m_Instance);
			return *this;
		}

		template <typename T2>
			requires(std::is_convertible_v<T2*, T*>)
		Ref& operator=(const Ref<T2>& other) {
			Reset(other.m_Instance);
			return *this;
		}

		Ref& operator=(Ref<T>&& other) noexcept {
			if (this == &other) {
				return *this;
			}
			// The source may be a member of the object released below.
			T* newInstance = std::exchange(other.m_Instance, nullptr);
			DecRef();

			m_Instance = newInstance;
			return *this;
		}

		template <typename T2>
			requires(std::is_convertible_v<T2*, T*>)
		Ref& operator=(Ref<T2>&& other) noexcept {
			T* newInstance = std::exchange(other.m_Instance, nullptr);
			DecRef();

			m_Instance = newInstance;
			return *this;
		}

		operator bool() const { return m_Instance != nullptr; }

		T* operator->() { return m_Instance; }
		const T* operator->() const { return m_Instance; }

		T& operator*() { return *m_Instance; }
		const T& operator*() const { return *m_Instance; }

		T* Raw() { return m_Instance; }
		const T* Raw() const { return m_Instance; }

		// Release this reference, deleting the object if it was the last owner.
		void Release() { Reset(); }

		void Reset(T* instance = nullptr) {
			if (m_Instance == instance) {
				return;
			}
			if (instance) {
				// Retain first: releasing the old owner can destroy the incoming object.
				instance->IncRefCount();
			}
			DecRef();
			m_Instance = instance;
		}

		template <typename T2>
			requires(std::is_base_of_v<T, T2> || std::is_base_of_v<T2, T>)
		[[nodiscard]] Ref<T2> As() const {
			return Ref<T2>(static_cast<T2*>(m_Instance));
		}

		template <typename... Args>
		static Ref<T> Create(Args&&... args) {
			return Ref<T>(new T(std::forward<Args>(args)...));
		}

		bool operator==(const Ref<T>& other) const { return m_Instance == other.m_Instance; }

		bool operator!=(const Ref<T>& other) const { return !(*this == other); }

		bool operator==(const T* other) const { return m_Instance == other; }

		bool operator!=(const T* other) const { return !(*this == other); }

	private:
		void IncRef() const {
			if (m_Instance) {
				m_Instance->IncRefCount();
			}
		}

		void DecRef() const {
			if (m_Instance && m_Instance->DecRefCount()) {
				delete m_Instance;
			}
		}

	private:
		template <class T2>
		friend class Ref;

	private:
		T* m_Instance = nullptr;
	};
}
