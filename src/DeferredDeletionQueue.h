#pragma once
#include "Queue.h"
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace RealRHI {
    // Holds backend destruction callbacks until every queue dependency completes.
    // Callbacks must not capture objects whose lifetime ends before Drain().
    class DeferredDeletionQueue {
    public:
        using CompletionQuery = std::function<bool(const SubmissionToken&)>;
        using CompletionWait = std::function<void(const SubmissionToken&)>;

        void Enqueue(std::vector<SubmissionToken> dependencies, std::function<void()> destroy) {
            if (!destroy) return;
            std::scoped_lock lock(m_Mutex);
            m_Entries.push_back({std::move(dependencies), std::move(destroy)});
        }

        // Does not wait for GPU execution. Callbacks run outside the mutex.
        void Process(const CompletionQuery& isComplete) {
            std::vector<std::function<void()>> ready;
            {
                std::scoped_lock lock(m_Mutex);
                auto it = m_Entries.begin();
                while (it != m_Entries.end()) {
                    bool complete = true;
                    for (const auto& token : it->dependencies) {
                        if (token.IsValid() && !isComplete(token)) {
                            complete = false;
                            break;
                        }
                    }
                    if (complete) {
                        ready.push_back(std::move(it->destroy));
                        it = m_Entries.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            for (auto& callback : ready) callback();
        }

        // Shutdown-only: waits for every recorded dependency.
        void Drain(const CompletionWait& wait, const CompletionQuery& isComplete) {
            std::vector<SubmissionToken> dependencies;
            {
                std::scoped_lock lock(m_Mutex);
                for (const auto& entry : m_Entries)
                    for (const auto& token : entry.dependencies)
                        if (token.IsValid()) dependencies.push_back(token);
            }
            for (const auto& token : dependencies) wait(token);
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
