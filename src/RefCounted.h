#pragma once
#include <atomic>

namespace RealRHI {
	class RefCounted {
	public:
		RefCounted() = default;
		virtual ~RefCounted() = default;

		void IncRefCount() const {
			m_RefCount++;
		}

		void DecRefCount() const {
			m_RefCount--;
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

		~Ref() {
			DecRef();
		}

		template<typename T2>
		requires(std::is_base_of_v<T, T2> || std::is_base_of_v<T2, T>)
		Ref(const Ref<T2>& other) {
			m_Instance = (T*)other.m_Instance;
			IncRef();
		}

		Ref& operator=(std::nullptr_t) {
			DecRef();
			m_Instance = nullptr;
			return *this;
		}

		Ref& operator=(const Ref<T>& other) {
			other.IncRef();
			DecRef();

			m_Instance = other.m_Instance;
			return *this;
		}

		template<typename T2>
		Ref& operator=(const Ref<T2>& other) {
			other.IncRef();
			DecRef();

			m_Instance = other.m_Instance;
			return *this;
		}

		template<typename T2>
		Ref& operator=(Ref<T2>&& other) {
			DecRef();

			m_Instance = (T*)other.m_Instance;
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

		void Release() {
			delete m_Instance;
			m_Instance->ZeroRefCount();
		}

		void Reset(T* instance = nullptr) {
			DecRef();
			m_Instance = instance;
		}

		template<typename T2>
		requires(std::is_base_of_v<T, T2> || std::is_base_of_v<T2, T>)
		[[nodiscard]] Ref<T2> As() const {
			return Ref<T2>(*this);
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
	private:
		void IncRef() {
			if (m_Instance) {
				m_Instance->IncRefCount();
			}
		}

		void DecRef() {
			if (m_Instance) {
				m_Instance->DecRefCount();
				
				if (m_Instance->GetRefCount() == 0) {
					delete m_Instance;
					m_Instance = nullptr;
				}
			}
		}
	private:
		template<class T2>
		friend class Ref;

		mutable T* m_Instance;
	};
}
