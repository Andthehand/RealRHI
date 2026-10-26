#pragma once
#include "Queue.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <mutex>
#include <utility>
#include <vector>

namespace RealRHI {
	// Callbacks must not outlive the device objects they use.
	class DeferredDeletionQueue {
	private:
		struct Entry {
			std::vector<SubmissionToken> dependencies;
			std::function<void()> destroy;
		};

	public:
		using CompletionQuery = std::function<bool(const SubmissionToken&)>;
		using CompletionWait = std::function<void(const SubmissionToken&)>;

	public:
		void Enqueue(std::vector<SubmissionToken> dependencies, std::function<void()> destroy) {
			if (!destroy) {
				return;
			}

			std::scoped_lock lock(m_Mutex);
			m_Entries.push_back({std::move(dependencies), std::move(destroy)});
		}

		void Process(const CompletionQuery& isComplete) {
			std::vector<Entry> pending;
			{
				std::scoped_lock lock(m_Mutex);
				pending.swap(m_Entries);
			}

			// Completion queries can retire resources and enqueue more cleanup too.
			// Process a snapshot, with all external callbacks outside the queue lock.
			std::vector<std::function<void()>> ready;
			std::erase_if(pending, [&](Entry& entry) {
				const bool completed = std::all_of(entry.dependencies.begin(), entry.dependencies.end(),
					[&](const SubmissionToken& token) { return !token.IsValid() || isComplete(token); });
				if (completed) {
					ready.push_back(std::move(entry.destroy));
				}
				return completed;
			});
			if (!pending.empty()) {
				std::scoped_lock lock(m_Mutex);
				m_Entries.insert(m_Entries.end(),
					std::make_move_iterator(pending.begin()), std::make_move_iterator(pending.end()));
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
		mutable std::mutex m_Mutex;
		std::vector<Entry> m_Entries;
	};
}
