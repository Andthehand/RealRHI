#pragma once
#include "Queue.h"

#include <cstddef>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace RealRHI {
	// Callbacks must not outlive the device objects they use.
	class DeferredDeletionQueue {
	public:
		using CompletionQuery = std::function<bool(const SubmissionToken&)>;
		using CompletionWait = std::function<void(const SubmissionToken&)>;

		void Enqueue(std::vector<SubmissionToken> dependencies, std::function<void()> destroy) {
			if (!destroy) {
				return;
			}

			std::scoped_lock lock(m_Mutex);
			m_Entries.push_back({ std::move(dependencies), std::move(destroy) });
		}

		void Process(const CompletionQuery& isComplete) {
			std::vector<std::function<void()>> ready;
			{
				std::scoped_lock lock(m_Mutex);
				auto entry = m_Entries.begin();
				while (entry != m_Entries.end()) {
					bool completed = true;
					for (const SubmissionToken& token : entry->dependencies) {
						if (token.IsValid() && !isComplete(token)) {
							completed = false;
							break;
						}
					}

					if (completed) {
						ready.push_back(std::move(entry->destroy));
						entry = m_Entries.erase(entry);
					}
					else {
						++entry;
					}
				}
			}

			// Callbacks may destroy objects that acquire other locks.
			for (auto& callback : ready) {
				callback();
			}
		}

		void Drain(const CompletionWait& wait, const CompletionQuery& isComplete) {
			std::vector<SubmissionToken> dependencies;
			{
				std::scoped_lock lock(m_Mutex);
				for (const auto& entry : m_Entries) {
					for (const SubmissionToken& token : entry.dependencies) {
						if (token.IsValid()) {
							dependencies.push_back(token);
						}
					}
				}
			}

			for (const SubmissionToken& token : dependencies) {
				wait(token);
			}
			Process(isComplete);
		}

		size_t PendingCount() const {
			std::scoped_lock lock(m_Mutex);
			return m_Entries.size();
		}

	private:
		struct Entry {
			std::vector<SubmissionToken> dependencies;
			std::function<void()> destroy;
		};

		mutable std::mutex m_Mutex;
		std::vector<Entry> m_Entries;
	};
}
